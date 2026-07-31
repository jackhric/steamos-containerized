#pragma once

// In-process "fake udev" for virtual input devices.
//
// SDL (hence Steam) enumerates joysticks via the udev client library: it needs a
// NETLINK_KOBJECT_UEVENT "add" event to hotplug-detect a device, and a /run/udev/data/c13:NN
// hwdb entry to classify it as ID_INPUT_JOYSTICK. This container has no udevd, so uinput-created
// pads are invisible to Steam. Wolf solves this cross-container with a fake-udev CLI + docker
// exec; we are single-container, so we inject the netlink event and write the hwdb entry directly
// from the server process. Ported from reference/wolf/src/fake-udev.
//
// NB: needs a writable /run/udev/data (see entrypoint) and NET_ADMIN (already granted) to bind
// the netlink uevent group.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace fake_udev {

// Identifies one device node so we can synthesize its udev event + hwdb entry, and undo them.
// Defaults describe a uinput joystick, which is what VirtualGamepad needs and what this started
// life as; USB/IP devices set subsystem/devtype explicitly.
struct Device {
  // input | hidraw | usb. Hashed into the netlink header, where subscribers filter on it.
  std::string subsystem = "input";
  // "" | usb_device | usb_interface. Only usb devices carry one.
  std::string devtype;

  std::string devnode; // e.g. /dev/input/event31. Empty for nodeless devices (usb_interface).
  std::string syspath; // /devices/... (no /sys prefix)
  std::string sysname; // e.g. "3-1:1.0" -- names the hwdb entry when there is no devnode
  unsigned int major = 13;
  unsigned int minor = 0;

  // Drives the ID_INPUT_* classification (joystick|mouse|keyboard). libinput assigns
  // device capabilities from these udev properties. Only meaningful for subsystem == "input".
  std::string id_input_class = "joystick";

  // Merged into the uevent verbatim, last, so they can override a computed property.
  std::map<std::string, std::string> extra_props;
  // If non-empty, used as the hwdb entry verbatim instead of the synthesized one. USB/IP devices
  // copy the host's already-computed entry rather than guessing at ID_VENDOR_ID/ID_SERIAL_SHORT.
  std::string hwdb_override;

  bool has_node() const { return !devnode.empty(); }
};

// Resolve a Device from a uinput fd (after UI_DEV_CREATE) via UI_GET_SYSNAME + stat. Returns false
// if the sysname/devnode can't be resolved.
bool device_from_uinput_fd(int fd, Device &out);

// Resolve a Device from an existing /dev/input/eventNN node (a device we did not create,
// e.g. Steam Input's virtual mouse/keyboard) via /sys/class/input + stat.
bool device_from_event_node(const std::string &devnode, Device &out);

// Announce (ACTION=add) or withdraw (ACTION=remove) the device to udev/SDL: writes/removes the
// /run/udev/data hwdb entry and sends the matching netlink uevent. Best-effort; logs on failure.
void plug(const Device &dev);
void unplug(const Device &dev);

// Pure helpers, exposed for testing. These decide every byte we hand to libudev subscribers, and
// a wrong value here fails silently -- the kernel drops a mis-hashed message before any subscriber
// sees it, with no error anywhere. Kept separate from the syscalls so they can be pinned by
// unit tests with no netlink socket and no /run/udev.
namespace detail {

inline constexpr const char *kUdevDataDir = "/run/udev/data";

// MurmurHash2, seeded exactly as systemd seeds it. Must stay byte-identical or subscriber-side
// socket filters silently discard our events.
std::uint32_t murmur2(const void *key, int len, std::uint32_t seed);

std::vector<std::string> class_props(const Device &dev);
std::map<std::string, std::string> base_event(const Device &dev, const char *action);
std::string serialize_props(const std::map<std::string, std::string> &props);
std::string hwdb_path(const Device &dev);
std::string hwdb_contents(const Device &dev);

} // namespace detail

} // namespace fake_udev
