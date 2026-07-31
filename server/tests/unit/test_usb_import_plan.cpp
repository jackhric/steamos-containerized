// Node and announcement planning for an imported device. Pure -- no devices, no /sys, no netlink.
//
// The ordering assertions are the point. Announcing a child before its parent, or announcing
// anything before its node exists, produces a device Steam can see but not open -- and nothing
// logs an error when that happens.

#include <cassert>
#include <iostream>
#include <server/usb_import_plan.hpp>
#include <string>
#include <vector>

using namespace usbip;

static void ok(const char *what) { std::cout << "[ OK ] " << what << "\n"; }

// The Puck as Step 0 imported it: busid 5-1, 7 interfaces (cdc_acm x2, usbhid x5), 5 hidraw.
static DiscoveredDevice puck() {
  DiscoveredDevice d;
  d.busid = "5-1";
  d.syspath = "/devices/platform/vhci_hcd.0/usb5/5-1";
  d.busnum = 5;
  d.devnum = 2;
  d.id_vendor = 0x28de;
  d.id_product = 0x1304;
  d.num_interfaces = 7;
  d.usbfs = {"5-1", d.syspath, 189, 513};

  for (int i = 0; i < 7; i++) {
    Interface iface;
    iface.sysname = "5-1:1." + std::to_string(i);
    iface.syspath = d.syspath + "/" + iface.sysname;
    iface.driver = i < 2 ? "cdc_acm" : "usbhid";
    if (i >= 2) {
      CharNode h;
      h.name = "hidraw" + std::to_string(i - 2);
      h.syspath = iface.syspath + "/0003:28DE:1304.000B/hidraw/" + h.name;
      h.major = 243;
      h.minor = static_cast<unsigned>(i - 2);
      iface.hidraws.push_back(h);
    }
    d.interfaces.push_back(std::move(iface));
  }
  d.complete = true;
  return d;
}

static void test_hex4() {
  assert(hex4(0x28de) == "28de");
  assert(hex4(0x1304) == "1304");
  assert(hex4(0x0001) == "0001"); // zero-padded, as sysfs renders it
  assert(hex4(0xffff) == "ffff");
  ok("vendor/product render as 4-digit lowercase hex like sysfs");
}

static void test_nodes() {
  auto n = plan_nodes(puck());

  // usbfs node + 5 hidraw. NOT evdev: /dev/input is bind-mounted from the host, and creating
  // those would mean chown'ing the host's devtmpfs.
  assert(n.size() == 6);
  assert(n[0].path == "/dev/bus/usb/005/002");
  assert(n[0].major == 189 && n[0].minor == 513);
  ok("usbfs node planned at the zero-padded path with sysfs-read major:minor");

  int hid = 0;
  for (const auto &s : n)
    if (s.path.rfind("/dev/hidraw", 0) == 0) {
      assert(s.major == 243);
      hid++;
    }
  assert(hid == 5);
  ok("5 hidraw nodes planned, majors read from sysfs (not assumed)");

  for (const auto &s : n)
    assert(s.path.rfind("/dev/input/", 0) != 0);
  ok("no evdev nodes planned (the /dev/input bind mount supplies them)");

  // A device whose usbfs `dev` file was unreadable must not get an invented node.
  auto d = puck();
  d.usbfs.major = 0;
  auto n2 = plan_nodes(d);
  assert(n2.size() == 5);
  for (const auto &s : n2)
    assert(s.path.rfind("/dev/bus/usb", 0) != 0);
  ok("no usbfs node is invented when the kernel gave no major");
}

static void test_announcement_order() {
  auto a = plan_announcements(puck());

  // 1 usb_device + 7 usb_interface + 5 hidraw = 13 (no evdev in this fixture).
  assert(a.size() == 13);

  assert(a[0].subsystem == "usb" && a[0].devtype == "usb_device");
  assert(a[0].sysname == "5-1");
  ok("the usb_device is announced FIRST (parent before children)");

  for (std::size_t i = 1; i <= 7; i++) {
    assert(a[i].subsystem == "usb");
    assert(a[i].devtype == "usb_interface");
  }
  ok("its 7 interfaces follow");

  for (std::size_t i = 8; i < 13; i++)
    assert(a[i].subsystem == "hidraw");
  ok("hidraw nodes come last, after the interfaces they hang off");

  // Reversing gives a valid unplug order: children before parents.
  assert(a.back().subsystem == "hidraw");
  assert(a.front().devtype == "usb_device");
  ok("reversed, the list is a valid unplug order (children first)");
}

