// I/O half of the vhci driver: the sysfs reads and writes. Kept apart from vhci.cpp so the
// parsing logic stays testable without the module loaded or root privileges.

#include <server/vhci.hpp>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <helpers/logger.hpp>
#include <sstream>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace vhci {

namespace {

std::string attr_path(const char *name) { return std::string(kSysfsBase) + "/" + name; }

std::optional<std::string> read_attr(const char *name) {
  std::ifstream f(attr_path(name));
  if (!f)
    return std::nullopt;
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// sysfs attributes take the whole value in a single write(); a partial or split write is an
// error, not something to retry.
bool write_attr(const char *name, const std::string &value) {
  auto path = attr_path(name);
  int fd = ::open(path.c_str(), O_WRONLY | O_CLOEXEC);
  if (fd < 0) {
    logs::log(logs::warning, "[VHCI] cannot open {}: {}", path, std::strerror(errno));
    return false;
  }
  auto n = ::write(fd, value.data(), value.size());
  int err = errno;
  ::close(fd);
  if (n != static_cast<ssize_t>(value.size())) {
    logs::log(logs::warning, "[VHCI] write '{}' -> {} failed: {}", value, path,
              n < 0 ? std::strerror(err) : "short write");
    return false;
  }
  return true;
}

} // namespace

bool available() {
  struct stat st {};
  if (::stat(kSysfsBase, &st) != 0)
    return false;
  // Presence is not enough: Docker mounts /sys read-only for non-privileged containers, so the
  // module can be loaded and attach still be unwritable until the entrypoint remounts it.
  return ::access(attr_path("attach").c_str(), W_OK) == 0;
}

std::optional<std::vector<PortRow>> read_status() {
  auto text = read_attr("status");
  if (!text)
    return std::nullopt;
  auto rows = parse_status(*text);

  // Multiple virtual controllers expose status.1, status.2, ... Absent on a default build
  // (VHCI_NR_HCS=1), so a missing file just ends the walk.
  for (int i = 1;; i++) {
    auto name = "status." + std::to_string(i);
    auto extra = read_attr(name.c_str());
    if (!extra)
      break;
    auto more = parse_status(*extra);
    rows.insert(rows.end(), more.begin(), more.end());
  }
  return rows;
}

bool attach(int port, int sockfd, std::uint32_t devid, std::uint32_t speed) {
  return write_attr("attach", format_attach(port, sockfd, devid, speed));
}

bool detach(int port) { return write_attr("detach", std::to_string(port)); }

bool wait_port_free(int port, int timeout_ms) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  for (;;) {
    auto rows = read_status();
    if (rows) {
      bool busy = false;
      for (const auto &r : *rows)
        if (r.port == port && !r.is_free())
          busy = true;
      if (!busy)
        return true;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      logs::log(logs::warning, "[VHCI] port {} still in use {}ms after detach", port, timeout_ms);
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

std::optional<std::string> wait_local_busid(int port, int timeout_ms) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  for (;;) {
    auto rows = read_status();
    if (rows) {
      for (const auto &r : *rows) {
        if (r.port != port)
          continue;
        if (r.enumerated())
          return r.local_busid;
        if (r.status == ST_ERROR) {
          logs::log(logs::warning, "[VHCI] port {} entered ST_ERROR during enumeration", port);
          return std::nullopt;
        }
        break;
      }
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      logs::log(logs::warning, "[VHCI] port {} did not enumerate within {}ms", port, timeout_ms);
      return std::nullopt;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
}

} // namespace vhci
