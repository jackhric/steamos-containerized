// USB/IP wire format: exact request bytes, reply decoding, the variable-stride devlist walk,
// and total-ness against truncated/hostile input. Pure -- no sockets, no kernel, no vhci module.
//
// Device values are real, read off this host's /sys/bus/usb/devices: the Steam Controller Puck
// (28de:1304, busid 1-1, 7 interfaces) and a Razer mouse (1532:0083, busid 1-13.4, 3 interfaces).
// The differing interface counts are the point -- they are what makes the devlist stride vary.

#include <cassert>
#include <cstring>
#include <iostream>
#include <server/usbip_proto.hpp>
#include <string>
#include <vector>

using namespace usbip;

static void ok(const char *what) { std::cout << "[ OK ] " << what << "\n"; }

static const std::uint8_t *u8(const std::string &s) {
  return reinterpret_cast<const std::uint8_t *>(s.data());
}

// Build a wire-format usbip_usb_device. Mirrors the kernel's packing so the decoder is tested
// against bytes laid out independently of it.
static std::string make_device(const std::string &path, const std::string &busid,
                               std::uint32_t busnum, std::uint32_t devnum, std::uint32_t speed,
                               std::uint16_t vid, std::uint16_t pid, std::uint16_t bcd,
                               std::uint8_t cls, std::uint8_t sub, std::uint8_t proto,
                               std::uint8_t cfgval, std::uint8_t nconfigs, std::uint8_t nifaces) {
  std::string s;
  s.append(path);
  s.append(kPathSize - path.size(), '\0');
  s.append(busid);
  s.append(kBusIdSize - busid.size(), '\0');
  auto be32 = [&](std::uint32_t v) {
    s.push_back((char)(v >> 24));
    s.push_back((char)(v >> 16));
    s.push_back((char)(v >> 8));
    s.push_back((char)v);
  };
  auto be16 = [&](std::uint16_t v) {
    s.push_back((char)(v >> 8));
    s.push_back((char)v);
  };
  be32(busnum);
  be32(devnum);
  be32(speed);
  be16(vid);
  be16(pid);
  be16(bcd);
  s.push_back((char)cls);
  s.push_back((char)sub);
  s.push_back((char)proto);
  s.push_back((char)cfgval);
  s.push_back((char)nconfigs);
  s.push_back((char)nifaces);
  return s;
}

static std::string make_iface(std::uint8_t c, std::uint8_t s_, std::uint8_t p) {
  std::string s;
  s.push_back((char)c);
  s.push_back((char)s_);
  s.push_back((char)p);
  s.push_back('\0'); // padding byte
  return s;
}

static std::string puck_device() {
  return make_device("/sys/devices/pci0000:00/0000:00:14.0/usb1/1-1", "1-1", 1, 2, SPEED_FULL,
                     0x28de, 0x1304, 0x0002, 0xef, 0x02, 0x01, 1, 1, 7);
}

static std::string razer_device() {
  return make_device("/sys/devices/pci0000:00/0000:00:14.0/usb1/1-13/1-13.4", "1-13.4", 1, 11,
                     SPEED_FULL, 0x1532, 0x0083, 0x0100, 0x00, 0x00, 0x00, 1, 1, 3);
}

static std::string op_reply(std::uint16_t code, std::uint32_t status) {
  std::string s;
  s.push_back((char)(kVersion >> 8));
  s.push_back((char)(kVersion & 0xff));
  s.push_back((char)(code >> 8));
  s.push_back((char)(code & 0xff));
  s.push_back((char)(status >> 24));
  s.push_back((char)(status >> 16));
  s.push_back((char)(status >> 8));
  s.push_back((char)status);
  return s;
}

// ---------------------------------------------------------------- encode --

static void test_encode() {
  auto devlist = encode_req_devlist();
  assert(devlist.size() == kOpCommonSize);
  const std::uint8_t want_devlist[] = {0x01, 0x11, 0x80, 0x05, 0x00, 0x00, 0x00, 0x00};
  assert(std::memcmp(devlist.data(), want_devlist, sizeof(want_devlist)) == 0);
  ok("OP_REQ_DEVLIST is exactly 8 bytes: 0111 8005 00000000");

  auto imp = encode_req_import("1-1");
  assert(imp.size() == kOpCommonSize + kBusIdSize); // 40
  const std::uint8_t want_head[] = {0x01, 0x11, 0x80, 0x03, 0x00, 0x00, 0x00, 0x00,
                                    '1',  '-',  '1',  0x00, 0x00, 0x00};
  assert(std::memcmp(imp.data(), want_head, sizeof(want_head)) == 0);
  for (std::size_t i = kOpCommonSize + 3; i < imp.size(); i++)
    assert(imp[i] == '\0');
  ok("OP_REQ_IMPORT is exactly 40 bytes with a NUL-padded busid field");

  // A busid longer than the field must truncate, never overrun.
  auto over = encode_req_import(std::string(80, 'x'));
  assert(over.size() == kOpCommonSize + kBusIdSize);
  assert(over.back() == '\0');
  ok("over-long busid truncates to the fixed field width");
}

