// The OP_REQ_IMPORT exchange, driven by a fake Channel replaying captured bytes.
//
// No socket, no exporter, no kernel. Worth having because two of these branches only occur when
// something has already gone wrong on the far end, so they are the ones least likely to be
// exercised by hand -- and the REFUSED case in particular would hang an import for the whole
// socket timeout if it read a body that was never sent.

#include <cassert>
#include <cstdio>
#include <cstring>
#include <server/usb_handshake.hpp>
#include <string>
#include <vector>

using namespace usbip;

static int failures = 0;
#define CHECK(cond, what)                                                                                    \
  do {                                                                                                       \
    if (cond)                                                                                                \
      std::printf("  ok   %s\n", what);                                                                      \
    else {                                                                                                   \
      std::printf("  FAIL %s\n", what);                                                                      \
      failures++;                                                                                            \
    }                                                                                                        \
  } while (0)

// Replays a scripted reply and records what we wrote. read_exact fails rather than blocking once
// the script runs out -- which is exactly how a real socket behaves at its timeout.
class FakeChannel : public Channel {
public:
  explicit FakeChannel(std::string reply) : reply_(std::move(reply)) {}

  bool read_exact(void *buf, std::size_t n) override {
    if (fail_reads_ || pos_ + n > reply_.size()) {
      over_read_ = pos_ + n > reply_.size();
      return false;
    }
    std::memcpy(buf, reply_.data() + pos_, n);
    pos_ += n;
    return true;
  }

  bool write_all(std::string_view s) override {
    if (fail_writes_)
      return false;
    written_.append(s);
    return true;
  }

  int into_kernel_fd() override { return -1; }

  void fail_writes() { fail_writes_ = true; }
  const std::string &written() const { return written_; }
  std::size_t consumed() const { return pos_; }
  bool attempted_over_read() const { return over_read_; }

private:
  std::string reply_;
  std::string written_;
  std::size_t pos_ = 0;
  bool fail_writes_ = false, fail_reads_ = false, over_read_ = false;
};

static void put_be16(std::string &s, std::uint16_t v) {
  s.push_back(static_cast<char>(v >> 8));
  s.push_back(static_cast<char>(v & 0xFF));
}
static void put_be32(std::string &s, std::uint32_t v) {
  s.push_back(static_cast<char>(v >> 24));
  s.push_back(static_cast<char>((v >> 16) & 0xFF));
  s.push_back(static_cast<char>((v >> 8) & 0xFF));
  s.push_back(static_cast<char>(v & 0xFF));
}

static std::string op_rep_import_header(std::uint16_t version, std::uint32_t status) {
  std::string s;
  put_be16(s, version);
  put_be16(s, 0x0003); // OP_REP_IMPORT
  put_be32(s, status);
  return s;
}

// A 312-byte usbip_usb_device for the Steam Controller Puck at bus 1 device 2.
static std::string usb_device_body() {
  std::string s;
  s.resize(256, '\0'); // path[256]
  std::string path = "/sys/devices/pci0000:00/0000:00:14.0/usb1/1-1";
  std::memcpy(s.data(), path.data(), path.size());
  std::string busid(32, '\0');
  std::memcpy(busid.data(), "1-1", 3);
  s += busid;
  put_be32(s, 1); // busnum
  put_be32(s, 2); // devnum
  put_be32(s, 2); // speed = USB_SPEED_HIGH
  put_be16(s, 0x28de);
  put_be16(s, 0x1304);
  put_be16(s, 0x0110); // bcdDevice
  s.push_back('\0');   // bDeviceClass
  s.push_back('\0');
  s.push_back('\0');
  s.push_back(1); // bConfigurationValue
  s.push_back(1); // bNumConfigurations
  s.push_back(7); // bNumInterfaces
  return s;
}

