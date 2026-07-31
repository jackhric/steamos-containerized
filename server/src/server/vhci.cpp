// Pure half of the vhci driver: status-table parsing and attach-argument formatting.
// Deliberately free of syscalls and logging so test_vhci links against nothing and runs
// anywhere, with vhci_hcd unloaded. The I/O half is vhci_io.cpp.

#include <server/vhci.hpp>

#include <charconv>

namespace vhci {

namespace {

// from_chars rather than stoi: no exceptions, no locale, and it rejects trailing junk, which
// matters because the sockfd column has held two different formats across kernel versions.
template <typename T> bool parse_int(std::string_view tok, T &out, int base = 10) {
  if (tok.empty())
    return false;
  T v{};
  auto *first = tok.data();
  auto *last = tok.data() + tok.size();
  auto res = std::from_chars(first, last, v, base);
  if (res.ec != std::errc() || res.ptr != last)
    return false;
  out = v;
  return true;
}

// Splits on runs of spaces/tabs. Returns at most `max` tokens.
std::vector<std::string_view> split_ws(std::string_view line, std::size_t max) {
  std::vector<std::string_view> out;
  std::size_t i = 0;
  while (i < line.size() && out.size() < max) {
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
      i++;
    if (i >= line.size())
      break;
    std::size_t start = i;
    while (i < line.size() && line[i] != ' ' && line[i] != '\t')
      i++;
    out.push_back(line.substr(start, i - start));
  }
  return out;
}

bool parse_row(std::string_view line, PortRow &out) {
  auto t = split_ws(line, 7);
  if (t.size() < 7)
    return false;

  if (t[0] == "hs")
    out.super_speed = false;
  else if (t[0] == "ss")
    out.super_speed = true;
  else
    return false; // header line, or a format we don't recognise

  if (!parse_int(t[1], out.port))
    return false;
  if (!parse_int(t[2], out.status))
    return false;
  if (!parse_int(t[3], out.speed))
    return false;
  if (!parse_int(t[4], out.devid, 16))
    return false;

  // Modern kernels print sockfd as %u; older ones printed the kernel pointer as %16p. We only
  // ever use this for logging, so a value we can't parse is not a reason to drop the row.
  if (!parse_int(t[5], out.sockfd))
    out.sockfd = -1;

  out.local_busid = std::string(t[6]);
  return true;
}

} // namespace

std::vector<PortRow> parse_status(std::string_view text) {
  std::vector<PortRow> rows;
  std::size_t pos = 0;
  while (pos <= text.size()) {
    auto nl = text.find('\n', pos);
    auto line = text.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
    PortRow row;
    if (parse_row(line, row))
      rows.push_back(std::move(row));
    if (nl == std::string_view::npos)
      break;
    pos = nl + 1;
  }
  return rows;
}

std::uint32_t clamp_speed(std::uint32_t speed) {
  return speed == usbip::SPEED_SUPER_PLUS ? usbip::SPEED_SUPER : speed;
}

std::optional<int> find_free_port(const std::vector<PortRow> &rows, std::uint32_t speed) {
  const bool want_ss = clamp_speed(speed) >= usbip::SPEED_SUPER;
  for (const auto &r : rows)
    if (r.super_speed == want_ss && r.is_free())
      return r.port;
  return std::nullopt;
}

std::string format_attach(int port, int sockfd, std::uint32_t devid, std::uint32_t speed) {
  return std::to_string(port) + " " + std::to_string(sockfd) + " " + std::to_string(devid) + " " +
         std::to_string(clamp_speed(speed));
}

} // namespace vhci
