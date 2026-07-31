// USB sysfs discovery, against an in-memory tree modelled on what Step 0 actually produced when
// the Steam Controller Puck was imported through vhci: busid 5-1, 7 interfaces (cdc_acm x2,
// usbhid x5), 5 hidraw nodes on the dynamic major 243.
//
// Pure: no /sys, no devices, no root.

#include <cassert>
#include <iostream>
#include <map>
#include <server/usb_discovery.hpp>
#include <iterator>
#include <set>
#include <string>
#include <vector>

using namespace usbip;

static void ok(const char *what) { std::cout << "[ OK ] " << what << "\n"; }

// Files are exact contents; directories are implied by their children's paths.
class FakeSysfs : public SysfsReader {
public:
  std::map<std::string, std::string> files;
  std::set<std::string> dirs;

  void file(const std::string &p, const std::string &v) {
    files[p] = v;
    // every ancestor directory exists
    auto q = p;
    while (true) {
      auto slash = q.find_last_of('/');
      if (slash == std::string::npos || slash == 0)
        break;
      q = q.substr(0, slash);
      dirs.insert(q);
    }
  }
  void dir(const std::string &p) { dirs.insert(p); }

  bool read(const std::string &path, std::string &out) const override {
    auto it = files.find(path);
    if (it == files.end())
      return false;
    out = it->second;
    return true;
  }
  bool exists(const std::string &path) const override {
    return files.count(path) || dirs.count(path);
  }
  std::vector<std::string> list_dir(const std::string &path) const override {
    std::set<std::string> kids;
    auto add_child = [&](const std::string &full) {
      if (full.size() <= path.size() + 1 || full.compare(0, path.size(), path) != 0 ||
          full[path.size()] != '/')
        return;
      auto rest = full.substr(path.size() + 1);
      auto slash = rest.find('/');
      kids.insert(slash == std::string::npos ? rest : rest.substr(0, slash));
    };
    for (const auto &[f, _] : files)
      add_child(f);
    for (const auto &d : dirs)
      add_child(d);
    return {kids.begin(), kids.end()};
  }
};

static const char *kRoot = "/sys/devices/platform/vhci_hcd.0/usb5";

// The Puck as Step 0 produced it. Values are the real ones read off this host.
static FakeSysfs puck_tree(bool with_hidraw = true, bool with_drivers = true) {
  FakeSysfs fs;
  const std::string d = std::string(kRoot) + "/5-1";
  fs.file(d + "/busnum", "1\n");
  fs.file(d + "/devnum", "2\n");
  fs.file(d + "/idVendor", "28de\n");
  fs.file(d + "/idProduct", "1304\n");
  fs.file(d + "/bNumInterfaces", " 7\n"); // sysfs space-pads this one
  fs.file(d + "/dev", "189:1\n");

  // 5-1:1.0 and 5-1:1.1 are the CDC ACM pair; 1.2-1.6 are HID.
  for (int i = 0; i < 7; i++) {
    std::string iface = d + "/5-1:1." + std::to_string(i);
    fs.dir(iface);
    if (with_drivers)
      fs.file(iface + "/driver_name", i < 2 ? "cdc_acm" : "usbhid");
    if (i >= 2 && with_hidraw) {
      // HID function dir, then the hidraw node under it. Numbers are the real ones: major 243,
      // minors 0-4 -- note they start at 0 because binding freed the originals.
      std::string hid = iface + "/0003:28DE:1304.000" + std::to_string(0xB + i);
      fs.file(hid + "/hidraw/hidraw" + std::to_string(i - 2) + "/dev",
              "243:" + std::to_string(i - 2) + "\n");
    }
  }
  return fs;
}

static void test_usbfs_path_padding() {
  // Zero-padded to three digits each. "/dev/bus/usb/5/2" would simply not exist.
  assert(usbfs_path(5, 2) == "/dev/bus/usb/005/002");
  assert(usbfs_path(1, 11) == "/dev/bus/usb/001/011");
  assert(usbfs_path(12, 345) == "/dev/bus/usb/012/345");
  ok("usbfs path is zero-padded to %03u/%03u");
}

static void test_parse_dev_file() {
  unsigned maj = 0, min = 0;
  assert(parse_dev_file("243:3\n", maj, min) && maj == 243 && min == 3);
  assert(parse_dev_file("189:513", maj, min) && maj == 189 && min == 513);
  assert(parse_dev_file("13:76\n", maj, min) && maj == 13 && min == 76);
  ok("dev files parse to major:minor");

  assert(!parse_dev_file("", maj, min));
  assert(!parse_dev_file("243", maj, min));
  assert(!parse_dev_file(":3", maj, min));
  assert(!parse_dev_file("243:", maj, min));
  assert(!parse_dev_file("abc:def", maj, min));
  assert(!parse_dev_file("243:3:9", maj, min));
  ok("malformed dev files are rejected rather than half-parsed");
}

