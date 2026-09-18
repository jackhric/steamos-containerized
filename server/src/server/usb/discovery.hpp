#pragma once

// Walks an imported USB device's sysfs tree to find every node we must create and announce.
//
// vhci gives us a busid ("5-1") once enumeration completes; everything else -- interfaces, their
// bound drivers, hidraw and evdev nodes, and the major:minor of each -- has to be read back out
// of sysfs. Two rules, both learned the hard way in Step 0:
//
//   * major:minor ALWAYS comes from the node's sysfs `dev` file, never computed and never cached.
//     Binding a device frees its old node numbers and the import reuses them, so the same physical
//     device can come back on different (or confusingly identical) numbers.
//   * enumeration is asynchronous. Interfaces appear before their drivers bind, and hidraw nodes
//     appear after that again, so discovery reports `complete` and the caller polls.
//
// Filesystem access goes through SysfsReader so the whole walk is unit-testable against a captured
// tree, with no devices and no root.

#include <cstdint>
#include <string>
#include <vector>

namespace usbip {

struct SysfsReader {
  virtual ~SysfsReader() = default;
  // Reads a sysfs attribute. Returns false if absent. Trailing newline is stripped by the caller.
  virtual bool read(const std::string &path, std::string &out) const = 0;
  virtual std::vector<std::string> list_dir(const std::string &path) const = 0;
  virtual bool exists(const std::string &path) const = 0;
};

// Reads the real /sys.
class RealSysfs : public SysfsReader {
public:
  bool read(const std::string &path, std::string &out) const override;
  std::vector<std::string> list_dir(const std::string &path) const override;
  bool exists(const std::string &path) const override;
};

struct CharNode {
  std::string name;    // "hidraw3" / "event12"
  std::string syspath; // /devices/... (no /sys prefix)
  unsigned major = 0;
  unsigned minor = 0;
};

struct Interface {
  std::string sysname; // "5-1:1.2"
  std::string syspath;
  std::string driver;  // "usbhid", "cdc_acm", "" when nothing has bound yet
  std::vector<CharNode> hidraws;
  std::vector<CharNode> inputs; // evdev nodes
};

struct DiscoveredDevice {
  std::string busid;   // "5-1" -- as it appears on OUR side, from vhci's local_busid
  std::string syspath;
  unsigned busnum = 0;
  unsigned devnum = 0;
  std::uint16_t id_vendor = 0;
  std::uint16_t id_product = 0;
  unsigned num_interfaces = 0; // bNumInterfaces, i.e. how many we expect

  CharNode usbfs; // the /dev/bus/usb node for the device itself
  std::vector<Interface> interfaces;

  // Every declared interface is present and has a driver bound. Until then the caller should keep
  // polling rather than announce a half-built device to Steam.
  bool complete = false;
};

// The usbfs node path. Zero-padded to three digits each -- /dev/bus/usb/005/002, not 5/2.
std::string usbfs_path(unsigned busnum, unsigned devnum);

// Parses a sysfs `dev` file ("189:513"). Returns false on anything malformed.
bool parse_dev_file(const std::string &text, unsigned &major, unsigned &minor);

// Walks <sysfs_root>/<busid>. `sysfs_root` is the bus directory holding the device (normally
// "/sys/bus/usb/devices"). Returns false only if the device itself is missing or unreadable;
// a device that exists but has not finished enumerating returns true with complete == false.
bool discover(const SysfsReader &fs, const std::string &sysfs_root, const std::string &busid,
              DiscoveredDevice &out);

} // namespace usbip
