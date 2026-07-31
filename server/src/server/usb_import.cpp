#include <server/usb_import.hpp>

#include <arpa/inet.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <helpers/logger.hpp>
#include <mutex>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <server/fake_udev.hpp>
#include <server/usb_discovery.hpp>
#include <server/usb_handshake.hpp>
#include <server/usb_import_plan.hpp>
#include <server/usb_transport.hpp>
#include <server/usb_tunnel.hpp>
#include <server/usbip_proto.hpp>
#include <server/vhci.hpp>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <thread>
#include <unistd.h>

namespace usbip {

namespace {

std::string env_or(const char *k, const std::string &def) {
  const char *v = std::getenv(k);
  return v && *v ? std::string(v) : def;
}

// Blocking read of exactly n bytes. USB/IP replies are fixed-size, so a short read is a protocol
// error, not something to paper over.
bool fd_read_exact(int fd, void *buf, std::size_t n) {
  auto *p = static_cast<std::uint8_t *>(buf);
  std::size_t got = 0;
  while (got < n) {
    auto r = ::recv(fd, p + got, n - got, 0);
    if (r <= 0)
      return false;
    got += static_cast<std::size_t>(r);
  }
  return true;
}

bool fd_write_all(int fd, const std::string &s) {
  std::size_t sent = 0;
  while (sent < s.size()) {
    auto r = ::send(fd, s.data() + sent, s.size() - sent, MSG_NOSIGNAL);
    if (r <= 0)
      return false;
    sent += static_cast<std::size_t>(r);
  }
  return true;
}

int tcp_connect(const std::string &host, const std::string &port, int timeout_ms) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo *res = nullptr;
  if (::getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0)
    return -1;

  int fd = -1;
  for (auto *ai = res; ai; ai = ai->ai_next) {
    fd = ::socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
    if (fd < 0)
      continue;
    timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
      break;
    ::close(fd);
    fd = -1;
  }
  ::freeaddrinfo(res);
  if (fd < 0)
    return -1;

  // Every URB is its own small segment; Nagle would add up to 40ms of input lag and it would
  // present as "the video feels laggy".
  int one = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
  int idle = 3, intvl = 2, cnt = 3;
  ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
  ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
  ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
  // The one that actually saves us: the keepalive timer does not run while data is unacked, which
  // is exactly the "client vanished mid-transfer" case. Without this the kernel spends ~13-15min
  // in tcp_retries2 before the device detaches.
  unsigned user_timeout = 8000;
  ::setsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &user_timeout, sizeof(user_timeout));

  // Clear the handshake timeouts: from here the socket belongs to vhci, which blocks on recv
  // indefinitely by design.
  timeval zero{0, 0};
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &zero, sizeof(zero));
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &zero, sizeof(zero));
  return fd;
}

// The DIRECT transport: a plain TCP connection to a usbipd. No auth, no encryption -- this is the
// debug escape hatch that exercises the whole server side with no client changes at all, and it
// belongs on loopback or a trusted LAN only.
class DirectChannel : public Channel {
public:
  explicit DirectChannel(int fd) : fd_(fd) {}
  ~DirectChannel() override {
    if (fd_ >= 0)
      ::close(fd_);
  }

  bool read_exact(void *buf, std::size_t n) override { return fd_read_exact(fd_, buf, n); }
  bool write_all(std::string_view s) override { return fd_write_all(fd_, std::string(s)); }

  // Nothing to pump: this socket already carries exactly the bytes the kernel wants.
  int into_kernel_fd() override {
    int fd = fd_;
    fd_ = -1;
    return fd;
  }

private:
  int fd_;
};

class DirectTransport : public Transport {
public:
  DirectTransport(std::string host, std::string port, std::vector<std::string> busids)
      : host_(std::move(host)), port_(std::move(port)), busids_(std::move(busids)) {}

  std::string describe() const override { return "direct " + host_ + ":" + port_; }
  std::vector<std::string> busids() override { return busids_; }

  std::unique_ptr<Channel> open(const std::string &, int timeout_ms) override {
    int fd = tcp_connect(host_, port_, timeout_ms);
    if (fd < 0) {
      logs::log(logs::warning, "[USBIP] cannot reach exporter {}:{}", host_, port_);
      return nullptr;
    }
    return std::make_unique<DirectChannel>(fd);
  }

private:
  std::string host_, port_;
  std::vector<std::string> busids_;
};

