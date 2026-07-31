// USB tunnel wire format. Pure -- no sockets, no TLS, no kernel.
//
// The point of this file: the client fork reimplements this codec from scratch (separate GPL
// codebase, no shared headers), so these vectors are the contract between the two. If a byte here
// changes, a deployed client stops connecting.

#include <cassert>
#include <cstdio>
#include <cstring>
#include <server/usb_tunnel_proto.hpp>
#include <string>
#include <vector>

using namespace usbip::tunnel;

static int failures = 0;
#define CHECK(cond, what)                                                                                    \
  do {                                                                                                       \
    if (cond)                                                                                                \
      std::printf("  ok   %s\n", what);                                                                      \
    else {                                                                                                   \
      std::printf("  FAIL %s\n", what);                                                                       \
      failures++;                                                                                            \
    }                                                                                                        \
  } while (0)

static std::string hex(const std::string &s) {
  std::string o;
  char b[4];
  for (unsigned char c : s) {
    std::snprintf(b, sizeof(b), "%02x", c);
    o += b;
  }
  return o;
}

static void test_preamble() {
  std::printf("== preamble ==\n");

  Preamble p;
  p.channel = Channel::CONTROL;
  p.token = 0;
  auto enc = encode_preamble(p);
  CHECK(enc.size() == kPreambleSize, "preamble is 12 bytes");
  CHECK(hex(enc) == "5353554201010000" "00000000", "CONTROL preamble bytes are exactly SSUB/1/1/0/0");

  p.channel = Channel::DATA;
  p.token = 0xdeadbeef;
  CHECK(hex(encode_preamble(p)) == "5353554201020000" "deadbeef", "DATA preamble carries the token big-endian");

  Preamble got;
  CHECK(decode_preamble(enc.data(), enc.size(), got) && got.channel == Channel::CONTROL && got.token == 0,
        "CONTROL preamble round-trips");
  auto d = encode_preamble(p);
  CHECK(decode_preamble(d.data(), d.size(), got) && got.channel == Channel::DATA && got.token == 0xdeadbeef,
        "DATA preamble round-trips with its token");

  // Every rejection below is a connection we must drop rather than guess at.
  auto bad = enc;
  bad[0] = 'X';
  CHECK(!decode_preamble(bad.data(), bad.size(), got), "wrong magic rejected");
  bad = enc;
  bad[4] = 2;
  CHECK(!decode_preamble(bad.data(), bad.size(), got), "unknown version rejected");
  bad = enc;
  bad[5] = 9;
  CHECK(!decode_preamble(bad.data(), bad.size(), got), "unknown channel rejected");
  for (std::size_t n = 0; n < kPreambleSize; n++)
    if (decode_preamble(enc.data(), n, got)) {
      CHECK(false, "short preamble rejected at every length");
      return;
    }
  CHECK(true, "short preamble rejected at every length 0..11");
}

static void test_frame_roundtrip() {
  std::printf("\n== frames ==\n");

  auto f = encode_frame(FrameType::PING);
  CHECK(hex(f) == "04000000", "empty PING is a bare 4-byte header");

  f = encode_frame(FrameType::HELLO, "session=7\n");
  CHECK(f.size() == kFrameHeaderSize + 10, "HELLO length field covers the payload");
  CHECK(static_cast<std::uint8_t>(f[2]) == 0 && static_cast<std::uint8_t>(f[3]) == 10,
        "length is big-endian");

  FrameDecoder dec;
  std::vector<Frame> out;
  CHECK(dec.feed(f.data(), f.size(), out) && out.size() == 1, "one frame in, one frame out");
  CHECK(out[0].type == FrameType::HELLO && out[0].payload == "session=7\n", "payload survives");

  // A payload at the 16-bit ceiling: the length field's own boundary.
  std::string big(kMaxPayload, 'x');
  auto bf = encode_frame(FrameType::OFFER, big);
  out.clear();
  FrameDecoder d2;
  CHECK(d2.feed(bf.data(), bf.size(), out) && out.size() == 1 && out[0].payload.size() == kMaxPayload,
        "65535-byte payload round-trips");
}

static void test_stream_reassembly() {
  std::printf("\n== TCP reassembly ==\n");

  // The bug this catches works perfectly on loopback and fails on a real network.
  std::string stream = encode_frame(FrameType::HELLO, "session=42\n") + encode_frame(FrameType::PING) +
                       encode_frame(FrameType::OFFER, "busid=1-4 vid=046d pid=c262\n") +
                       encode_frame(FrameType::BYE, "reason=done");

  FrameDecoder whole;
  std::vector<Frame> a;
  CHECK(whole.feed(stream.data(), stream.size(), a) && a.size() == 4, "four frames in one buffer");

  FrameDecoder bytewise;
  std::vector<Frame> b;
  bool okb = true;
  for (char c : stream)
    okb = okb && bytewise.feed(&c, 1, b);
  CHECK(okb && b.size() == 4, "same four frames fed one byte at a time");

  bool same = a.size() == b.size();
  for (std::size_t i = 0; same && i < a.size(); i++)
    same = a[i].type == b[i].type && a[i].payload == b[i].payload;
  CHECK(same, "byte-at-a-time yields identical frames to one-shot");
  CHECK(bytewise.buffered() == 0, "decoder retains nothing once frames are complete");

  // Three arbitrary chunks, splitting mid-header and mid-payload.
  FrameDecoder chunked;
  std::vector<Frame> c;
  std::size_t cut1 = 2, cut2 = stream.size() / 2;
  bool okc = chunked.feed(stream.data(), cut1, c);
  okc = okc && chunked.feed(stream.data() + cut1, cut2 - cut1, c);
  okc = okc && chunked.feed(stream.data() + cut2, stream.size() - cut2, c);
  CHECK(okc && c.size() == 4, "frames split across three chunks all parse");

  // A header alone yields nothing and must not be mistaken for a complete frame.
  FrameDecoder partial;
  std::vector<Frame> p;
  auto hdr = encode_frame(FrameType::OFFER, "busid=1-1\n");
  CHECK(partial.feed(hdr.data(), kFrameHeaderSize, p) && p.empty(), "header without payload yields nothing");
  CHECK(partial.feed(hdr.data() + kFrameHeaderSize, hdr.size() - kFrameHeaderSize, p) && p.size() == 1,
        "the frame appears once its payload arrives");
}

