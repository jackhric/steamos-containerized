// fake_udev property/hash generation.
//
// The FIRST half of this file is a REGRESSION GUARD written before fake_udev was generalized for
// USB/IP. It pins the exact bytes the working uinput gamepad path produces today, so extending
// fake_udev to the usb/hidraw subsystems cannot silently break the controller that already works.
// If these fail, the generalization regressed the existing path -- do not "update the expected
// values", find the change.
//
// Pure: no netlink socket, no /run/udev, no devices, no Boost.Log link.

#include <cassert>
#include <cstring>
#include <iostream>
#include <server/fake_udev.hpp>
#include <string>
#include <vector>

using namespace fake_udev;
using namespace fake_udev::detail;

static void ok(const char *what) { std::cout << "[ OK ] " << what << "\n"; }

// Exactly what VirtualGamepad produces via device_from_uinput_fd(): an evdev node on char major
// 13, classified as a joystick.
static Device gamepad() {
  Device d;
  d.devnode = "/dev/input/event31";
  d.syspath = "/devices/virtual/input/input42/event31";
  d.major = 13;
  d.minor = 31;
  d.id_input_class = "joystick";
  return d;
}

static std::string prop(const std::map<std::string, std::string> &m, const char *k) {
  auto it = m.find(k);
  return it == m.end() ? std::string("<absent>") : it->second;
}
static bool has(const std::map<std::string, std::string> &m, const char *k) {
  return m.count(k) > 0;
}

// ------------------------------------------------------- REGRESSION GUARD --

static void guard_gamepad_event() {
  auto ev = base_event(gamepad(), "add");

  assert(prop(ev, "ACTION") == "add");
  assert(prop(ev, "SUBSYSTEM") == "input");
  assert(prop(ev, "DEVNAME") == "/dev/input/event31");
  assert(prop(ev, "DEVPATH") == "/devices/virtual/input/input42/event31");
  assert(prop(ev, "MAJOR") == "13");
  assert(prop(ev, "MINOR") == "31");
  assert(prop(ev, "ID_INPUT") == "1");
  assert(prop(ev, "ID_INPUT_JOYSTICK") == "1");
  assert(prop(ev, "ID_SERIAL") == "noserial");
  assert(prop(ev, "TAGS") == ":seat:uaccess:");
  assert(prop(ev, "CURRENT_TAGS") == ":seat:uaccess:");
  assert(has(ev, "USEC_INITIALIZED")); // value is a timestamp, so presence only
  assert(has(ev, "SEQNUM"));
  ok("GUARD: gamepad add-event carries every property SDL/Steam key off today");

  // Nothing extra crept in: 13 keys exactly. A stray property is as much a regression as a
  // missing one, since subscribers match on the whole set.
  assert(ev.size() == 13);
  ok("GUARD: gamepad add-event has exactly 13 properties, no more");

  auto rm = base_event(gamepad(), "remove");
  assert(prop(rm, "ACTION") == "remove");
  assert(prop(rm, "DEVNAME") == "/dev/input/event31");
  assert(rm.size() == ev.size());
  ok("GUARD: remove-event mirrors add except ACTION");
}

static void guard_gamepad_hwdb() {
  assert(hwdb_path(gamepad()) == "/run/udev/data/c13:31");
  ok("GUARD: hwdb path is /run/udev/data/c<major>:<minor>");

  const std::string expected = "E:ID_INPUT=1\n"
                               "E:ID_INPUT_JOYSTICK=1\n"
                               "E:ID_BUS=usb\n"
                               "G:seat\n"
                               "G:uaccess\n"
                               "Q:seat\n"
                               "Q:uaccess\n"
                               "V:1\n";
  assert(hwdb_contents(gamepad()) == expected);
  ok("GUARD: hwdb contents are byte-identical to the working gamepad entry");
}