// ---------------------------------------------------------------- decode --

static void test_decode_import() {
  auto wire = op_reply(OP_REP_IMPORT, 0) + puck_device();
  assert(wire.size() == 320);

  OpCommon hdr{};
  assert(decode_op_common(u8(wire), wire.size(), hdr));
  assert(hdr.version == kVersion);
  assert(hdr.code == OP_REP_IMPORT);
  assert(hdr.status == 0);
  ok("OP_REP_IMPORT header decodes (version/code/status)");

  UsbDevice d{};
  assert(decode_usb_device(u8(wire) + kOpCommonSize, wire.size() - kOpCommonSize, d));
  assert(d.busid == "1-1");
  assert(d.path == "/sys/devices/pci0000:00/0000:00:14.0/usb1/1-1");
  assert(d.busnum == 1);
  assert(d.devnum == 2);
  assert(d.speed == SPEED_FULL);
  assert(d.id_vendor == 0x28de);
  assert(d.id_product == 0x1304);
  assert(d.bcd_device == 0x0002);
  assert(d.dev_class == 0xef);
  assert(d.dev_subclass == 0x02);
  assert(d.dev_protocol == 0x01);
  assert(d.config_value == 1);
  assert(d.num_configurations == 1);
  assert(d.num_interfaces == 7);
  ok("usbip_usb_device decodes to the real Steam Controller Puck descriptor");

  // What vhci's attach wants -- the identity on the EXPORTER's bus.
  assert(d.devid() == ((1u << 16) | 2u));
  assert(d.devid() == 0x00010002);
  ok("devid packs as (busnum << 16) | devnum");
}

static void test_endianness() {
  // A value that is not a palindrome in either half, so a swap cannot pass by accident.
  auto wire = op_reply(OP_REP_IMPORT, 0) +
              make_device("/p", "3-2", 0x11223344, 0x55667788, SPEED_HIGH, 0x28de, 0x1304, 0x0110,
                          0, 0, 0, 1, 1, 0);
  UsbDevice d{};
  assert(decode_usb_device(u8(wire) + kOpCommonSize, wire.size() - kOpCommonSize, d));
  assert(d.bcd_device == 0x0110); // NOT 0x1001
  assert(d.id_vendor == 0x28de);  // NOT 0xde28
  assert(d.busnum == 0x11223344); // NOT 0x44332211
  assert(d.devnum == 0x55667788);
  ok("multi-byte fields are big-endian (bcdDevice 0x0110 does not read as 0x1001)");

  // The two char arrays are byte strings and must NOT be swapped.
  assert(d.busid == "3-2");
  assert(d.path == "/p");
  ok("path/busid are byte strings, not byte-swapped");
}

static void test_status_and_version() {
  // A refused import carries ONLY the 8-byte header -- decoding must not demand a body.
  auto refused = op_reply(OP_REP_IMPORT, 1);
  assert(refused.size() == kOpCommonSize);
  OpCommon hdr{};
  assert(decode_op_common(u8(refused), refused.size(), hdr));
  assert(hdr.status != 0);
  ok("refused import decodes from the 8-byte header alone (no 312-byte body)");

  // Version mismatch must be visible to the caller rather than silently misparsed.
  std::string bad = refused;
  bad[0] = 0x01;
  bad[1] = 0x06; // 0x0106, a real historical version
  OpCommon h2{};
  assert(decode_op_common(u8(bad), bad.size(), h2));
  assert(h2.version == 0x0106);
  assert(h2.version != kVersion);
  ok("a 0x0106 reply is reported as-is so the caller can reject it");
}

