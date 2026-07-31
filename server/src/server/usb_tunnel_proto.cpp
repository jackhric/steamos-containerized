#include <server/usb_tunnel_proto.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace usbip::tunnel {

namespace {

void put_be32(std::string &s, std::uint32_t v) {
  s.push_back(static_cast<char>((v >> 24) & 0xFF));
  s.push_back(static_cast<char>((v >> 16) & 0xFF));
  s.push_back(static_cast<char>((v >> 8) & 0xFF));
  s.push_back(static_cast<char>(v & 0xFF));
}

std::uint32_t get_be32(const std::uint8_t *p) {
  return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
         (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

std::string_view trim(std::string_view s) {
  auto ws = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
  while (!s.empty() && ws(s.front()))
    s.remove_prefix(1);
  while (!s.empty() && ws(s.back()))
    s.remove_suffix(1);
  return s;
}

// Single-line payloads still arrive with the trailing newline the encoder wrote, and a future
// sender may append lines we do not know about. Take the first line and ignore the rest.
std::map<std::string, std::string> first_line_kv(std::string_view payload) {
  auto rows = parse_kv_lines(payload);
  return rows.empty() ? std::map<std::string, std::string>{} : rows.front();
}

bool valid_channel(std::uint8_t c) {
  return c == static_cast<std::uint8_t>(Channel::CONTROL) || c == static_cast<std::uint8_t>(Channel::DATA);
}

} // namespace

std::string encode_preamble(const Preamble &p) {
  std::string s;
  s.reserve(kPreambleSize);
  s.append(kMagic, sizeof(kMagic));
  s.push_back(static_cast<char>(p.version));
  s.push_back(static_cast<char>(p.channel));
  s.push_back('\0');
  s.push_back('\0');
  put_be32(s, p.token);
  return s;
}

bool decode_preamble(const void *buf, std::size_t len, Preamble &out) {
  if (len < kPreambleSize)
    return false;
  const auto *p = static_cast<const std::uint8_t *>(buf);
  if (std::memcmp(p, kMagic, sizeof(kMagic)) != 0)
    return false;
  if (p[4] != kVersion)
    return false;
  if (!valid_channel(p[5]))
    return false;
  out.version = p[4];
  out.channel = static_cast<Channel>(p[5]);
  out.token = get_be32(p + 8);
  return true;
}

std::string encode_frame(FrameType type, std::string_view payload) {
  std::string s;
  auto n = payload.size() > kMaxPayload ? kMaxPayload : payload.size();
  s.reserve(kFrameHeaderSize + n);
  s.push_back(static_cast<char>(type));
  s.push_back('\0');
  s.push_back(static_cast<char>((n >> 8) & 0xFF));
  s.push_back(static_cast<char>(n & 0xFF));
  s.append(payload.data(), n);
  return s;
}

bool FrameDecoder::feed(const void *buf, std::size_t len, std::vector<Frame> &out) {
  buf_.append(static_cast<const char *>(buf), len);
  for (;;) {
    if (buf_.size() < kFrameHeaderSize)
      return true;
    auto type = static_cast<std::uint8_t>(buf_[0]);
    if (type < static_cast<std::uint8_t>(FrameType::HELLO) || type > static_cast<std::uint8_t>(FrameType::ERROR))
      return false;
    std::size_t plen = (static_cast<std::uint8_t>(buf_[2]) << 8) | static_cast<std::uint8_t>(buf_[3]);
    if (buf_.size() < kFrameHeaderSize + plen)
      return true; // wait for the rest
    Frame f;
    f.type = static_cast<FrameType>(type);
    f.payload = buf_.substr(kFrameHeaderSize, plen);
    out.push_back(std::move(f));
    buf_.erase(0, kFrameHeaderSize + plen);
  }
}

std::map<std::string, std::string> parse_kv(std::string_view line) {
  std::map<std::string, std::string> kv;
  std::size_t pos = 0;
  while (pos < line.size()) {
    auto sp = line.find_first_of(" \t", pos);
    auto tok = trim(line.substr(pos, sp == std::string_view::npos ? std::string_view::npos : sp - pos));
    if (!tok.empty()) {
      auto eq = tok.find('=');
      if (eq != std::string_view::npos)
        kv.emplace(std::string(tok.substr(0, eq)), std::string(tok.substr(eq + 1)));
    }
    if (sp == std::string_view::npos)
      break;
    pos = sp + 1;
  }
  return kv;
}

std::vector<std::map<std::string, std::string>> parse_kv_lines(std::string_view payload) {
  std::vector<std::map<std::string, std::string>> rows;
  std::size_t pos = 0;
  while (pos <= payload.size()) {
    auto nl = payload.find('\n', pos);
    auto line = trim(payload.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos));
    if (!line.empty())
      rows.push_back(parse_kv(line));
    if (nl == std::string_view::npos)
      break;
    pos = nl + 1;
  }
  return rows;
}

std::string encode_offer(const std::vector<OfferedDevice> &devices) {
  std::string s;
  char buf[64];
  for (const auto &d : devices) {
    s += "busid=" + d.busid;
    std::snprintf(buf, sizeof(buf), " vid=%04x pid=%04x", d.vid, d.pid);
    s += buf;
    if (!d.name.empty()) {
      // Spaces separate fields, so a device name has to lose them. Cosmetic only -- nothing keys
      // off the name.
      auto n = d.name;
      for (auto &c : n)
        if (c == ' ' || c == '\t' || c == '\n')
          c = '_';
      s += " name=" + n;
    }
    s += "\n";
  }
  return s;
}

std::vector<OfferedDevice> decode_offer(std::string_view payload) {
  std::vector<OfferedDevice> out;
  for (const auto &kv : parse_kv_lines(payload)) {
    auto it = kv.find("busid");
    if (it == kv.end() || it->second.empty())
      continue;
    OfferedDevice d;
    d.busid = it->second;
    if (auto v = kv.find("vid"); v != kv.end())
      d.vid = static_cast<std::uint16_t>(std::strtoul(v->second.c_str(), nullptr, 16));
    if (auto p = kv.find("pid"); p != kv.end())
      d.pid = static_cast<std::uint16_t>(std::strtoul(p->second.c_str(), nullptr, 16));
    if (auto n = kv.find("name"); n != kv.end())
      d.name = n->second;
    out.push_back(std::move(d));
  }
  return out;
}

std::string encode_need_data(std::uint32_t token, std::string_view busid) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "token=%08x busid=", token);
  return std::string(buf) + std::string(busid) + "\n";
}