// Copy the host's already-computed udev entry. udevd ran every rule against this device, so its
// properties (ID_VENDOR_ID, ID_SERIAL_SHORT, ...) are real; synthesizing them would be guesswork.
std::string host_hwdb_entry(const std::string &host_dir, unsigned major, unsigned minor) {
  auto path = host_dir + "/c" + std::to_string(major) + ":" + std::to_string(minor);
  std::ifstream f(path);
  if (!f)
    return {};
  return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

struct Attached {
  std::string busid;      // on the exporter
  std::string local_busid; // on our side
  int port = -1;
  std::vector<std::string> created_nodes;
  std::vector<fake_udev::Device> announced;
};

} // namespace

struct ImportManager::Impl {
  std::mutex mtx;
  bool enabled = false;
  bool vhci_ok = false;
  std::string direct_host, direct_port;
  TunnelServer *tunnel = nullptr;

  // The tunnel wins when a client is connected: it is authenticated, encrypted, and its device
  // list came from the user's own filter UI. DIRECT is the fallback and the debug path.
  std::unique_ptr<Transport> pick_transport() {
    if (tunnel && tunnel->has_client())
      if (auto t = tunnel->transport())
        return t;
    if (!direct_host.empty())
      return std::make_unique<DirectTransport>(direct_host, direct_port, busids);
    return nullptr;
  }
  std::vector<std::string> busids; // what to import
  std::string host_udev_dir;
  std::string port_record;         // where we note attached ports, for reap_stale
  int max_devices = 4;
  std::size_t session = 0;
  std::vector<Attached> attached;

  void record_ports() {
    std::ofstream f(port_record, std::ios::trunc);
    if (!f)
      return;
    for (const auto &a : attached)
      f << a.port << "\n";
  }

  // Create the device nodes in the container's PRIVATE /dev, then chown to the app user. We
  // mknod rather than bind-mount precisely so chown cannot reach the host's devtmpfs.
  void make_nodes(const DiscoveredDevice &dev, Attached &att) {
    uid_t uid = static_cast<uid_t>(std::atoi(env_or("STEAM_STREAM_RUN_UID", "1000").c_str()));
    gid_t gid = static_cast<gid_t>(std::atoi(env_or("STEAM_STREAM_RUN_GID", "1000").c_str()));

    for (const auto &spec : plan_nodes(dev)) {
      std::error_code ec;
      std::filesystem::create_directories(std::filesystem::path(spec.path).parent_path(), ec);
      ::unlink(spec.path.c_str());
      if (::mknod(spec.path.c_str(), S_IFCHR | 0660, ::makedev(spec.major, spec.minor)) != 0) {
        logs::log(logs::warning, "[USBIP] mknod {} (c{}:{}) failed: {}", spec.path, spec.major,
                  spec.minor, std::strerror(errno));
        continue;
      }
      // mknod is subject to umask, so set the mode explicitly. Steam needs read+WRITE on hidraw --
      // it sends feature reports.
      ::chmod(spec.path.c_str(), 0660);
      if (::chown(spec.path.c_str(), uid, gid) != 0)
        logs::log(logs::warning, "[USBIP] chown {} failed: {}", spec.path, std::strerror(errno));
      att.created_nodes.push_back(spec.path);
    }
  }

  void announce(const DiscoveredDevice &dev, Attached &att) {
    for (auto d : plan_announcements(dev)) {
      if (d.has_node())
        d.hwdb_override = host_hwdb_entry(host_udev_dir, d.major, d.minor);
      fake_udev::plug(d);
      att.announced.push_back(std::move(d));
    }
  }

  void withdraw(Attached &att) {
    // Reverse order: children go away before their parent, rather than a parent vanishing under
    // its children.
    for (auto it = att.announced.rbegin(); it != att.announced.rend(); ++it)
      fake_udev::unplug(*it);
    att.announced.clear();
    for (const auto &n : att.created_nodes) {
      ::unlink(n.c_str());
      // Leave no empty /dev/bus/usb/<bus> behind; rmdir is a no-op unless it is now empty.
      std::error_code ec;
      auto parent = std::filesystem::path(n).parent_path();
      if (parent.string().rfind("/dev/bus/usb/", 0) == 0)
        std::filesystem::remove(parent, ec);
    }
    att.created_nodes.clear();
  }

