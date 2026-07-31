// Drives ImportManager directly, without a Moonlight client or a stream.
//
// attach_for_session() normally fires from MediaSession::start, which needs a real client. This
// exercises the same path standalone: connect to a usbipd, import, attach to vhci, wait for
// enumeration, mknod, announce, then detach and verify the cleanup.
//
// Needs: root (mknod + sysfs writes), vhci_hcd loaded and /sys writable, and a usbipd somewhere
// with the target device bound. Run it INSIDE the container so it sees the container's private
// /dev and its device cgroup rules -- that is the thing under test.
//
//   STEAM_STREAM_USBIP_DIRECT=127.0.0.1:3240 STEAM_STREAM_USBIP_BUSIDS=1-1 ./test_usbip_import

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <server/usb_import.hpp>
#include <server/vhci.hpp>
#include <string>
#include <unistd.h>
#include <vector>

namespace {
int failures = 0;
void ok(const std::string &s) { std::cout << "[ OK ] " << s << "\n"; }
void no(const std::string &s) {
  std::cout << "[FAIL] " << s << "\n";
  failures++;
}

int ports_in_use() {
  auto rows = vhci::read_status();
  if (!rows)
    return -1;
  int n = 0;
  for (const auto &r : *rows)
    if (!r.is_free())
      n++;
  return n;
}

// Every char device node the container can see, so we can diff before/after.
std::vector<std::string> nodes_now() {
  std::vector<std::string> out;
  std::error_code ec;
  // Character devices only. Matching on the path prefix alone also caught /dev/bus/usb/<bus>
  // directories, and iterating /dev and /dev/bus/usb separately double-counted the usbfs nodes.
  for (const auto &e : std::filesystem::recursive_directory_iterator("/dev", ec)) {
    if (!std::filesystem::is_character_file(e.path(), ec))
      continue;
    auto s = e.path().string();
    if (s.rfind("/dev/hidraw", 0) == 0 || s.rfind("/dev/bus/usb/", 0) == 0)
      out.push_back(s);
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}
} // namespace

int main() {
  if (::geteuid() != 0) {
    std::cout << "[SKIP] needs root\n";
    return 77;
  }
  if (!std::getenv("STEAM_STREAM_USBIP_DIRECT")) {
    std::cout << "[SKIP] set STEAM_STREAM_USBIP_DIRECT=host:port and STEAM_STREAM_USBIP_BUSIDS\n";
    return 77;
  }

  auto &m = usbip::ImportManager::instance();
  m.init();
  if (!m.enabled()) {
    no("manager reports disabled -- see the [USBIP] log line above for why");
    return 1;
  }
  ok("manager enabled");

  m.reap_stale();
  const int before_ports = ports_in_use();
  const auto before_nodes = nodes_now();
  std::cout << "       before: " << before_ports << " vhci port(s) in use, " << before_nodes.size()
            << " device node(s)\n";

  // ---- import ----
  m.attach_for_session(/*session_id=*/1, 5000);

  const int after_ports = ports_in_use();
  const auto after_nodes = nodes_now();
  if (after_ports > before_ports)
    ok("vhci port count rose " + std::to_string(before_ports) + " -> " +
       std::to_string(after_ports));
  else
    no("no new vhci port in use -- the import did not attach");

  if (after_nodes.size() > before_nodes.size()) {
    ok("device nodes created: " + std::to_string(after_nodes.size() - before_nodes.size()));
    for (const auto &n : after_nodes) {
      bool is_new = true;
      for (const auto &b : before_nodes)
        if (b == n)
          is_new = false;
      if (!is_new)
        continue;
      // The real test of the cgroup rules: can the node actually be opened?
      int fd = ::open(n.c_str(), O_RDWR);
      if (fd >= 0) {
        ok("  " + n + " is open()able read-write (Steam can send feature reports)");
        ::close(fd);
      } else {
        no("  " + n + " could not be opened: " + std::strerror(errno) +
           (errno == EPERM ? "  <-- device cgroup rule missing for this major" : ""));
      }
    }
  } else {
    no("no new device nodes -- enumeration or mknod failed");
  }

  // ---- detach ----
  m.detach_session(1);
  // detach_session now blocks on wait_port_free, so this should already be settled; poll anyway
  // so a regression reports "took Nms" rather than a bare failure.
  int waited = 0;
  while (ports_in_use() > before_ports && waited < 3000) {
    ::usleep(50 * 1000);
    waited += 50;
  }
  if (waited)
    std::cout << "       (port release took " << waited << "ms)\n";
  const int final_ports = ports_in_use();
  const auto final_nodes = nodes_now();

  if (final_ports == before_ports)
    ok("detach released every vhci port");
  else
    no("vhci ports not released: " + std::to_string(final_ports) + " still in use");

  if (final_nodes.size() == before_nodes.size())
    ok("detach removed every node it created");
  else
    no("nodes left behind after detach");

  std::cout << "\n";
  if (failures) {
    std::cout << failures << " FAILURE(S)\n";
    return 1;
  }
  std::cout << "ALL USBIP IMPORT TESTS PASSED\n";
  return 0;
}