static void guard_class_props() {
  Device d = gamepad();
  assert(class_props(d) == std::vector<std::string>{"ID_INPUT_JOYSTICK"});
  d.id_input_class = "mouse";
  assert(class_props(d) == std::vector<std::string>{"ID_INPUT_MOUSE"});
  d.id_input_class = "keyboard";
  assert((class_props(d) == std::vector<std::string>{"ID_INPUT_KEYBOARD", "ID_INPUT_KEY"}));
  d.id_input_class = "something-else";
  assert(class_props(d) == std::vector<std::string>{"ID_INPUT_JOYSTICK"});
  ok("GUARD: joystick/mouse/keyboard classification unchanged (unknown falls back to joystick)");
}

static void guard_serialization() {
  std::map<std::string, std::string> m = {{"B", "2"}, {"A", "1"}};
  auto s = serialize_props(m);
  // NUL-separated, trailing NUL, and std::map means sorted-by-key order.
  const char expected[] = "A=1\0B=2\0";
  assert(s.size() == sizeof(expected) - 1);
  assert(std::memcmp(s.data(), expected, s.size()) == 0);
  ok("GUARD: props serialize NUL-separated with a trailing NUL, key-sorted");

  // A value containing '=' must survive -- udev splits on the FIRST '=' only.
  auto eq = serialize_props({{"K", "a=b"}});
  assert(std::string(eq.data()) == "K=a=b");
  ok("GUARD: a value containing '=' round-trips intact");
}

// murmur2 is proven correct by the fact that the uinput gamepad path works in production today;
// these vectors freeze it so a refactor cannot silently perturb it. A wrong hash here is invisible
// at runtime -- the kernel filter drops the message and nothing logs an error.
static void guard_murmur() {
  assert(murmur2("input", 5, 0) == 0xc1a28470u);
  ok("GUARD: murmur2(\"input\") matches the value the working path relies on");

  // Same function, other subsystems -- needed once fake_udev announces USB/IP devices.
  assert(murmur2("hidraw", 6, 0) == 0xc2caf397u);
  assert(murmur2("usb", 3, 0) == 0x0577c5e5u);
  assert(murmur2("hid", 3, 0) == 0x7088c98au);
  assert(murmur2("usb_device", 10, 0) == 0x27f8f50cu);
  assert(murmur2("usb_interface", 13, 0) == 0xb1024765u);
  ok("murmur2 vectors for the usb/hidraw subsystems");

  // Sanity on the algorithm itself: seed and length both participate.
  assert(murmur2("input", 5, 0) != murmur2("input", 5, 1));
  assert(murmur2("", 0, 0) != murmur2("a", 1, 0));
  assert(murmur2("abcd", 4, 0) != murmur2("abcde", 5, 0));
  ok("murmur2 is sensitive to seed and length (tail-handling exercised)");
}

// --------------------------------------------------- generalized behaviour --

// A hidraw node on the imported Steam Controller Puck. Real shape, from this host: hidraw is a
// dynamically-allocated char major (243 here), and the node hangs off the HID device under the
// USB interface.
static Device hidraw_node() {
  Device d;
  d.subsystem = "hidraw";
  d.devnode = "/dev/hidraw3";
  d.syspath = "/devices/platform/vhci_hcd.0/usb5/5-1/5-1:1.2/0003:28DE:1304.000B/hidraw/hidraw3";
  d.sysname = "hidraw3";
  d.major = 243;
  d.minor = 3;
  return d;
}

static void test_hidraw_event() {
  auto ev = base_event(hidraw_node(), "add");
  assert(prop(ev, "SUBSYSTEM") == "hidraw");
  assert(prop(ev, "DEVNAME") == "/dev/hidraw3");
  assert(prop(ev, "MAJOR") == "243");
  assert(prop(ev, "MINOR") == "3");

  // The evdev block must NOT leak onto a hidraw device -- libinput would try to claim it.
  assert(!has(ev, "ID_INPUT"));
  assert(!has(ev, "ID_INPUT_JOYSTICK"));
  assert(!has(ev, "TAGS"));
  assert(!has(ev, "CURRENT_TAGS"));
  assert(!has(ev, "ID_SERIAL"));
  assert(!has(ev, "DEVTYPE")); // hidraw has no devtype
  ok("hidraw event omits the evdev-only ID_INPUT/TAGS block");

  assert(hwdb_path(hidraw_node()) == "/run/udev/data/c243:3");
  ok("hidraw hwdb path uses the dynamic major (243), not a hardcoded 13");
}

