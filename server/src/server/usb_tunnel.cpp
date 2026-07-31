#include <server/usb_tunnel.hpp>

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <helpers/logger.hpp>
#include <map>
#include <mutex>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace usbip {

namespace {

constexpr int kHandshakeTimeoutMs = 5000;
constexpr int kControlPollMs = 20;

std::string ssl_err() {
  auto e = ERR_get_error();
  if (!e)
    return "no TLS error";
  char buf[256];
  ERR_error_string_n(e, buf, sizeof(buf));
  ERR_clear_error();
  return buf;
}

void set_nonblocking(int fd, bool on) {
  int fl = ::fcntl(fd, F_GETFL, 0);
  if (fl < 0)
    return;
  ::fcntl(fd, F_SETFL, on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
}

void set_timeout(int fd, int ms) {
  timeval tv{ms / 1000, (ms % 1000) * 1000};
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

// Same rationale as the DIRECT socket: every URB is its own small segment, and Nagle would add up
// to 40ms of input lag that presents as "the video feels laggy". TCP_USER_TIMEOUT is the one that
// actually saves us, because the keepalive timer does not run while data is unacked -- exactly the
// client-vanished-mid-transfer case.
void set_stream_opts(int fd) {
  int one = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
  int idle = 3, intvl = 2, cnt = 3;
  ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
  ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
  ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
  unsigned user_timeout = 8000;
  ::setsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &user_timeout, sizeof(user_timeout));
}

std::int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

bool ssl_read_exact(SSL *ssl, void *buf, std::size_t n) {
  auto *p = static_cast<std::uint8_t *>(buf);
  std::size_t got = 0;
  while (got < n) {
    int r = SSL_read(ssl, p + got, static_cast<int>(n - got));
    if (r <= 0)
      return false;
    got += static_cast<std::size_t>(r);
  }
  return true;
}

bool ssl_write_all(SSL *ssl, std::string_view s) {
  std::size_t sent = 0;
  while (sent < s.size()) {
    // The buffer pointer must not move between retries: SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER is not
    // set by default, and OpenSSL treats a moved pointer as a caller bug.
    int r = SSL_write(ssl, s.data() + sent, static_cast<int>(s.size() - sent));
    if (r <= 0)
      return false;
    sent += static_cast<std::size_t>(r);
  }
  return true;
}

void close_ssl(SSL *ssl, int fd) {
  if (ssl) {
    SSL_shutdown(ssl);
    SSL_free(ssl);
  }
  if (fd >= 0)
    ::close(fd);
}

// Splices a TLS connection to a socketpair whose other end belongs to vhci_hcd. Single-threaded
// and non-blocking, because an SSL object cannot be read and written from two threads at once.
//
// This loop is also the liveness mechanism: when vhci detaches, our socketpair end EOFs and we
// close the TLS side; when the client vanishes, TLS errors and we close the socketpair, which
// vhci_rx reads as 0 and turns into an immediate unplug.
void pump(SSL *ssl, int ssl_fd, int sp) {
  set_nonblocking(ssl_fd, true);
  set_nonblocking(sp, true);

  std::string to_sp, to_ssl;
  char buf[16384];
  bool done = false;

  while (!done) {
    // poll() cannot see records already decrypted into OpenSSL's buffer, so drain those first or
    // the connection stalls until the peer happens to send more.
    bool ssl_ready = SSL_pending(ssl) > 0 && to_sp.empty();

    if (!ssl_ready) {
      pollfd p[2]{};
      p[0].fd = ssl_fd;
      p[0].events = 0;
      if (to_sp.empty())
        p[0].events |= POLLIN;
      if (!to_ssl.empty())
        p[0].events |= POLLOUT;
      p[1].fd = sp;
      p[1].events = 0;
      if (to_ssl.empty())
        p[1].events |= POLLIN;
      if (!to_sp.empty())
        p[1].events |= POLLOUT;

      int rc = ::poll(p, 2, 1000);
      if (rc < 0) {
        if (errno == EINTR)
          continue;
        break;
      }
      if (rc == 0)
        continue;
      if ((p[0].revents | p[1].revents) & (POLLERR | POLLNVAL))
        break;
      if ((p[1].revents & POLLHUP) && to_ssl.empty())
        break;

      ssl_ready = (p[0].revents & POLLIN) != 0;

      if (p[1].revents & POLLIN) {
        auto r = ::read(sp, buf, sizeof(buf));
        if (r > 0)
          to_ssl.assign(buf, static_cast<std::size_t>(r));
        else if (r == 0 || (r < 0 && errno != EAGAIN && errno != EINTR))
          done = true; // vhci let go
      }
      if (!to_sp.empty() && (p[1].revents & POLLOUT)) {
        auto w = ::write(sp, to_sp.data(), to_sp.size());
        if (w > 0)
          to_sp.erase(0, static_cast<std::size_t>(w));
        else if (w < 0 && errno != EAGAIN && errno != EINTR)
          done = true;
      }
      if (!to_ssl.empty() && (p[0].revents & POLLOUT)) {
        if (ssl_write_all(ssl, to_ssl))
          to_ssl.clear();
        else {
          int e = SSL_get_error(ssl, -1);
          if (e != SSL_ERROR_WANT_WRITE && e != SSL_ERROR_WANT_READ)
            done = true;
        }
      }
    }

    if (ssl_ready && to_sp.empty()) {
      int r = SSL_read(ssl, buf, sizeof(buf));
      if (r > 0)
        to_sp.assign(buf, static_cast<std::size_t>(r));
      else {
        int e = SSL_get_error(ssl, r);
        if (e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE)
          done = true; // client gone -- closing sp unplugs the device in milliseconds
      }
    }
  }

  ::close(sp);
  close_ssl(ssl, ssl_fd);
}

// ---- Channel ----

class TlsChannel : public Channel {
public:
  TlsChannel(SSL *ssl, int fd) : ssl_(ssl), fd_(fd) {}
  ~TlsChannel() override {
    if (!handed_off_)
      close_ssl(ssl_, fd_);
  }

  bool read_exact(void *buf, std::size_t n) override { return ssl_read_exact(ssl_, buf, n); }
  bool write_all(std::string_view s) override { return ssl_write_all(ssl_, s); }

  int into_kernel_fd() override {
    int sv[2];
    // A socketpair, not a loopback TCP pair: vhci_sysfs.c checks only socket->type, and a
    // socketpair binds no port, so nothing else on the box can connect to it.
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0) {
      logs::log(logs::warning, "[USBIP] socketpair failed: {}", std::strerror(errno));
      return -1;
    }
    handed_off_ = true;
    std::thread(pump, ssl_, fd_, sv[1]).detach();
    return sv[0];
  }

private:
  SSL *ssl_;
  int fd_;
  bool handed_off_ = false;
};

} // namespace

