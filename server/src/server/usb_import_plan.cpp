#include <server/usb_import_plan.hpp>

#include <cstdio>

namespace usbip {

std::string hex4(std::uint16_t v) {
  char buf[8];
  std::snprintf(buf, sizeof(buf), "%04x", v);
  return buf;
}

std::vector<NodeSpec> plan_nodes(const DiscoveredDevice &dev) {
  std::vector<NodeSpec> out;

  // The usbfs node. Only if the kernel actually gave it one -- major 0 means we never read a
  // valid `dev` file, and inventing numbers is how you get a node that EPERMs or, worse, points
  // at someone else's device.
  if (dev.usbfs.major != 0)
    out.push_back({usbfs_path(dev.busnum, dev.devnum), dev.usbfs.major, dev.usbfs.minor});

  for (const auto &iface : dev.interfaces)
    for (const auto &h : iface.hidraws)
      out.push_back({"/dev/" + h.name, h.major, h.minor});

  return out;
}

std::vector<fake_udev::Device> plan_announcements(const DiscoveredDevice &dev) {
  std::vector<fake_udev::Device> out;

  // 1. The usb_device itself.
  {
    fake_udev::Device d;
    d.subsystem = "usb";
    d.devtype = "usb_device";
    d.syspath = dev.syspath;
    d.sysname = dev.busid;
    if (dev.usbfs.major != 0) {
      d.devnode = usbfs_path(dev.busnum, dev.devnum);
      d.major = dev.usbfs.major;
      d.minor = dev.usbfs.minor;
    }
    // Valve's rules and SDL both key off the vendor id, and it is read from the USB parent, so it
    // has to be on this event.
    d.extra_props = {{"ID_VENDOR_ID", hex4(dev.id_vendor)},
                     {"ID_MODEL_ID", hex4(dev.id_product)},
                     {"BUSNUM", std::to_string(dev.busnum)},
                     {"DEVNUM", std::to_string(dev.devnum)}};
    out.push_back(std::move(d));
  }

  // 2. Each interface. No device node, so no DEVNAME/MAJOR/MINOR.
  for (const auto &iface : dev.interfaces) {
    fake_udev::Device d;
    d.subsystem = "usb";
    d.devtype = "usb_interface";
    d.syspath = iface.syspath;
    d.sysname = iface.sysname;
    if (!iface.driver.empty())
      d.extra_props = {{"DRIVER", iface.driver}};
    out.push_back(std::move(d));
  }

  // 3. hidraw nodes -- what Steam actually opens to talk to a controller.
  for (const auto &iface : dev.interfaces) {
    for (const auto &h : iface.hidraws) {
      fake_udev::Device d;
      d.subsystem = "hidraw";
      d.devnode = "/dev/" + h.name;
      d.syspath = h.syspath;
      d.sysname = h.name;
      d.major = h.major;
      d.minor = h.minor;
      out.push_back(std::move(d));
    }
  }

  // 4. evdev nodes. We do not create these (the /dev/input bind mount supplies them) but we do
  //    announce them, so SDL's evdev backend hotplug-detects the pad.
  for (const auto &iface : dev.interfaces) {
    for (const auto &e : iface.inputs) {
      fake_udev::Device d;
      d.subsystem = "input";
      d.devnode = "/dev/input/" + e.name;
      d.syspath = e.syspath;
      d.sysname = e.name;
      d.major = e.major;
      d.minor = e.minor;
      d.id_input_class = "joystick";
      out.push_back(std::move(d));
    }
  }

  return out;
}

} // namespace usbip