  bool import_one(Transport &tr, const std::string &busid, int timeout_ms) {
    auto ch = tr.open(busid, timeout_ms);
    if (!ch)
      return false;

    UsbDevice udev{};
    auto hs = do_import_handshake(*ch, busid, udev);
    if (hs.result != HandshakeResult::OK) {
      if (hs.result == HandshakeResult::REFUSED)
        logs::log(logs::warning, "[USBIP] exporter refused {} (status {}) -- is it bound? `usbip bind -b {}`",
                  busid, hs.status, busid);
      else if (hs.result == HandshakeResult::BAD_VERSION)
        logs::log(logs::warning, "[USBIP] exporter speaks version 0x{:04x}, we speak 0x{:04x}", hs.peer_version,
                  kVersion);
      else
        logs::log(logs::warning, "[USBIP] import of {} failed: {}", busid, describe(hs.result));
      return false;
    }

    auto rows = vhci::read_status();
    if (!rows) {
      logs::log(logs::warning, "[USBIP] cannot read vhci status -- is vhci_hcd loaded?");
      return false;
    }
    auto port = vhci::find_free_port(*rows, udev.speed);
    if (!port) {
      logs::log(logs::warning, "[USBIP] no free vhci {} port (8 max)",
                vhci::clamp_speed(udev.speed) >= SPEED_SUPER ? "SuperSpeed" : "high-speed");
      return false;
    }

    // On DIRECT this is the socket itself; on the tunnel it is a socketpair with a TLS pump on the
    // other end, because a TLS stream cannot be handed to the kernel.
    int fd = ch->into_kernel_fd();
    if (fd < 0)
      return false;

    if (!vhci::attach(*port, fd, udev.devid(), udev.speed)) {
      ::close(fd);
      return false;
    }
    // The kernel took its own reference via sockfd_lookup; we must drop ours or the socket never
    // closes when vhci is done with it. This is exactly what the usbip CLI does before exiting.
    ::close(fd);

    auto local = vhci::wait_local_busid(*port, 2000);
    if (!local) {
      logs::log(logs::warning, "[USBIP] {} attached on port {} but never enumerated", busid, *port);
      vhci::detach(*port);
      return false;
    }

    // Enumeration continues asynchronously after local_busid appears: interfaces bind, then
    // hidraw nodes materialize. Poll rather than assume one read is enough.
    RealSysfs fs;
    DiscoveredDevice dd;
    for (int i = 0; i < 100; i++) {
      if (discover(fs, "/sys/bus/usb/devices", *local, dd) && dd.complete)
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (!dd.complete)
      logs::log(logs::warning,
                "[USBIP] {} (local {}) did not fully enumerate ({} of {} interfaces bound); "
                "announcing what exists",
                busid, *local, dd.interfaces.size(), dd.num_interfaces);

    Attached att;
    att.busid = busid;
    att.local_busid = *local;
    att.port = *port;
    make_nodes(dd, att);
    announce(dd, att); // nodes first, THEN the uevents -- SDL opens what we announce
    attached.push_back(std::move(att));
    record_ports();

    logs::log(logs::info,
              "[USBIP] imported {} -> {} on vhci port {} ({:04x}:{:04x}, {} interfaces, {} nodes)",
              busid, *local, *port, udev.id_vendor, udev.id_product, dd.interfaces.size(),
              attached.back().created_nodes.size());
    return true;
  }

  void detach_all() {
    for (auto &att : attached) {
      withdraw(att); // announce the removal before the device actually goes, so it reads as a
                     // clean unplug rather than an I/O error
      vhci::detach(att.port);
      // Block until the kernel has really torn it down, so the next attach cannot land on a port
      // that is still transitioning.
      vhci::wait_port_free(att.port, 2000);
      logs::log(logs::info, "[USBIP] detached {} (port {})", att.busid, att.port);
    }
    attached.clear();
    record_ports();
  }
};

ImportManager::ImportManager() : impl_(std::make_unique<Impl>()) {}
ImportManager::~ImportManager() = default;

ImportManager &ImportManager::instance() {
  static ImportManager m;
  return m;
}

void ImportManager::init() {
  auto &I = *impl_;
  std::lock_guard<std::mutex> lk(I.mtx);

  I.host_udev_dir = env_or("STEAM_STREAM_HOST_UDEV_DATA", "/run/host-udev/data");
  I.port_record = env_or("STEAM_STREAM_STATE_DIR", "/var/lib/steam-stream") + "/usbip-ports";
  I.max_devices = std::atoi(env_or("STEAM_STREAM_USBIP_MAX_DEVICES", "4").c_str());

  auto direct = env_or("STEAM_STREAM_USBIP_DIRECT", "");
  if (!direct.empty()) {
    auto colon = direct.rfind(':');
    if (colon == std::string::npos) {
      I.direct_host = direct;
      I.direct_port = std::to_string(kDefaultPort);
    } else {
      I.direct_host = direct.substr(0, colon);
      I.direct_port = direct.substr(colon + 1);
    }
  }

  // Comma-separated busids on the EXPORTER, e.g. "1-1,1-2".
  auto list = env_or("STEAM_STREAM_USBIP_BUSIDS", "");
  std::size_t pos = 0;
  while (pos < list.size()) {
    auto comma = list.find(',', pos);
    auto tok = list.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
    if (!tok.empty())
      I.busids.push_back(tok);
    if (comma == std::string::npos)
      break;
    pos = comma + 1;
  }

  // vhci is the one hard requirement. Without it there is nowhere to put an imported device, and
  // no transport can substitute.
  I.vhci_ok = vhci::available();
  I.enabled = I.vhci_ok;

  if (!I.vhci_ok) {
    logs::log(logs::info, "[USBIP] disabled: vhci_hcd unavailable or {}/attach not writable "
                          "(modprobe vhci-hcd; entrypoint remounts /sys)",
              vhci::kSysfsBase);
    return;
  }
  if (!I.direct_host.empty() && !I.busids.empty())
    logs::log(logs::info, "[USBIP] vhci ready; DIRECT exporter {}:{}, busids [{}]", I.direct_host,
              I.direct_port, list);
  else if (!I.direct_host.empty())
    logs::log(logs::info, "[USBIP] vhci ready, DIRECT exporter {}:{}; set STEAM_STREAM_USBIP_BUSIDS to use it",
              I.direct_host, I.direct_port);
  else
    logs::log(logs::info, "[USBIP] vhci ready; waiting for a tunnel client to offer devices");
}

void ImportManager::set_tunnel(TunnelServer *t) {
  auto &I = *impl_;
  std::lock_guard<std::mutex> lk(I.mtx);
  I.tunnel = t;
}

bool ImportManager::enabled() const { return impl_->enabled; }

void ImportManager::reap_stale() {
  auto &I = *impl_;
  std::lock_guard<std::mutex> lk(I.mtx);
  std::ifstream f(I.port_record);
  if (!f)
    return;
  auto rows = vhci::read_status();
  int reaped = 0;
  int port;
  while (f >> port) {
    // Only ports we recorded, and only if still in use -- never blow away someone else's attach.
    if (!rows)
      continue;
    for (const auto &r : *rows)
      if (r.port == port && !r.is_free()) {
        vhci::detach(port);
        reaped++;
      }
  }
  if (reaped)
    logs::log(logs::info, "[USBIP] reaped {} stale vhci port(s) from a previous run", reaped);
  std::ofstream(I.port_record, std::ios::trunc).close();
}

void ImportManager::attach_for_session(std::size_t session_id, int timeout_ms) {
  auto &I = *impl_;
  std::lock_guard<std::mutex> lk(I.mtx);
  if (!I.enabled)
    return;
  if (!I.attached.empty() && I.session == session_id)
    return; // already imported for this session

  if (!I.attached.empty())
    I.detach_all();
  I.session = session_id;

  auto tr = I.pick_transport();
  if (!tr) {
    logs::log(logs::debug, "[USBIP] session {}: no transport (no tunnel client, no DIRECT)", session_id);
    return;
  }
  auto want = tr->busids();
  if (want.empty()) {
    logs::log(logs::info, "[USBIP] session {}: {} offers no devices", session_id, tr->describe());
    return;
  }

  int n = 0;
  for (const auto &busid : want) {
    if (n >= I.max_devices) {
      logs::log(logs::warning, "[USBIP] device cap {} reached; skipping {}", I.max_devices, busid);
      break;
    }
    // A controller that does not arrive must never fail the stream.
    if (I.import_one(*tr, busid, timeout_ms))
      n++;
  }
  logs::log(n ? logs::info : logs::warning, "[USBIP] session {}: {} of {} device(s) imported via {}",
            session_id, n, want.size(), tr->describe());
}

void ImportManager::reconcile_session(std::size_t session_id) {
  auto &I = *impl_;
  std::lock_guard<std::mutex> lk(I.mtx);
  if (!I.enabled || I.session != session_id)
    return;

  auto rows = vhci::read_status();
  if (!rows)
    return;

  std::vector<std::string> lost;
  for (auto it = I.attached.begin(); it != I.attached.end();) {
    bool live = false;
    for (const auto &r : *rows)
      if (r.port == it->port && !r.is_free())
        live = true;
    if (live) {
      ++it; // still attached -- re-plugging would look like a disconnect to the game
      continue;
    }
    lost.push_back(it->busid);
    I.withdraw(*it);
    it = I.attached.erase(it);
  }
  if (lost.empty())
    return;

  auto tr = I.pick_transport();
  if (!tr) {
    logs::log(logs::warning, "[USBIP] resume: {} device(s) lost but no transport to re-import them",
              lost.size());
    return;
  }
  logs::log(logs::info, "[USBIP] resume: re-importing {} device(s) whose link died", lost.size());
  for (const auto &busid : lost)
    I.import_one(*tr, busid, 3000);
  I.record_ports();
}

void ImportManager::detach_session(std::size_t session_id) {
  auto &I = *impl_;
  std::lock_guard<std::mutex> lk(I.mtx);
  if (I.attached.empty() || I.session != session_id)
    return;
  I.detach_all();
}

} // namespace usbip