static void test_announcement_properties() {
  auto a = plan_announcements(puck());

  const auto &usbdev = a[0];
  assert(usbdev.devnode == "/dev/bus/usb/005/002");
  assert(usbdev.major == 189 && usbdev.minor == 513);
  // Valve's udev rules and SDL both match on the vendor id via the USB parent, so it must be here.
  assert(usbdev.extra_props.at("ID_VENDOR_ID") == "28de");
  assert(usbdev.extra_props.at("ID_MODEL_ID") == "1304");
  assert(usbdev.extra_props.at("BUSNUM") == "5");
  assert(usbdev.extra_props.at("DEVNUM") == "2");
  ok("usb_device announcement carries ID_VENDOR_ID/ID_MODEL_ID (what Valve's rules match on)");

  assert(a[1].devnode.empty());
  assert(!a[1].has_node());
  assert(a[1].sysname == "5-1:1.0");
  assert(a[1].extra_props.at("DRIVER") == "cdc_acm");
  ok("usb_interface has no node and reports its bound driver");

  const auto &hid = a[8];
  assert(hid.subsystem == "hidraw");
  assert(hid.devnode == "/dev/hidraw0");
  assert(hid.major == 243);
  assert(hid.sysname == "hidraw0");
  ok("hidraw announcement points at the node we created");
}

static void test_evdev_announced_not_created() {
  // Give an interface an evdev node: it must be ANNOUNCED (so SDL hotplug-detects it) but not
  // planned for creation.
  auto d = puck();
  CharNode e{"event12", d.interfaces[2].syspath + "/input/input57/event12", 13, 76};
  d.interfaces[2].inputs.push_back(e);

  auto nodes = plan_nodes(d);
  for (const auto &n : nodes)
    assert(n.path != "/dev/input/event12");
  ok("evdev node is NOT planned for creation");

  auto a = plan_announcements(d);
  bool found = false;
  for (const auto &x : a)
    if (x.subsystem == "input" && x.devnode == "/dev/input/event12") {
      assert(x.major == 13 && x.minor == 76);
      assert(x.id_input_class == "joystick");
      found = true;
      // and it must come after the hidraw entries
      assert(&x == &a.back());
    }
  assert(found);
  ok("evdev node IS announced, last, classified as a joystick");
}

static void test_non_hid_device() {
  // Generic USB: a mass-storage device still gets its usbfs node and announcements.
  DiscoveredDevice d;
  d.busid = "5-2";
  d.syspath = "/devices/platform/vhci_hcd.0/usb5/5-2";
  d.busnum = 5;
  d.devnum = 3;
  d.id_vendor = 0x058f;
  d.id_product = 0x6364;
  d.num_interfaces = 1;
  d.usbfs = {"5-2", d.syspath, 189, 1026};
  Interface i;
  i.sysname = "5-2:1.0";
  i.syspath = d.syspath + "/5-2:1.0";
  i.driver = "usb-storage";
  d.interfaces.push_back(i);

  auto nodes = plan_nodes(d);
  assert(nodes.size() == 1 && nodes[0].path == "/dev/bus/usb/005/003");
  auto a = plan_announcements(d);
  assert(a.size() == 2); // usb_device + one interface
  assert(a[0].extra_props.at("ID_VENDOR_ID") == "058f");
  ok("a non-HID device plans one node and two announcements");
}

int main() {
  test_hex4();
  test_nodes();
  test_announcement_order();
  test_announcement_properties();
  test_evdev_announced_not_created();
  test_non_hid_device();

  std::cout << "\nALL USB-IMPORT-PLAN TESTS PASSED\n";
  return 0;
}