// ---- server ----

struct TunnelServer::Impl {
  mutable std::mutex mtx;
  std::condition_variable cv;

  int listen_fd = -1;
  int port = 0;
  SSL_CTX *ctx = nullptr;
  CertVerifier verify;
  std::atomic<bool> stopping{false};
  std::atomic<int> workers{0};
  std::thread accept_thread;

  // The single CONTROL link. A second HELLO replaces the first: a client that reconnected has a
  // new socket, and holding the dead one just delays everything by a keepalive interval.
  SSL *control_ssl = nullptr;
  int control_fd = -1;
  std::thread control_thread;
  bool control_up = false;
  std::size_t session_id = 0;
  std::string client_name;
  std::vector<tunnel::OfferedDevice> offers;

  // Every CONTROL write goes through here and is drained by the control thread. An SSL object is
  // not safe for concurrent use, and NEED_DATA is written by whichever thread is importing.
  std::deque<std::string> outbox;

  struct Pending {
    std::string busid;
    SSL *ssl = nullptr;
    int fd = -1;
    bool arrived = false;
  };
  std::map<std::uint32_t, Pending> pending;

  std::uint32_t new_token() {
    std::uint32_t t = 0;
    do {
      if (RAND_bytes(reinterpret_cast<unsigned char *>(&t), sizeof(t)) != 1)
        t = 0;
    } while (t == 0 || pending.count(t));
    return t;
  }

