#pragma once

// Wire format for the session-scoped USB tunnel. No I/O -- this TU is pure so the whole codec is
// unit-testable, and so the client fork can be checked against the same vectors.
//
// The client fork is a separate GPL-3 codebase and MUST NOT link this file. It reimplements the
// format from docs/usb-tunnel.md. That is why the payloads are text: a spec someone reimplements
// survives version skew far better than a packed struct does.
//
//   Preamble -- 12 bytes, client->server, first thing after the TLS handshake, EVERY connection:
//     0..3   magic "SSUB"
//     4      version
//     5      channel (1 CONTROL, 2 DATA)
//     6..7   reserved, zero
//     8..11  token, big-endian; zero on CONTROL, the NEED_DATA token on DATA
//
//   Frame -- CONTROL channel only. DATA carries raw USB/IP bytes with no framing at all, because
//   vhci_hcd owns that stream and would choke on anything we added to it.
//     0      type
//     1      flags, reserved, zero
//     2..3   payload length, big-endian
//     4..    payload
//
// Everything multi-byte is big-endian, matching USB/IP itself rather than the host.
//
// Why a preamble at all: without it "TLS is up" and "connected to a server that accepted the
// socket and then silently dropped my cert" look identical. It is also the seam a future
// proto=hid transport slots into, and it cannot be retrofitted once clients are deployed.

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace usbip::tunnel {

constexpr char kMagic[4] = {'S', 'S', 'U', 'B'};
constexpr std::uint8_t kVersion = 1;
constexpr std::size_t kPreambleSize = 12;
constexpr std::size_t kFrameHeaderSize = 4;
constexpr std::size_t kMaxPayload = 0xFFFF;
constexpr int kDefaultPort = 48011;

enum class Channel : std::uint8_t {
  CONTROL = 1,
  DATA = 2,
};

enum class FrameType : std::uint8_t {
  HELLO = 0x01,     // c->s  session=<id>  client=<name>
  OFFER = 0x02,     // c->s  one "busid=... vid=... pid=... name=..." line per exportable device
  NEED_DATA = 0x03, // s->c  token=<hex8> busid=<...>   -- how the server initiates over a
                    //       connection the client had to dial
  PING = 0x04,
  PONG = 0x05,
  DEV_GONE = 0x06, // c->s  busid=<...>   the client lost it locally
  BYE = 0x07,
  ERROR = 0x08, // s->c  reason=<text>
};

struct Preamble {
  std::uint8_t version = kVersion;
  Channel channel = Channel::CONTROL;
  std::uint32_t token = 0;
};

struct Frame {
  FrameType type = FrameType::PING;
  std::string payload;
};

// A device the client is willing to export. Populated from OFFER; the client's filter UI decides
// what appears here, so the server never sees a device the user did not tick.
struct OfferedDevice {
  std::string busid; // on the CLIENT, e.g. "1-4"
  std::uint16_t vid = 0;
  std::uint16_t pid = 0;
  std::string name;
};

// ---- encode ----

std::string encode_preamble(const Preamble &p);
std::string encode_frame(FrameType type, std::string_view payload = {});

// ---- decode ----

// Needs exactly kPreambleSize bytes. Rejects a bad magic, an unknown version and an unknown
// channel rather than guessing.
bool decode_preamble(const void *buf, std::size_t len, Preamble &out);

// Incremental: TCP will split and coalesce frames however it likes, and a decoder that assumes
// one whole frame per read works flawlessly on loopback and fails on a real network.
class FrameDecoder {
public:
  // Appends bytes and returns whatever frames are now complete. Returns false on a protocol
  // violation, after which the connection must be dropped -- the stream cannot be resynchronised.
  bool feed(const void *buf, std::size_t len, std::vector<Frame> &out);
  std::size_t buffered() const { return buf_.size(); }

private:
  std::string buf_;
};

// ---- payloads ----

// "k=v" lines. Whitespace-separated pairs on one line are also accepted, since OFFER puts a whole
// device on a line. A value may contain '=' -- only the first one splits.
std::vector<std::map<std::string, std::string>> parse_kv_lines(std::string_view payload);
std::map<std::string, std::string> parse_kv(std::string_view line);

std::string encode_offer(const std::vector<OfferedDevice> &devices);
// Skips lines without a busid rather than failing the whole OFFER: a client that learns to send a
// field we do not understand should not take the session down.
std::vector<OfferedDevice> decode_offer(std::string_view payload);

std::string encode_need_data(std::uint32_t token, std::string_view busid);
bool decode_need_data(std::string_view payload, std::uint32_t &token, std::string &busid);

std::string encode_hello(std::size_t session_id, std::string_view client_name);
bool decode_hello(std::string_view payload, std::size_t &session_id, std::string &client_name);

// ---- liveness ----

// USB/IP has no heartbeat -- there is still a literal "TODO: write code for heartbeat" in
// usbip_network.c -- so a vanished client leaves a zombie device for the 13-15 minutes TCP spends
// in tcp_retries2. This supplies one at the transport layer.
//
// A pure state machine over an injected clock, so its test is deterministic rather than a sleep.
class Liveness {
public:
  Liveness(int interval_ms = 2000, int max_misses = 3) : interval_ms_(interval_ms), max_misses_(max_misses) {}

  // True when a PING is due. Records it as outstanding.
  bool should_ping(std::int64_t now_ms);
  void on_pong(std::int64_t now_ms);
  bool gone() const { return misses_ >= max_misses_; }
  int misses() const { return misses_; }

private:
  int interval_ms_, max_misses_;
  std::int64_t last_ping_ = 0;
  bool started_ = false;
  int misses_ = 0;
};

} // namespace usbip::tunnel
