// Pure half of USB device discovery: the sysfs walk, driven through SysfsReader so it can run
// against a captured tree. RealSysfs (the only part that touches the filesystem) lives at the
// bottom and is deliberately trivial.

#include <server/usb_discovery.hpp>

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace usbip {

namespace {

// Both ends: sysfs pads some attributes on the LEFT (bNumInterfaces reads as " 7") and terminates
// most with a newline.
std::string trim(std::string s) {
  auto ws = [](char c) { return c == '\n' || c == '\r' || c == ' ' || c == '\t'; };
  while (!s.empty() && ws(s.back()))
    s.pop_back();
  std::size_t start = 0;
  while (start < s.size() && ws(s[start]))
    start++;
  return s.substr(start);
}

bool read_trimmed(const SysfsReader &fs, const std::string &path, std::string &out) {
  if (!fs.read(path, out))
    return false;
  out = trim(out);
  return true;
}

template <typename T> bool read_int(const SysfsReader &fs, const std::string &path, T &out,
                                    int base = 10) {
  std::string s;
  if (!read_trimmed(fs, path, s) || s.empty())
    return false;
  T v{};
  auto res = std::from_chars(s.data(), s.data() + s.size(), v, base);
  if (res.ec != std::errc() || res.ptr != s.data() + s.size())
    return false;
  out = v;
  return true;
}

// A sysfs path with the /sys prefix removed -- that is what udev's DEVPATH carries.
std::string to_devpath(const std::string &syspath) {
  return syspath.rfind("/sys", 0) == 0 ? syspath.substr(4) : syspath;
}

// <iface>/<hid-or-input-dir>/<nodeN>/dev -- collects nodes of one class under an interface.
void collect_nodes(const SysfsReader &fs, const std::string &dir, const std::string &prefix,
                   std::vector<CharNode> &out) {
  for (const auto &entry : fs.list_dir(dir)) {
    if (entry.rfind(prefix, 0) != 0)
      continue;
    CharNode n;
    n.name = entry;
    n.syspath = to_devpath(dir + "/" + entry);
    std::string dev;
    // No `dev` file means the node is not there yet -- skip rather than invent numbers.
    if (!read_trimmed(fs, dir + "/" + entry + "/dev", dev))
      continue;
    if (!parse_dev_file(dev, n.major, n.minor))
      continue;
    out.push_back(std::move(n));
  }
  std::sort(out.begin(), out.end(),
            [](const CharNode &a, const CharNode &b) { return a.name < b.name; });
}

// hidraw lives at <iface>/<HID id>/hidraw/hidrawN; evdev at <iface>/input/inputN/eventN.
void collect_interface_nodes(const SysfsReader &fs, Interface &iface) {
  const std::string base = "/sys" + iface.syspath;

  for (const auto &child : fs.list_dir(base)) {
    const std::string child_path = base + "/" + child;

    // HID function directories are named like "0003:28DE:1304.000B".
    if (fs.exists(child_path + "/hidraw"))
      collect_nodes(fs, child_path + "/hidraw", "hidraw", iface.hidraws);

    if (child == "input") {
      for (const auto &inp : fs.list_dir(child_path)) {
        if (inp.rfind("input", 0) == 0)
          collect_nodes(fs, child_path + "/" + inp, "event", iface.inputs);
      }
    }
  }
}

} // namespace

std::string usbfs_path(unsigned busnum, unsigned devnum) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "/dev/bus/usb/%03u/%03u", busnum, devnum);
  return buf;
}

bool parse_dev_file(const std::string &text, unsigned &major, unsigned &minor) {
  auto s = trim(text);
  auto colon = s.find(':');
  if (colon == std::string::npos || colon == 0 || colon + 1 >= s.size())
    return false;
  unsigned maj{}, min{};
  auto a = std::from_chars(s.data(), s.data() + colon, maj);
  if (a.ec != std::errc() || a.ptr != s.data() + colon)
    return false;
  auto b = std::from_chars(s.data() + colon + 1, s.data() + s.size(), min);
  if (b.ec != std::errc() || b.ptr != s.data() + s.size())
    return false;
  major = maj;
  minor = min;
  return true;
}

bool discover(const SysfsReader &fs, const std::string &sysfs_root, const std::string &busid,
              DiscoveredDevice &out) {
  out = DiscoveredDevice{};
  out.busid = busid;

  const std::string dev_dir = sysfs_root + "/" + busid;
  if (!fs.exists(dev_dir))
    return false;
  out.syspath = to_devpath(dev_dir);

  // idVendor/idProduct are hex without a 0x prefix; busnum/devnum are decimal.
  if (!read_int(fs, dev_dir + "/busnum", out.busnum) ||
      !read_int(fs, dev_dir + "/devnum", out.devnum) ||
      !read_int(fs, dev_dir + "/idVendor", out.id_vendor, 16) ||
      !read_int(fs, dev_dir + "/idProduct", out.id_product, 16))
    return false;
  // bNumInterfaces is space-padded in sysfs (" 7"), which read_int's trim handles.
  read_int(fs, dev_dir + "/bNumInterfaces", out.num_interfaces);

  out.usbfs.name = busid;
  out.usbfs.syspath = out.syspath;
  {
    std::string dev;
    if (read_trimmed(fs, dev_dir + "/dev", dev))
      parse_dev_file(dev, out.usbfs.major, out.usbfs.minor);
  }

  // Interfaces are CHILDREN of the device dir and named "<busid>:<config>.<num>".
  const std::string iface_prefix = busid + ":";
  for (const auto &child : fs.list_dir(dev_dir)) {
    if (child.rfind(iface_prefix, 0) != 0)
      continue;
    Interface iface;
    iface.sysname = child;
    iface.syspath = to_devpath(dev_dir + "/" + child);
    std::string drv;
    // driver is a symlink to .../drivers/<name>; its basename is what we want.
    if (read_trimmed(fs, dev_dir + "/" + child + "/driver_name", drv))
      iface.driver = drv;
    collect_interface_nodes(fs, iface);
    out.interfaces.push_back(std::move(iface));
  }
  std::sort(out.interfaces.begin(), out.interfaces.end(),
            [](const Interface &a, const Interface &b) { return a.sysname < b.sysname; });

  // "Complete" means every declared interface exists and something has bound to each. Announcing
  // before that gives Steam a device whose nodes are still appearing.
  out.complete = out.num_interfaces > 0 && out.interfaces.size() == out.num_interfaces &&
                 std::all_of(out.interfaces.begin(), out.interfaces.end(),
                             [](const Interface &i) { return !i.driver.empty(); });
  return true;
}

// ---------------------------------------------------------------- real fs --

bool RealSysfs::read(const std::string &path, std::string &out) const {
  // A `driver_name` request is really "resolve the driver symlink and take its basename"; sysfs
  // has no such attribute.
  if (path.size() > 12 && path.compare(path.size() - 12, 12, "/driver_name") == 0) {
    std::error_code ec;
    auto target = std::filesystem::read_symlink(path.substr(0, path.size() - 12) + "/driver", ec);
    if (ec)
      return false;
    out = target.filename().string();
    return true;
  }
  std::ifstream f(path);
  if (!f)
    return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  return true;
}

std::vector<std::string> RealSysfs::list_dir(const std::string &path) const {
  std::vector<std::string> out;
  std::error_code ec;
  for (const auto &e : std::filesystem::directory_iterator(path, ec))
    out.push_back(e.path().filename().string());
  return out;
}

bool RealSysfs::exists(const std::string &path) const {
  std::error_code ec;
  return std::filesystem::exists(path, ec);
}

} // namespace usbip