static void test_success() {
  std::printf("== successful import ==\n");
  FakeChannel ch(op_rep_import_header(0x0111, 0) + usb_device_body());
  UsbDevice dev{};
  auto h = do_import_handshake(ch, "1-1", dev);

  CHECK(h.result == HandshakeResult::OK, "handshake reports OK");
  CHECK(ch.written().size() == 40, "we sent exactly a 40-byte OP_REQ_IMPORT");
  CHECK(ch.written().compare(8, 3, "1-1") == 0, "the busid is in the request, unswapped");
  CHECK(dev.id_vendor == 0x28de && dev.id_product == 0x1304, "VID/PID decoded big-endian");
  CHECK(dev.speed == 2, "speed decoded");
  CHECK(dev.devid() == ((1u << 16) | 2u), "devid is (busnum << 16) | devnum");
  CHECK(ch.consumed() == 8 + 312, "consumed the header and exactly one device body");
}

static void test_refused() {
  std::printf("\n== refused import ==\n");

  // The header ALONE -- no body follows. This is what usbipd sends for a device that is not bound,
  // and the single most likely failure in the field.
  FakeChannel ch(op_rep_import_header(0x0111, 1));
  UsbDevice dev{};
  auto h = do_import_handshake(ch, "1-1", dev);

  CHECK(h.result == HandshakeResult::REFUSED, "refusal reported as REFUSED, not as a decode error");
  CHECK(h.status == 1, "the exporter's status code survives for the log message");
  CHECK(ch.consumed() == 8, "consumed ONLY the 8-byte header");
  CHECK(!ch.attempted_over_read(), "never tried to read a 312-byte body that was not sent");
}

static void test_version_mismatch() {
  std::printf("\n== version mismatch ==\n");
  FakeChannel ch(op_rep_import_header(0x0106, 0) + usb_device_body());
  UsbDevice dev{};
  auto h = do_import_handshake(ch, "1-1", dev);
  CHECK(h.result == HandshakeResult::BAD_VERSION, "an older exporter is rejected, not misparsed");
  CHECK(h.peer_version == 0x0106, "their version is reported so the log can name it");
  CHECK(ch.consumed() == 8, "stopped at the header");
}

static void test_truncation() {
  std::printf("\n== truncated replies ==\n");

  FakeChannel empty("");
  UsbDevice dev{};
  CHECK(do_import_handshake(empty, "1-1", dev).result == HandshakeResult::NO_REPLY,
        "no reply at all is NO_REPLY");

  FakeChannel short_hdr(op_rep_import_header(0x0111, 0).substr(0, 5));
  CHECK(do_import_handshake(short_hdr, "1-1", dev).result == HandshakeResult::NO_REPLY,
        "a partial header is NO_REPLY");

  // Every truncation point of the body: none may over-read, all must report cleanly.
  auto full = op_rep_import_header(0x0111, 0) + usb_device_body();
  bool all_clean = true;
  for (std::size_t n = 8; n < full.size(); n++) {
    FakeChannel c(full.substr(0, n));
    UsbDevice d{};
    if (do_import_handshake(c, "1-1", d).result != HandshakeResult::TRUNCATED_BODY)
      all_clean = false;
  }
  CHECK(all_clean, "every body truncation from 0..311 bytes reports TRUNCATED_BODY");

  FakeChannel wr("");
  wr.fail_writes();
  CHECK(do_import_handshake(wr, "1-1", dev).result == HandshakeResult::WRITE_FAILED,
        "a dead socket on write is WRITE_FAILED, not a hang");
}

static void test_descriptions() {
  std::printf("\n== reason strings ==\n");
  // The caller logs these, so an unlabelled enum value would surface to a user as "unknown".
  HandshakeResult all[] = {HandshakeResult::OK,      HandshakeResult::WRITE_FAILED, HandshakeResult::NO_REPLY,
                           HandshakeResult::BAD_HEADER, HandshakeResult::BAD_VERSION,  HandshakeResult::REFUSED,
                           HandshakeResult::TRUNCATED_BODY};
  bool named = true;
  for (auto r : all)
    if (std::strcmp(describe(r), "unknown") == 0)
      named = false;
  CHECK(named, "every result has a human-readable reason");
}

int main() {
  test_success();
  test_refused();
  test_version_mismatch();
  test_truncation();
  test_descriptions();

  if (failures) {
    std::printf("\n%d HANDSHAKE TEST(S) FAILED\n", failures);
    return 1;
  }
  std::printf("\nALL HANDSHAKE TESTS PASSED\n");
  return 0;
}
