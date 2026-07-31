// Does libudev actually RECEIVE what fake_udev sends?
//
// The unit tests prove murmur2 computes the right numbers. They cannot prove we put them in the
// right header field, or that libudev's socket filter passes them -- and that failure is
// completely silent: the kernel drops a mis-hashed message before any subscriber sees it, nothing
// is logged, and the device simply never appears in Steam. This test closes that gap.
//
// It uses real libudev as the subscriber rather than a hand-rolled BPF filter, because
// reconstructing systemd's filter from memory would risk testing a wrong reconstruction against a
// wrong implementation. libudev is also literally what SDL uses, and SDL dlopens it exactly like
// this (the builder image ships libudev.so.1 but no header).
//
// Needs CAP_NET_ADMIN to multicast to the udev monitor group, and root (libudev drops messages
// whose sender uid != 0). No GPU, no /dev/uinput.

#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <poll.h>
#include <server/fake_udev.hpp>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

int failures = 0;
void ok(const std::string &what) { std::cout << "[ OK ] " << what << "\n"; }
void fail(const std::string &what) {
  std::cout << "[FAIL] " << what << "\n";
  failures++;
}

struct udev;
struct udev_monitor;
struct udev_device;

// The handful of libudev entry points we need. Resolved at runtime so no -ludev / libudev.h.
struct Udev {
  void *lib = nullptr;
  udev *(*new_)() = nullptr;
  udev_monitor *(*monitor_new)(udev *, const char *) = nullptr;
  int (*filter_add)(udev_monitor *, const char *, const char *) = nullptr;
  int (*enable)(udev_monitor *) = nullptr;
  int (*get_fd)(udev_monitor *) = nullptr;
  udev_device *(*receive)(udev_monitor *) = nullptr;
  const char *(*get_action)(udev_device *) = nullptr;
  const char *(*get_property)(udev_device *, const char *) = nullptr;
  void (*device_unref)(udev_device *) = nullptr;
  void (*monitor_unref)(udev_monitor *) = nullptr;

  bool load() {
    lib = ::dlopen("libudev.so.1", RTLD_NOW);
    if (!lib)
      return false;
    auto sym = [&](const char *n) { return ::dlsym(lib, n); };
    new_ = (decltype(new_))sym("udev_new");
    monitor_new = (decltype(monitor_new))sym("udev_monitor_new_from_netlink");
    filter_add = (decltype(filter_add))sym("udev_monitor_filter_add_match_subsystem_devtype");
    enable = (decltype(enable))sym("udev_monitor_enable_receiving");
    get_fd = (decltype(get_fd))sym("udev_monitor_get_fd");
    receive = (decltype(receive))sym("udev_monitor_receive_device");
    get_action = (decltype(get_action))sym("udev_device_get_action");
    get_property = (decltype(get_property))sym("udev_device_get_property_value");
    device_unref = (decltype(device_unref))sym("udev_device_unref");
    monitor_unref = (decltype(monitor_unref))sym("udev_monitor_unref");
    return new_ && monitor_new && filter_add && enable && get_fd && receive && get_action &&
           get_property && device_unref && monitor_unref;
  }
};

Udev U;

// A subscriber filtered exactly the way SDL filters: subsystem, optional devtype.
struct Subscriber {
  udev_monitor *mon = nullptr;
  int fd = -1;

  bool open(udev *ctx, const char *subsystem, const char *devtype) {
    mon = U.monitor_new(ctx, "udev"); // "udev" = the rebroadcast group, group 2
    if (!mon)
      return false;
    if (U.filter_add(mon, subsystem, devtype) < 0)
      return false;
    int rc = U.enable(mon);
    if (rc < 0) {
      std::cout << "       (udev_monitor_enable_receiving -> " << rc << ")\n";
      return false;
    }
    fd = U.get_fd(mon);
    return fd >= 0;
  }
  // Returns the received device, or nullptr on timeout.
  // Distinguishes "nothing arrived" from "arrived but libudev discarded it" -- very different
  // bugs, and the failure message must not guess.
  udev_device *wait(int timeout_ms, const char **why = nullptr) {
    pollfd p{fd, POLLIN, 0};
    int pr = ::poll(&p, 1, timeout_ms);
    if (pr == 0) {
      if (why) *why = "poll timed out: the message never reached the socket (filter dropped it)";
      return nullptr;
    }
    if (pr < 0) {
      if (why) *why = "poll error";
      return nullptr;
    }
    udev_device *d = U.receive(mon);
    if (!d && why)
      *why = "poll fired but udev_monitor_receive_device() returned NULL (libudev discarded it)";
    return d;
  }
  void close() {
    if (mon)
      U.monitor_unref(mon);
    mon = nullptr;
  }
};