static void test_malformed() {
  std::printf("\n== malformed input ==\n");

  FrameDecoder dec;
  std::vector<Frame> out;
  std::uint8_t junk[] = {0x00, 0x00, 0x00, 0x00};
  CHECK(!dec.feed(junk, sizeof(junk), out), "frame type 0 rejected");

  FrameDecoder d2;
  out.clear();
  std::uint8_t high[] = {0xFF, 0x00, 0x00, 0x00};
  CHECK(!d2.feed(high, sizeof(high), out), "unknown frame type rejected");

  // A declared length larger than what has arrived must WAIT, never read past the buffer. Left
  // dangling forever it is a stalled connection, which the liveness monitor is for.
  FrameDecoder d3;
  out.clear();
  std::uint8_t overrun[] = {0x02, 0x00, 0xFF, 0xFF, 'a', 'b'};
  CHECK(d3.feed(overrun, sizeof(overrun), out) && out.empty(), "over-long length waits rather than over-reading");
}

static void test_payloads() {
  std::printf("\n== payloads ==\n");

  std::vector<OfferedDevice> devs = {
      {"1-4", 0x046d, 0xc262, "Logitech G920"},
      {"3-2.1", 0x28de, 0x1304, "Steam Controller Puck"},
  };
  auto enc = encode_offer(devs);
  auto dec = decode_offer(enc);
  CHECK(dec.size() == 2, "two offered devices round-trip");
  CHECK(dec[0].busid == "1-4" && dec[0].vid == 0x046d && dec[0].pid == 0xc262, "first device fields");
  CHECK(dec[1].busid == "3-2.1" && dec[1].vid == 0x28de && dec[1].pid == 0x1304, "second device, dotted busid");
  CHECK(dec[0].name == "Logitech_G920", "spaces in a name become underscores, not extra fields");

  // A client that grows a field must not take the session down with it.
  auto fwd = decode_offer("busid=1-1 vid=28de pid=1304 speed=480 serial=ABC\nbusid=2-1 vid=0 pid=0\n");
  CHECK(fwd.size() == 2, "unknown keys ignored, not fatal");
  auto skip = decode_offer("vid=1234 pid=5678\nbusid=1-1 vid=28de pid=1304\n");
  CHECK(skip.size() == 1 && skip[0].busid == "1-1", "a line with no busid is skipped, the rest still parse");
  CHECK(decode_offer("").empty(), "an empty OFFER means no devices, not an error");

  std::uint32_t tok = 0;
  std::string busid;
  auto nd = encode_need_data(0x0badf00d, "1-4");
  CHECK(decode_need_data(nd, tok, busid) && tok == 0x0badf00d && busid == "1-4", "NEED_DATA round-trips");
  CHECK(!decode_need_data("token=1234\n", tok, busid), "NEED_DATA without a busid rejected");
  CHECK(!decode_need_data("busid=1-1\n", tok, busid), "NEED_DATA without a token rejected");

  std::size_t sid = 0;
  std::string name;
  CHECK(decode_hello(encode_hello(9007199254740993ULL, "htpc"), sid, name) && sid == 9007199254740993ULL &&
            name == "htpc",
        "HELLO round-trips a 64-bit session id");
  CHECK(!decode_hello("client=htpc\n", sid, name), "HELLO without a session rejected");

  auto kv = parse_kv("a=1 b=x=y c=");
  CHECK(kv["b"] == "x=y", "only the first '=' splits, so a value may contain one");
  CHECK(kv.count("c") && kv["c"].empty(), "an empty value is a value");
}

static void test_liveness() {
  std::printf("\n== liveness ==\n");

  // Injected clock: no sleeps, so this cannot go flaky in CI.
  Liveness l(2000, 3);
  CHECK(!l.should_ping(0), "no ping the instant the connection opens");
  CHECK(!l.should_ping(1999), "no ping before the interval elapses");
  CHECK(l.should_ping(2000), "ping at the interval");
  CHECK(!l.gone() && l.misses() == 1, "one outstanding ping is not 'gone'");
  l.on_pong(2100);
  CHECK(l.misses() == 0, "a pong clears the outstanding count");

  CHECK(l.should_ping(4100) && l.should_ping(6100) && !l.gone(), "two unanswered pings still tolerated");
  CHECK(l.should_ping(8100) && l.gone(), "three unanswered pings means gone");

  Liveness r(2000, 3);
  r.should_ping(0);
  r.should_ping(2000);
  r.should_ping(4000);
  r.on_pong(4500);
  r.should_ping(6000);
  CHECK(!r.gone(), "a late pong rescues a connection that was two misses down");
}

int main() {
  test_preamble();
  test_frame_roundtrip();
  test_stream_reassembly();
  test_malformed();
  test_payloads();
  test_liveness();

  if (failures) {
    std::printf("\n%d TUNNEL FRAMING TEST(S) FAILED\n", failures);
    return 1;
  }
  std::printf("\nALL TUNNEL FRAMING TESTS PASSED\n");
  return 0;
}
