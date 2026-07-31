#pragma once

// What to create and what to announce for an imported USB device -- computed as pure functions
// over a DiscoveredDevice, so the ordering and property rules are unit-testable with no devices.
//
// Ordering is load-bearing. The node must exist BEFORE the uevent that announces it: the container
// shares the host's network namespace, so in-container SDL already saw the host's real uevent for
// this device and found no node in the container's private /dev. Our announcement is a
// re-announcement, and it is only useful once the node is really there.
//
// Within that, we follow the kernel's own order -- usb_device, then its interfaces, then the
// hidraw and evdev nodes hanging off them -- so a subscriber never sees a child before its parent.

#include <server/fake_udev.hpp>
#include <server/usb_discovery.hpp>
#include <string>
#include <vector>

namespace usbip {

// A device node we must mknod into the container's private /dev.
struct NodeSpec {
  std::string path;
  unsigned major = 0;
  unsigned minor = 0;
};

// Nodes to create. Deliberately excludes evdev: /dev/input is bind-mounted from the host, so
// those nodes appear on their own and creating them would mean chown'ing the HOST's devtmpfs.
std::vector<NodeSpec> plan_nodes(const DiscoveredDevice &dev);

// Announcements in the order they must be emitted. Reverse this list for unplug, so a game sees
// children go away before their parent rather than a parent vanishing under them.
std::vector<fake_udev::Device> plan_announcements(const DiscoveredDevice &dev);

// 4-digit lowercase hex, matching how sysfs renders idVendor/idProduct.
std::string hex4(std::uint16_t v);

} // namespace usbip
