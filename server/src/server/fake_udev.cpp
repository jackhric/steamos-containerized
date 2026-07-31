#include <server/fake_udev.hpp>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <helpers/logger.hpp>
#include <linux/netlink.h>
#include <linux/uinput.h>
#include <map>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vector>

namespace fake_udev {

namespace {

const char *kUdevCtrlPath = "/run/udev/control";

// systemd device-monitor netlink header (see sd-device/device-monitor.c). Field order/sizes are
// ABI with libudev subscribers, so it must match exactly.
struct MonitorNetlinkHeader {
  char prefix[8] = "libudev";
  unsigned magic = 0xfeedcafe;
  unsigned header_size;
  unsigned properties_off;
  unsigned properties_len;
  unsigned filter_subsystem_hash;
  unsigned filter_devtype_hash;
  unsigned filter_tag_bloom_hi;
  unsigned filter_tag_bloom_lo;
};

// UDEV_MONITOR_UDEV: the multicast group libudev/SDL subscribers listen on. (Group 1 is the raw
// KERNEL group, which libudev subscribers ignore.)
constexpr unsigned kUdevMonitorGroup = 2;

} // namespace



namespace {

bool send_uevent(const Device &dev, const std::map<std::string, std::string> &props) {
  int fd = ::socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT);
  if (fd < 0) {
    logs::log(logs::warning, "[FAKE-UDEV] netlink socket failed: {}", std::strerror(errno));
    return false;
  }
  // Bind with groups=0: we are a SENDER, not a subscriber. The multicast target group is set on the
  // destination address below. (Binding to the group would instead subscribe us to it.)
  sockaddr_nl src{};
  src.nl_family = AF_NETLINK;
  src.nl_pid = 0; // let the kernel assign a unique unicast pid
  if (::bind(fd, reinterpret_cast<sockaddr *>(&src), sizeof(src)) < 0) {
    logs::log(logs::warning, "[FAKE-UDEV] netlink bind failed: {}", std::strerror(errno));
    ::close(fd);
    return false;
  }

  std::string payload = detail::serialize_props(props);
  MonitorNetlinkHeader header{};
  header.magic = htobe32(0xfeedcafe);
  header.header_size = sizeof(header);
  header.properties_off = sizeof(header);
  header.properties_len = static_cast<unsigned>(payload.size());
  // Subscribers install a BPF socket filter derived from these hashes. Get one wrong and the
  // KERNEL drops the message before the subscriber ever sees it -- no error, nothing logged,
  // the device simply never appears. SDL calls filter_add_match_subsystem_devtype("hidraw", NULL),
  // so devtype is only compared when the subscriber asked for one.
  header.filter_subsystem_hash =
      htobe32(detail::murmur2(dev.subsystem.data(), static_cast<int>(dev.subsystem.size()), 0));
  if (!dev.devtype.empty())
    header.filter_devtype_hash =
        htobe32(detail::murmur2(dev.devtype.data(), static_cast<int>(dev.devtype.size()), 0));

  iovec iov[2] = {
      {&header, sizeof(header)},
      {payload.data(), payload.size()},
  };
  // Multicast to the UDEV monitor group: nl_pid=0 + nl_groups=<group> means "send to the group",
  // which requires CAP_NET_ADMIN (granted). This is how libudev broadcasts synthesized events.
  sockaddr_nl dst{};
  dst.nl_family = AF_NETLINK;
  dst.nl_pid = 0;
  dst.nl_groups = kUdevMonitorGroup;
  msghdr msg{};
  msg.msg_name = &dst;
  msg.msg_namelen = sizeof(dst);
  msg.msg_iov = iov;
  msg.msg_iovlen = 2;