bool decode_need_data(std::string_view payload, std::uint32_t &token, std::string &busid) {
  auto kv = first_line_kv(payload);
  auto t = kv.find("token"), b = kv.find("busid");
  if (t == kv.end() || b == kv.end() || b->second.empty())
    return false;
  token = static_cast<std::uint32_t>(std::strtoul(t->second.c_str(), nullptr, 16));
  busid = b->second;
  return true;
}

std::string encode_hello(std::size_t session_id, std::string_view client_name) {
  auto s = "session=" + std::to_string(session_id);
  if (!client_name.empty())
    s += " client=" + std::string(client_name);
  return s + "\n";
}

bool decode_hello(std::string_view payload, std::size_t &session_id, std::string &client_name) {
  auto kv = first_line_kv(payload);
  auto s = kv.find("session");
  if (s == kv.end())
    return false;
  session_id = static_cast<std::size_t>(std::strtoull(s->second.c_str(), nullptr, 10));
  if (auto c = kv.find("client"); c != kv.end())
    client_name = c->second;
  return true;
}

bool Liveness::should_ping(std::int64_t now_ms) {
  if (!started_) {
    started_ = true;
    last_ping_ = now_ms;
    return false; // do not ping the instant the connection opens
  }
  if (now_ms - last_ping_ < interval_ms_)
    return false;
  last_ping_ = now_ms;
  misses_++; // cleared by the pong; three outstanding means gone
  return true;
}

void Liveness::on_pong(std::int64_t) { misses_ = 0; }

} // namespace usbip::tunnel
