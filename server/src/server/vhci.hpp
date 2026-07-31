#pragma once

// vhci_hcd control -- the virtual host controller that materializes an imported USB device.
//
// Attaching is three steps: pick a free port on the correct root hub, hand the kernel an
// already-connected stream socket, and write "port sockfd devid speed" to <base>/attach. From
// that point the kernel owns the socket and runs its own rx/tx kthreads on it; we never touch
// the URB framing ourselves.
//
// Two consequences worth knowing before changing anything here:
//   * sockfd is resolved against the WRITING process's fd table, so the attach must be performed
//     by whoever holds the socket. That is why we cannot shell out to the `usbip` binary.
//   * closing our end of that socket is what tears the device down. The protocol has no
//     heartbeat, so this is the liveness mechanism, not a cleanup detail.
//
// Text parsing and argument formatting are pure (vhci.cpp) and split from the syscalls
// (vhci_io.cpp) so the whole state machine is unit-testable with the module unloaded.

#include <cstdint>
#include <optional>
#include <server/usbip_proto.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace vhci {

constexpr const char *kSysfsBase = "/sys/devices/platform/vhci_hcd.0";

// enum usbip_device_status (usbip_common.h). Only the vdev (importer) half applies to us; the
// SDEV_* values 1-3 belong to the exporter side.
enum Status : int {
  ST_NULL = 4,        // free
  ST_NOTASSIGNED = 5, // socket attached, USB address not assigned yet
  ST_USED = 6,
  ST_ERROR = 7,
};

struct PortRow {
  bool super_speed = false; // "ss" root hub rather than "hs"
  int port = -1;
  int status = ST_NULL;
  unsigned speed = 0;
  std::uint32_t devid = 0;
  int sockfd = -1;
  // Busid on OUR side once the kernel finishes enumerating, e.g. "3-1". "0-0" while free.
  std::string local_busid;

  bool is_free() const { return status == ST_NULL; }
  bool enumerated() const { return status == ST_USED && !local_busid.empty() && local_busid != "0-0"; }
};

// ---- pure ----

// Parses the <base>/status table. Unknown/short/malformed lines are skipped rather than fatal:
// the header line is one such, and the format has changed across kernel versions.
std::vector<PortRow> parse_status(std::string_view text);

// Only SuperSpeed devices belong on the ss root hub. Putting one on the wrong hub does not fail
// the attach -- it fails later, obscurely, during enumeration.
std::optional<int> find_free_port(const std::vector<PortRow> &rows, std::uint32_t speed);

// vhci has no SUPER_PLUS support; usbipd-win hits the same wall and downgrades. Do the same
// rather than let the kernel reject an otherwise fine attach.
std::uint32_t clamp_speed(std::uint32_t speed);

// The exact "%u %u %u %u" attach_store expects.
std::string format_attach(int port, int sockfd, std::uint32_t devid, std::uint32_t speed);

// ---- I/O ----

// True when the module is loaded AND <base>/attach is writable. The latter is not a given:
// Docker mounts /sys read-only, so the entrypoint remounts it.
bool available();
std::optional<std::vector<PortRow>> read_status();
bool attach(int port, int sockfd, std::uint32_t devid, std::uint32_t speed);
bool detach(int port);
// local_busid is only populated once enumeration completes, which is asynchronous, so poll.
std::optional<std::string> wait_local_busid(int port, int timeout_ms = 2000);

// Detach is ASYNCHRONOUS: detach_store only queues VDEV_EVENT_DOWN, and a kthread does the
// teardown. Reading status straight after a detach can still show the port in use, so a fast
// detach->attach cycle could otherwise pick a port that is still tearing down. Returns false if
// the port never frees.
bool wait_port_free(int port, int timeout_ms = 2000);

} // namespace vhci