  bool ok = ::sendmsg(fd, &msg, 0) > 0;
  if (!ok)
    logs::log(logs::warning, "[FAKE-UDEV] sendmsg failed: {}", std::strerror(errno));
  ::close(fd);
  return ok;
}

void write_hwdb(const Device &dev) {
  std::error_code ec;
  std::filesystem::create_directories(detail::kUdevDataDir, ec);
  auto path = detail::hwdb_path(dev);
  std::ofstream f(path, std::ios::trunc);
  if (!f) {
    logs::log(logs::warning, "[FAKE-UDEV] cannot write hwdb {}: {}", path, std::strerror(errno));
    return;
  }
  f << detail::hwdb_contents(dev);
}

void remove_hwdb(const Device &dev) {
  std::error_code ec;
  std::filesystem::remove(detail::hwdb_path(dev), ec);
}

// libudev decides a udev daemon is running by the presence of /run/udev/control; without it,
// SDL's udev backend concludes udev is absent and won't hotplug-enumerate our injected event.
// Wolf creates this same empty control file (docker.cpp). Mode 0777 so any uid can stat it.
void ensure_udev_control() {
  std::error_code ec;
  std::filesystem::create_directories("/run/udev", ec);
  if (std::filesystem::exists(kUdevCtrlPath, ec))
    return;
  std::ofstream(kUdevCtrlPath).close();
  std::filesystem::permissions(kUdevCtrlPath, std::filesystem::perms::all, ec);
}

} // namespace

bool device_from_uinput_fd(int fd, Device &out) {
  char sysname[128] = {0};
  if (::ioctl(fd, UI_GET_SYSNAME(sizeof(sysname)), sysname) < 0) {
    logs::log(logs::warning, "[FAKE-UDEV] UI_GET_SYSNAME failed: {}", std::strerror(errno));
    return false;
  }
  // sysname is like "input42"; the event node lives under its sysfs dir. Find the eventNN child.
  std::string sys_input = std::string("/sys/devices/virtual/input/") + sysname;
  std::string event_name;
  std::error_code ec;
  for (const auto &e : std::filesystem::directory_iterator(sys_input, ec)) {
    auto n = e.path().filename().string();
    if (n.rfind("event", 0) == 0) {
      event_name = n;
      break;
    }
  }
  if (event_name.empty()) {
    logs::log(logs::warning, "[FAKE-UDEV] no eventNN under {}", sys_input);
    return false;
  }

  out.devnode = "/dev/input/" + event_name;
  // DEVPATH is the syspath without the /sys prefix, pointing at the event node.
  out.syspath = "/devices/virtual/input/" + std::string(sysname) + "/" + event_name;

  struct stat st{};
  if (::stat(out.devnode.c_str(), &st) == 0) {
    out.major = major(st.st_rdev);
    out.minor = minor(st.st_rdev);
  } else {
    logs::log(logs::warning, "[FAKE-UDEV] stat {} failed: {}", out.devnode, std::strerror(errno));
    return false;
  }
  return true;
}

bool device_from_event_node(const std::string &devnode, Device &out) {
  auto event_name = std::filesystem::path(devnode).filename().string(); // eventNN
  std::error_code ec;
  auto sys = std::filesystem::canonical("/sys/class/input/" + event_name, ec);
  if (ec) {
    logs::log(logs::warning, "[FAKE-UDEV] cannot resolve sysfs path for {}: {}", devnode,
              ec.message());
    return false;
  }
  out.syspath = sys.string();
  if (out.syspath.rfind("/sys", 0) == 0)
    out.syspath = out.syspath.substr(4);
  out.devnode = devnode;

  struct stat st{};
  if (::stat(devnode.c_str(), &st) != 0) {
    logs::log(logs::warning, "[FAKE-UDEV] stat {} failed: {}", devnode, std::strerror(errno));
    return false;
  }
  out.major = major(st.st_rdev);
  out.minor = minor(st.st_rdev);
  return true;
}

void plug(const Device &dev) {
  ensure_udev_control();
  write_hwdb(dev);
  bool sent = send_uevent(dev, detail::base_event(dev, "add"));
  logs::log(logs::info, "[FAKE-UDEV] plug {} [{}{}] {} uevent={}", detail::hwdb_path(dev),
            dev.subsystem, dev.devtype.empty() ? "" : "/" + dev.devtype,
            dev.has_node() ? dev.devnode : dev.sysname, sent ? "sent" : "FAILED");
}

void unplug(const Device &dev) {
  send_uevent(dev, detail::base_event(dev, "remove"));
  remove_hwdb(dev);
  logs::log(logs::info, "[FAKE-UDEV] unplug {} [{}]",
            dev.has_node() ? dev.devnode : dev.sysname, dev.subsystem);
}

} // namespace fake_udev