  void send_control(std::string bytes) {
    std::lock_guard<std::mutex> lk(mtx);
    if (control_up)
      outbox.push_back(std::move(bytes));
  }

  void drop_control(const char *why) {
    std::unique_lock<std::mutex> lk(mtx);
    if (!control_up)
      return;
    control_up = false;
    offers.clear();
    outbox.clear();
    logs::log(logs::info, "[USBIP] tunnel client disconnected ({})", why);
    cv.notify_all();
  }
};

TunnelServer::TunnelServer() : impl_(std::make_unique<Impl>()) {}
TunnelServer::~TunnelServer() { stop(); }

bool TunnelServer::start(int port, const std::string &cert_path, const std::string &key_path, CertVerifier verify) {
  auto &I = *impl_;
  I.verify = std::move(verify);

  I.ctx = SSL_CTX_new(TLS_server_method());
  if (!I.ctx) {
    logs::log(logs::warning, "[USBIP] SSL_CTX_new failed: {}", ssl_err());
    return false;
  }
  SSL_CTX_set_min_proto_version(I.ctx, TLS1_2_VERSION);
  if (SSL_CTX_use_certificate_chain_file(I.ctx, cert_path.c_str()) != 1 ||
      SSL_CTX_use_PrivateKey_file(I.ctx, key_path.c_str(), SSL_FILETYPE_PEM) != 1) {
    logs::log(logs::warning, "[USBIP] cannot load {} / {}: {}", cert_path, key_path, ssl_err());
    SSL_CTX_free(I.ctx);
    I.ctx = nullptr;
    return false;
  }
  // Demand a cert but accept self-signed at the TLS layer -- Moonlight clients are self-signed by
  // design. The real check is against the paired store, immediately after the handshake.
  SSL_CTX_set_verify(I.ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
                     [](int, X509_STORE_CTX *) { return 1; });

  I.listen_fd = ::socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC, 0);
  bool v6 = I.listen_fd >= 0;
  if (!v6)
    I.listen_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (I.listen_fd < 0) {
    logs::log(logs::warning, "[USBIP] socket() failed: {}", std::strerror(errno));
    return false;
  }
  int one = 1;
  ::setsockopt(I.listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  int zero = 0;
  if (v6)
    ::setsockopt(I.listen_fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(zero));

  bool bound;
  if (v6) {
    sockaddr_in6 a{};
    a.sin6_family = AF_INET6;
    a.sin6_addr = in6addr_any;
    a.sin6_port = htons(static_cast<std::uint16_t>(port));
    bound = ::bind(I.listen_fd, reinterpret_cast<sockaddr *>(&a), sizeof(a)) == 0;
  } else {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = INADDR_ANY;
    a.sin_port = htons(static_cast<std::uint16_t>(port));
    bound = ::bind(I.listen_fd, reinterpret_cast<sockaddr *>(&a), sizeof(a)) == 0;
  }
  if (!bound || ::listen(I.listen_fd, 8) != 0) {
    logs::log(logs::warning, "[USBIP] cannot listen on :{}: {}", port, std::strerror(errno));
    ::close(I.listen_fd);
    I.listen_fd = -1;
    return false;
  }
  I.port = port;

  I.accept_thread = std::thread([this] {
    auto &I = *impl_;
    while (!I.stopping) {
      pollfd p{I.listen_fd, POLLIN, 0};
      int rc = ::poll(&p, 1, 500);
      if (rc <= 0)
        continue;
      int fd = ::accept4(I.listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
      if (fd < 0)
        continue;
      set_stream_opts(fd);
      // One thread per connection: a peer that completes TCP and then says nothing must not block
      // the DATA connection an in-flight import is waiting for.
      I.workers++;
      std::thread([this, fd] {
        handshake(fd);
        impl_->workers--;
      }).detach();
    }
  });

  logs::log(logs::info, "[USBIP] tunnel listening on :{} (mTLS, paired clients only)", port);
  return true;
}

void TunnelServer::stop() {
  auto &I = *impl_;
  if (I.stopping.exchange(true))
    return;

  if (I.listen_fd >= 0)
    ::shutdown(I.listen_fd, SHUT_RDWR);
  if (I.accept_thread.joinable())
    I.accept_thread.join();
  if (I.listen_fd >= 0) {
    ::close(I.listen_fd);
    I.listen_fd = -1;
  }
  if (I.control_thread.joinable())
    I.control_thread.join();

  // Give the per-connection handshake threads a moment; they are detached and hold an SSL each.
  for (int i = 0; i < 100 && I.workers > 0; i++)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

  {
    std::lock_guard<std::mutex> lk(I.mtx);
    for (auto &[tok, p] : I.pending)
      if (p.arrived)
        close_ssl(p.ssl, p.fd);
    I.pending.clear();
    I.cv.notify_all();
  }
  if (I.ctx) {
    SSL_CTX_free(I.ctx);
    I.ctx = nullptr;
  }
}

bool TunnelServer::running() const { return impl_->listen_fd >= 0 && !impl_->stopping; }
int TunnelServer::port() const { return impl_->port; }

bool TunnelServer::has_client() const {
  std::lock_guard<std::mutex> lk(impl_->mtx);
  return impl_->control_up;
}

std::vector<tunnel::OfferedDevice> TunnelServer::offered() const {
  std::lock_guard<std::mutex> lk(impl_->mtx);
  return impl_->offers;
}

std::size_t TunnelServer::client_session() const {
  std::lock_guard<std::mutex> lk(impl_->mtx);
  return impl_->session_id;
}

void TunnelServer::handshake(int fd) {
  auto &I = *impl_;

  // Bounded, because an accepted-but-silent peer would otherwise hold an SSL and a thread forever.
  set_timeout(fd, kHandshakeTimeoutMs);

  SSL *ssl = SSL_new(I.ctx);
  if (!ssl) {
    ::close(fd);
    return;
  }
  SSL_set_fd(ssl, fd);
  if (SSL_accept(ssl) != 1) {
    logs::log(logs::debug, "[USBIP] tunnel TLS handshake failed: {}", ssl_err());
    close_ssl(ssl, fd);
    return;
  }

  // THE security check. Everything downstream of here reads bytes that end up in the host
  // kernel's URB parser, so an unpaired peer must not get one byte further.
  x509::x509_ptr peer(SSL_get_peer_certificate(ssl), X509_free);
  if (!peer || !I.verify || !I.verify(peer)) {
    logs::log(logs::warning, "[USBIP] tunnel: rejecting a client that is not paired");
    ssl_write_all(ssl, tunnel::encode_frame(tunnel::FrameType::ERROR, "reason=not_paired\n"));
    close_ssl(ssl, fd);
    return;
  }

  std::uint8_t pre[tunnel::kPreambleSize];
  tunnel::Preamble p;
  if (!ssl_read_exact(ssl, pre, sizeof(pre)) || !tunnel::decode_preamble(pre, sizeof(pre), p)) {
    logs::log(logs::warning, "[USBIP] tunnel: bad or missing preamble");
    close_ssl(ssl, fd);
    return;
  }

  if (p.channel == tunnel::Channel::CONTROL) {
    std::unique_lock<std::mutex> lk(I.mtx);
    if (I.control_up) {
      // A reconnecting client has a fresh socket; keeping the stale one just delays it.
      lk.unlock();
      I.drop_control("replaced by a new CONTROL connection");
      lk.lock();
    }
    if (I.control_thread.joinable())
      I.control_thread.join();
    I.control_ssl = ssl;
    I.control_fd = fd;
    I.control_up = true;
    I.session_id = 0;
    I.client_name.clear();
    I.offers.clear();
    I.outbox.clear();
    lk.unlock();
    // The control loop owns this connection from here; run it inline on this worker thread.
    control_loop();
    return;
  }

  // DATA: the token is the authorization. An unknown one means either a stale request we already
  // timed out, or someone guessing -- neither gets spliced to a device.
  std::unique_lock<std::mutex> lk(I.mtx);
  auto it = I.pending.find(p.token);
  if (it == I.pending.end() || it->second.arrived) {
    lk.unlock();
    logs::log(logs::warning, "[USBIP] tunnel: DATA connection with an unknown token {:08x}", p.token);
    close_ssl(ssl, fd);
    return;
  }
  // Clear the handshake timeouts: from here the socket carries URBs and blocks by design.
  set_timeout(fd, 0);
  it->second.ssl = ssl;
  it->second.fd = fd;
  it->second.arrived = true;
  I.cv.notify_all();
  // Ownership passed to whoever asked for it; do not close on the way out.
}

void TunnelServer::control_loop() {
  auto &I = *impl_;
  SSL *ssl = I.control_ssl;
  int fd = I.control_fd;

  set_timeout(fd, 0);
  set_nonblocking(fd, true);
  tunnel::FrameDecoder dec;
  tunnel::Liveness live;
  char buf[8192];
  bool alive = true;
  const char *why = "closed";

  while (alive && !I.stopping) {
    // Drain OpenSSL's own buffer first: poll() cannot see records it has already decrypted.
    bool readable = SSL_pending(ssl) > 0;
    if (!readable) {
      pollfd p{fd, POLLIN, 0};
      int rc = ::poll(&p, 1, kControlPollMs);
      if (rc < 0 && errno != EINTR)
        break;
      if (rc > 0 && (p.revents & (POLLERR | POLLHUP | POLLNVAL))) {
        why = "socket error";
        break;
      }
      readable = rc > 0 && (p.revents & POLLIN);
    }

    if (readable) {
      int r = SSL_read(ssl, buf, sizeof(buf));
      if (r <= 0) {
        int e = SSL_get_error(ssl, r);
        if (e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE) {
          why = "peer closed";
          break;
        }
      } else {
        std::vector<tunnel::Frame> frames;
        if (!dec.feed(buf, static_cast<std::size_t>(r), frames)) {
          why = "protocol violation";
          break;
        }
        for (const auto &f : frames) {
          switch (f.type) {
          case tunnel::FrameType::HELLO: {
            std::size_t sid = 0;
            std::string name;
            if (tunnel::decode_hello(f.payload, sid, name)) {
              std::lock_guard<std::mutex> lk(I.mtx);
              I.session_id = sid;
              I.client_name = name;
              logs::log(logs::info, "[USBIP] tunnel client '{}' connected (session {})",
                        name.empty() ? "?" : name, sid);
            }
            break;
          }
          case tunnel::FrameType::OFFER: {
            auto devs = tunnel::decode_offer(f.payload);
            {
              std::lock_guard<std::mutex> lk(I.mtx);
              I.offers = devs;
            }
            std::string list;
            for (const auto &d : devs)
              list += (list.empty() ? "" : ", ") + d.busid;
            logs::log(logs::info, "[USBIP] client offers {} device(s){}{}", devs.size(),
                      list.empty() ? "" : ": ", list);
            break;
          }
          case tunnel::FrameType::PONG:
            live.on_pong(now_ms());
            break;
          case tunnel::FrameType::PING:
            I.send_control(tunnel::encode_frame(tunnel::FrameType::PONG));
            break;
          case tunnel::FrameType::DEV_GONE: {
            auto kv = tunnel::parse_kv_lines(f.payload);
            if (!kv.empty() && kv[0].count("busid")) {
              auto busid = kv[0]["busid"];
              std::lock_guard<std::mutex> lk(I.mtx);
              for (auto d = I.offers.begin(); d != I.offers.end();)
                d = (d->busid == busid) ? I.offers.erase(d) : d + 1;
              logs::log(logs::info, "[USBIP] client lost {} locally", busid);
            }
            break;
          }
          case tunnel::FrameType::BYE:
            why = "client said BYE";
            alive = false;
            break;
          default:
            break;
          }
        }
      }
    }

    // Liveness: USB/IP has no heartbeat of its own, so without this a vanished client leaves a
    // zombie device for the 13-15 minutes TCP spends in tcp_retries2.
    auto t = now_ms();
    if (live.should_ping(t))
      I.send_control(tunnel::encode_frame(tunnel::FrameType::PING));
    if (live.gone()) {
      why = "no PONG";
      break;
    }

    // Drain the outbox here rather than from the requesting thread: an SSL object is not safe for
    // concurrent use, and this is the only thread that touches this one.
    for (;;) {
      std::string out;
      {
        std::lock_guard<std::mutex> lk(I.mtx);
        if (I.outbox.empty())
          break;
        out = std::move(I.outbox.front());
        I.outbox.pop_front();
      }
      if (!ssl_write_all(ssl, out)) {
        int e = SSL_get_error(ssl, -1);
        if (e == SSL_ERROR_WANT_WRITE || e == SSL_ERROR_WANT_READ) {
          // Would block. Put it back rather than dropping it -- a lost NEED_DATA is an import that
          // hangs until its timeout with no error anywhere.
          std::lock_guard<std::mutex> lk(I.mtx);
          I.outbox.push_front(std::move(out));
        } else {
          why = "write failed";
          alive = false;
        }
        break;
      }
    }
  }

  I.drop_control(why);
  {
    std::lock_guard<std::mutex> lk(I.mtx);
    I.control_ssl = nullptr;
    I.control_fd = -1;
  }
  close_ssl(ssl, fd);
}

namespace {

class TunnelTransport : public Transport {
public:
  TunnelTransport(TunnelServer::Impl &impl, std::string desc) : I_(impl), desc_(std::move(desc)) {}

  std::string describe() const override { return desc_; }

  std::vector<std::string> busids() override {
    std::lock_guard<std::mutex> lk(I_.mtx);
    std::vector<std::string> out;
    for (const auto &d : I_.offers)
      out.push_back(d.busid);
    return out;
  }

  std::unique_ptr<Channel> open(const std::string &busid, int timeout_ms) override {
    std::uint32_t token;
    {
      std::lock_guard<std::mutex> lk(I_.mtx);
      if (!I_.control_up)
        return nullptr;
      token = I_.new_token();
      I_.pending[token] = {busid, nullptr, -1, false};
      // The server cannot dial the client, so this is how it initiates: ask, then wait for the
      // connection the client makes in response.
      I_.outbox.push_back(
          tunnel::encode_frame(tunnel::FrameType::NEED_DATA, tunnel::encode_need_data(token, busid)));
    }

    std::unique_lock<std::mutex> lk(I_.mtx);
    bool ok = I_.cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] {
      auto it = I_.pending.find(token);
      return it == I_.pending.end() || it->second.arrived || !I_.control_up;
    });

    auto it = I_.pending.find(token);
    if (!ok || it == I_.pending.end() || !it->second.arrived) {
      if (it != I_.pending.end())
        I_.pending.erase(it);
      logs::log(logs::warning, "[USBIP] client never opened a DATA connection for {} within {}ms", busid,
                timeout_ms);
      return nullptr;
    }
    auto ssl = it->second.ssl;
    auto fd = it->second.fd;
    I_.pending.erase(it);
    return std::make_unique<TlsChannel>(ssl, fd);
  }

private:
  TunnelServer::Impl &I_;
  std::string desc_;
};

} // namespace

std::unique_ptr<Transport> TunnelServer::transport() {
  auto &I = *impl_;
  std::lock_guard<std::mutex> lk(I.mtx);
  if (!I.control_up)
    return nullptr;
  auto desc = "tunnel :" + std::to_string(I.port) + " (" + (I.client_name.empty() ? "?" : I.client_name) + ")";
  return std::make_unique<TunnelTransport>(I, desc);
}

} // namespace usbip