std::string prop(udev_device *d, const char *k) {
  const char *v = U.get_property(d, k);
  return v ? v : "<absent>";
}

fake_udev::Device gamepad() {
  fake_udev::Device d;
  d.devnode = "/dev/input/event31";
  d.syspath = "/devices/virtual/input/input42/event31";
  d.major = 13;
  d.minor = 31;
  return d;
}

fake_udev::Device hidraw() {
  fake_udev::Device d;
  d.subsystem = "hidraw";
  d.devnode = "/dev/hidraw3";
  d.syspath = "/devices/platform/vhci_hcd.0/usb5/5-1/5-1:1.2/0003:28DE:1304.000B/hidraw/hidraw3";
  d.sysname = "hidraw3";
  d.major = 243;
  d.minor = 3;
  return d;
}

fake_udev::Device usb_device() {
  fake_udev::Device d;
  d.subsystem = "usb";
  d.devtype = "usb_device";
  d.devnode = "/dev/bus/usb/005/002";
  d.syspath = "/devices/platform/vhci_hcd.0/usb5/5-1";
  d.sysname = "5-1";
  d.major = 189;
  d.minor = 513;
  d.extra_props = {{"ID_VENDOR_ID", "28de"}, {"ID_MODEL_ID", "1304"}};
  return d;
}

fake_udev::Device usb_interface() {
  fake_udev::Device d;
  d.subsystem = "usb";
  d.devtype = "usb_interface";
  d.syspath = "/devices/platform/vhci_hcd.0/usb5/5-1/5-1:1.2";
  d.sysname = "5-1:1.2";
  return d;
}

// Send one device, expect a subscriber filtered on (subsystem, devtype) to receive it.
void expect_received(udev *ctx, const char *subsystem, const char *devtype,
                     const fake_udev::Device &dev, const std::string &label) {
  Subscriber sub;
  if (!sub.open(ctx, subsystem, devtype)) {
    fail(label + ": could not open subscriber");
    return;
  }
  ::usleep(50 * 1000); // let the subscriber's group join settle before multicasting
  fake_udev::plug(dev);

  const char *why = "?";
  udev_device *got = sub.wait(2000, &why);
  if (!got) {
    fail(label + ": subscriber on " + subsystem + "/" + (devtype ? devtype : "(any)") + " -- " +
         why);
    sub.close();
    return;
  }

  bool good = true;
  auto check = [&](const char *key, const std::string &want) {
    auto have = prop(got, key);
    if (have != want) {
      fail(label + ": " + key + " = '" + have + "', expected '" + want + "'");
      good = false;
    }
  };
  check("SUBSYSTEM", dev.subsystem);
  check("DEVPATH", dev.syspath);
  if (dev.has_node())
    check("DEVNAME", dev.devnode);
  if (!dev.devtype.empty())
    check("DEVTYPE", dev.devtype);

  const char *action = U.get_action(got);
  if (!action || std::string(action) != "add") {
    fail(label + ": ACTION was not 'add'");
    good = false;
  }
  for (const auto &[k, v] : dev.extra_props)
    check(k.c_str(), v);

  if (good)
    ok(label + ": received by a libudev subscriber with all properties intact");

  U.device_unref(got);
  sub.close();
}