static void test_usb_device_event() {
  Device d;
  d.subsystem = "usb";
  d.devtype = "usb_device";
  d.devnode = "/dev/bus/usb/005/002";
  d.syspath = "/devices/platform/vhci_hcd.0/usb5/5-1";
  d.sysname = "5-1";
  d.major = 189;
  d.minor = 513;

  auto ev = base_event(d, "add");
  assert(prop(ev, "SUBSYSTEM") == "usb");
  assert(prop(ev, "DEVTYPE") == "usb_device");
  assert(prop(ev, "DEVNAME") == "/dev/bus/usb/005/002");
  assert(prop(ev, "MAJOR") == "189");
  assert(!has(ev, "ID_INPUT"));
  ok("usb_device event carries DEVTYPE and the usbfs node");

  assert(hwdb_path(d) == "/run/udev/data/c189:513");
  ok("usb_device hwdb path is c<major>:<minor> (it has a node)");
}

static void test_usb_interface_has_no_node() {
  Device d;
  d.subsystem = "usb";
  d.devtype = "usb_interface";
  d.syspath = "/devices/platform/vhci_hcd.0/usb5/5-1/5-1:1.2";
  d.sysname = "5-1:1.2";
  // deliberately no devnode

  assert(!d.has_node());
  auto ev = base_event(d, "add");
  assert(prop(ev, "DEVTYPE") == "usb_interface");
  assert(prop(ev, "DEVPATH") == d.syspath);
  // Emitting these would describe a node that does not exist.
  assert(!has(ev, "DEVNAME"));
  assert(!has(ev, "MAJOR"));
  assert(!has(ev, "MINOR"));
  ok("usb_interface omits DEVNAME/MAJOR/MINOR entirely");

  // udev names nodeless entries "+<subsystem>:<sysname>".
  assert(hwdb_path(d) == "/run/udev/data/+usb:5-1:1.2");
  ok("nodeless hwdb entry is +usb:<sysname>, matching udev's own naming");
}

static void test_extra_props_and_override() {
  auto d = hidraw_node();
  d.extra_props = {{"ID_VENDOR_ID", "28de"}, {"ID_MODEL_ID", "1304"}};
  auto ev = base_event(d, "add");
  assert(prop(ev, "ID_VENDOR_ID") == "28de");
  assert(prop(ev, "ID_MODEL_ID") == "1304");
  ok("extra_props are merged into the event");

  // Merged last, so they win -- that's what makes them useful for correcting a computed value.
  d.extra_props = {{"SUBSYSTEM", "overridden"}};
  assert(prop(base_event(d, "add"), "SUBSYSTEM") == "overridden");
  ok("extra_props override computed properties (merged last)");

  // hwdb_override replaces the synthesized entry wholesale, for copying the host's real one.
  d.extra_props.clear();
  d.hwdb_override = "E:ID_VENDOR_ID=28de\nE:ID_SERIAL_SHORT=FXB99603011D0\nG:uaccess\nV:1\n";
  assert(hwdb_contents(d) == d.hwdb_override);
  ok("hwdb_override is used verbatim instead of the synthesized entry");
}

static void test_seqnum_increases() {
  // Was hardcoded "7". We now emit 5-10 events per imported device, and subscribers may order
  // or dedupe on this.
  auto a = base_event(hidraw_node(), "add");
  auto b = base_event(hidraw_node(), "add");
  auto c = base_event(hidraw_node(), "remove");
  assert(std::stoul(prop(a, "SEQNUM")) < std::stoul(prop(b, "SEQNUM")));
  assert(std::stoul(prop(b, "SEQNUM")) < std::stoul(prop(c, "SEQNUM")));
  ok("SEQNUM strictly increases across events");
}

int main() {
  guard_gamepad_event();
  guard_gamepad_hwdb();
  guard_class_props();
  guard_serialization();
  guard_murmur();

  test_hidraw_event();
  test_usb_device_event();
  test_usb_interface_has_no_node();
  test_extra_props_and_override();
  test_seqnum_increases();

  std::cout << "\nALL FAKE-UDEV TESTS PASSED\n";
  return 0;
}