static void test_discover_puck() {
  auto fs = puck_tree();
  DiscoveredDevice d;
  assert(discover(fs, kRoot, "5-1", d));

  assert(d.busid == "5-1");
  assert(d.busnum == 1 && d.devnum == 2);
  assert(d.id_vendor == 0x28de);  // hex, no 0x prefix in sysfs
  assert(d.id_product == 0x1304);
  assert(d.num_interfaces == 7);  // parsed despite the leading space
  assert(d.syspath == "/devices/platform/vhci_hcd.0/usb5/5-1"); // /sys stripped, as DEVPATH wants
  ok("device attributes parse (hex ids, space-padded bNumInterfaces, DEVPATH form)");

  assert(d.usbfs.major == 189 && d.usbfs.minor == 1);
  ok("usbfs node major:minor comes from the sysfs dev file");

  assert(d.interfaces.size() == 7);
  assert(d.interfaces[0].sysname == "5-1:1.0");
  assert(d.interfaces[0].driver == "cdc_acm");
  assert(d.interfaces[6].sysname == "5-1:1.6");
  assert(d.interfaces[6].driver == "usbhid");
  ok("all 7 interfaces found, with their bound drivers");

  int hidraws = 0;
  for (const auto &i : d.interfaces)
    hidraws += static_cast<int>(i.hidraws.size());
  assert(hidraws == 5); // only the 5 usbhid interfaces produce hidraw; cdc_acm does not
  ok("5 hidraw nodes across the 5 HID interfaces (cdc_acm contributes none)");

  // The number that matters: read from sysfs, not assumed to be 13 or hardcoded 243.
  for (const auto &i : d.interfaces)
    for (const auto &h : i.hidraws)
      assert(h.major == 243);
  assert(d.complete);
  ok("hidraw majors read from sysfs; device reports complete");
}

static void test_incomplete_enumeration() {
  // Interfaces present, drivers bound, but hidraw nodes have not appeared yet. This is the state
  // right after attach, and it must NOT be announced -- SDL would open a node that isn't there.
  DiscoveredDevice d;
  auto no_hid = puck_tree(/*with_hidraw=*/false);
  assert(discover(no_hid, kRoot, "5-1", d));
  assert(d.interfaces.size() == 7);
  int n = 0;
  for (const auto &i : d.interfaces)
    n += static_cast<int>(i.hidraws.size());
  assert(n == 0);
  ok("hidraw nodes absent -> discovery still succeeds, reports zero nodes");

  // Drivers not yet bound -> not complete, keep polling.
  auto no_drv = puck_tree(/*with_hidraw=*/true, /*with_drivers=*/false);
  DiscoveredDevice d2;
  assert(discover(no_drv, kRoot, "5-1", d2));
  assert(d2.interfaces.size() == 7);
  assert(!d2.complete);
  ok("interfaces present but unbound -> complete == false (caller keeps polling)");

  // Only some interfaces have appeared: drop 5-1:1.6 entirely (files AND the dirs its children
  // implied -- erasing just driver_name leaves the interface visible via its hidraw path).
  FakeSysfs partial = puck_tree();
  const std::string gone = std::string(kRoot) + "/5-1/5-1:1.6";
  for (auto it = partial.files.begin(); it != partial.files.end();)
    it = it->first.rfind(gone, 0) == 0 ? partial.files.erase(it) : std::next(it);
  for (auto it = partial.dirs.begin(); it != partial.dirs.end();)
    it = it->rfind(gone, 0) == 0 ? partial.dirs.erase(it) : std::next(it);
  DiscoveredDevice d3;
  assert(discover(partial, kRoot, "5-1", d3));
  assert(d3.interfaces.size() < 7);
  assert(!d3.complete);
  ok("fewer interfaces than bNumInterfaces -> complete == false");
}

static void test_non_hid_device() {
  // Generic USB is in scope: a mass-storage device has no HID interfaces at all and must not
  // crash or be treated as a failure.
  FakeSysfs fs;
  const std::string d = std::string(kRoot) + "/5-2";
  fs.file(d + "/busnum", "5\n");
  fs.file(d + "/devnum", "3\n");
  fs.file(d + "/idVendor", "058f\n");
  fs.file(d + "/idProduct", "6364\n");
  fs.file(d + "/bNumInterfaces", " 1\n");
  fs.file(d + "/dev", "189:1026\n");
  fs.file(d + "/5-2:1.0/driver_name", "usb-storage");

  DiscoveredDevice out;
  assert(discover(fs, kRoot, "5-2", out));
  assert(out.id_vendor == 0x058f && out.id_product == 0x6364);
  assert(out.interfaces.size() == 1);
  assert(out.interfaces[0].driver == "usb-storage");
  assert(out.interfaces[0].hidraws.empty());
  assert(out.complete);
  assert(usbfs_path(out.busnum, out.devnum) == "/dev/bus/usb/005/003");
  ok("a non-HID device (mass storage) discovers cleanly with no hidraw nodes");
}

static void test_missing_device() {
  auto fs = puck_tree();
  DiscoveredDevice d;
  assert(!discover(fs, kRoot, "5-99", d));
  ok("a busid that does not exist returns false");

  // Present but unreadable attributes -> false, not a half-filled struct.
  FakeSysfs bare;
  bare.dir(std::string(kRoot) + "/5-1");
  DiscoveredDevice d2;
  assert(!discover(bare, kRoot, "5-1", d2));
  ok("a device directory with no attributes returns false");
}

int main() {
  test_usbfs_path_padding();
  test_parse_dev_file();
  test_discover_puck();
  test_incomplete_enumeration();
  test_non_hid_device();
  test_missing_device();

  std::cout << "\nALL USB-DISCOVERY TESTS PASSED\n";
  return 0;
}