// The control that makes the positives meaningful: a subscriber filtered on one subsystem must
// NOT receive a different one. Without this, every test above would pass just as happily if the
// filter were broken open and passing everything.
void expect_filtered_out(udev *ctx, const char *subsystem, const fake_udev::Device &dev,
                         const std::string &label) {
  Subscriber sub;
  if (!sub.open(ctx, subsystem, nullptr)) {
    fail(label + ": could not open subscriber");
    return;
  }
  ::usleep(50 * 1000);
  fake_udev::plug(dev);
  udev_device *got = sub.wait(700);
  if (got) {
    fail(label + ": subscriber on '" + std::string(subsystem) + "' ALSO received a '" +
         dev.subsystem + "' event -- the filter is not discriminating, so the positive results " +
         "above prove nothing");
    U.device_unref(got);
  } else {
    ok(label + ": correctly NOT delivered to a subscriber on '" + subsystem + "'");
  }
  sub.close();
}

} // namespace

// PRODUCTION CONSTRAINT, found by this test: libudev binds a monitor to the udev group only if
// /run/udev/control exists AT MONITOR-CREATION TIME. A monitor created before it exists is
// permanently deaf -- no later event ever reaches it, and nothing reports an error.
//
// That is a live hazard: Steam/SDL creates its monitor at startup. Today we get away with it
// because fake_udev::ensure_udev_control() runs on the first plug() and MediaSession cold-plugs
// gamepad 0 before launching Steam -- i.e. by luck of ordering, not by design. The entrypoint
// creates /run/udev/data but not the control file, so it must be created there too.
//
// Runs FIRST, while no plug() has yet created the file.
void pin_control_file_ordering(udev *ctx) {
  std::error_code ec;
  std::filesystem::remove("/run/udev/control", ec);

  Subscriber deaf;
  bool opened = deaf.open(ctx, "input", nullptr); // created with NO control file present
  ::usleep(50 * 1000);
  fake_udev::plug(gamepad());                     // this call creates the control file
  udev_device *got = opened ? deaf.wait(700) : nullptr;
  if (got) {
    // Not a failure -- it would mean the hazard does not exist on this libudev. Say so loudly
    // rather than silently keeping a test that proves nothing.
    ok("NOTE: a monitor created before /run/udev/control still receives on this libudev "
       "(the ordering hazard may not apply here)");
    U.device_unref(got);
  } else {
    ok("PINNED: a monitor created before /run/udev/control exists is permanently deaf "
       "-- the entrypoint must create that file before Steam starts");
  }
  deaf.close();

  // Everything after this point assumes the file exists, exactly as the entrypoint will ensure.
  std::filesystem::create_directories("/run/udev", ec);
  if (!std::filesystem::exists("/run/udev/control", ec))
    std::ofstream("/run/udev/control").close();
}

int main() {
  if (::geteuid() != 0) {
    std::cout << "[SKIP] needs root (libudev drops messages from a non-root sender)\n";
    return 77; // ctest "skipped"
  }
  if (!U.load()) {
    std::cout << "[SKIP] libudev.so.1 not available: " << (::dlerror() ?: "?") << "\n";
    return 77;
  }
  udev *ctx = U.new_();
  if (!ctx) {
    std::cout << "[FAIL] udev_new() returned null\n";
    return 1;
  }

  pin_control_file_ordering(ctx);

  // The path that already works in production -- if this breaks, so has the gamepad.
  expect_received(ctx, "input", nullptr, gamepad(), "input/joystick");

  // The new subsystems.
  expect_received(ctx, "hidraw", nullptr, hidraw(), "hidraw");
  expect_received(ctx, "usb", "usb_device", usb_device(), "usb/usb_device");
  expect_received(ctx, "usb", "usb_interface", usb_interface(), "usb/usb_interface");

  // A subsystem-only filter must still match a device that carries a devtype.
  expect_received(ctx, "usb", nullptr, usb_device(), "usb (subsystem-only filter)");

  // Negative controls.
  expect_filtered_out(ctx, "hidraw", gamepad(), "input event vs hidraw filter");
  expect_filtered_out(ctx, "input", hidraw(), "hidraw event vs input filter");

  // Same as the very first case. If this passes and the first failed, the problem is the first
  // call, not the input subsystem.
  expect_received(ctx, "input", nullptr, gamepad(), "input/joystick (repeat, last)");

  std::cout << "\n";
  if (failures) {
    std::cout << failures << " FAILURE(S) -- fake_udev netlink delivery is broken\n";
    return 1;
  }
  std::cout << "ALL FAKE-UDEV NETLINK TESTS PASSED\n";
  return 0;
}