static void test_devlist_variable_stride() {
  // Two devices with DIFFERENT interface counts (7 then 3). If the walk assumed a fixed record
  // size it would land mid-descriptor on the second device and produce garbage.
  std::string body;
  body.push_back(0);
  body.push_back(0);
  body.push_back(0);
  body.push_back(2); // ndev = 2, big-endian

  body += puck_device();
  for (int i = 0; i < 7; i++)
    body += make_iface(0x03, 0x00, (std::uint8_t)i); // HID interfaces

  body += razer_device();
  for (int i = 0; i < 3; i++)
    body += make_iface(0x03, 0x01, (std::uint8_t)(0x10 + i));

  std::vector<UsbDevice> devs;
  assert(decode_devlist_body(u8(body), body.size(), devs));
  assert(devs.size() == 2);

  assert(devs[0].busid == "1-1");
  assert(devs[0].id_product == 0x1304);
  assert(devs[0].interfaces.size() == 7);
  assert(devs[0].interfaces[6].iface_protocol == 6);

  // The one that proves the stride: reachable only if 7 interfaces were skipped, not 3 or 0.
  assert(devs[1].busid == "1-13.4");
  assert(devs[1].id_vendor == 0x1532);
  assert(devs[1].id_product == 0x0083);
  assert(devs[1].interfaces.size() == 3);
  assert(devs[1].interfaces[0].iface_subclass == 0x01);
  assert(devs[1].interfaces[2].iface_protocol == 0x12);
  ok("devlist walks a variable per-device stride (7 then 3 interfaces)");

  // Empty list is valid, not an error.
  std::string empty(4, '\0');
  std::vector<UsbDevice> none;
  assert(decode_devlist_body(u8(empty), empty.size(), none));
  assert(none.empty());
  ok("devlist with ndev=0 decodes to an empty list");
}

static void test_totality() {
  // Everything here is remote-controlled. Truncation at every length must be rejected cleanly
  // rather than read past the buffer. Run under ASan in CI to make this meaningful.
  auto full = op_reply(OP_REP_IMPORT, 0) + puck_device();
  for (std::size_t n = 0; n < kOpCommonSize; n++) {
    OpCommon h{};
    assert(!decode_op_common(u8(full), n, h));
  }
  for (std::size_t n = 0; n < kUsbDeviceSize; n++) {
    UsbDevice d{};
    assert(!decode_usb_device(u8(full) + kOpCommonSize, n, d));
  }
  ok("truncated headers and devices are rejected at every length");

  OpCommon h{};
  UsbDevice d{};
  assert(!decode_op_common(nullptr, 64, h));
  assert(!decode_usb_device(nullptr, 512, d));
  std::vector<UsbDevice> v;
  assert(!decode_devlist_body(nullptr, 512, v));
  ok("null buffers are rejected");

  // A device claiming interfaces the buffer cannot hold.
  std::string lying;
  lying.push_back(0);
  lying.push_back(0);
  lying.push_back(0);
  lying.push_back(1);
  lying += puck_device(); // says 7 interfaces, none follow
  std::vector<UsbDevice> devs;
  assert(!decode_devlist_body(u8(lying), lying.size(), devs));
  ok("a device whose interface count overruns the buffer is rejected");

  // A huge ndev must not cause a large allocation before the length check.
  std::string huge;
  huge.push_back((char)0xff);
  huge.push_back((char)0xff);
  huge.push_back((char)0xff);
  huge.push_back((char)0xff);
  huge += puck_device();
  std::vector<UsbDevice> hd;
  assert(!decode_devlist_body(u8(huge), huge.size(), hd));
  ok("ndev=0xffffffff is rejected against the buffer bound");

  // Truncate mid-devlist at every length; none may crash or over-read.
  std::string body;
  body.push_back(0);
  body.push_back(0);
  body.push_back(0);
  body.push_back(1);
  body += puck_device();
  for (int i = 0; i < 7; i++)
    body += make_iface(0x03, 0, (std::uint8_t)i);
  for (std::size_t n = 0; n < body.size(); n++) {
    std::vector<UsbDevice> out;
    decode_devlist_body(u8(body), n, out); // must return, either verdict, without over-reading
  }
  std::vector<UsbDevice> whole;
  assert(decode_devlist_body(u8(body), body.size(), whole));
  assert(whole.size() == 1 && whole[0].interfaces.size() == 7);
  ok("devlist truncated at every length returns cleanly; the whole buffer still decodes");
}

int main() {
  test_encode();
  test_decode_import();
  test_endianness();
  test_status_and_version();
  test_devlist_variable_stride();
  test_totality();

  std::cout << "\nALL USBIP-PROTO TESTS PASSED\n";
  return 0;
}
