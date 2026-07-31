#include <server/usb_handshake.hpp>

namespace usbip {

Handshake do_import_handshake(Channel &ch, const std::string &busid, UsbDevice &out) {
  Handshake h;

  if (!ch.write_all(encode_req_import(busid))) {
    h.result = HandshakeResult::WRITE_FAILED;
    return h;
  }

  std::uint8_t hdr[kOpCommonSize];
  if (!ch.read_exact(hdr, sizeof(hdr))) {
    h.result = HandshakeResult::NO_REPLY;
    return h;
  }
  OpCommon rep{};
  if (!decode_op_common(hdr, sizeof(hdr), rep)) {
    h.result = HandshakeResult::BAD_HEADER;
    return h;
  }
  h.peer_version = rep.version;
  if (rep.version != kVersion) {
    h.result = HandshakeResult::BAD_VERSION;
    return h;
  }
  if (rep.status != 0) {
    // A refused import sends ONLY the header. Reading a 312-byte body here would block until the
    // socket timed out -- and on the tunnel, that is a stalled import rather than a clean failure.
    h.result = HandshakeResult::REFUSED;
    h.status = rep.status;
    return h;
  }

  std::uint8_t body[kUsbDeviceSize];
  if (!ch.read_exact(body, sizeof(body))) {
    h.result = HandshakeResult::TRUNCATED_BODY;
    return h;
  }
  if (!decode_usb_device(body, sizeof(body), out)) {
    h.result = HandshakeResult::TRUNCATED_BODY;
    return h;
  }
  return h;
}

const char *describe(HandshakeResult r) {
  switch (r) {
  case HandshakeResult::OK:
    return "ok";
  case HandshakeResult::WRITE_FAILED:
    return "could not send OP_REQ_IMPORT";
  case HandshakeResult::NO_REPLY:
    return "no OP_REP_IMPORT";
  case HandshakeResult::BAD_HEADER:
    return "malformed reply header";
  case HandshakeResult::BAD_VERSION:
    return "USB/IP version mismatch";
  case HandshakeResult::REFUSED:
    return "exporter refused the device (is it bound?)";
  case HandshakeResult::TRUNCATED_BODY:
    return "truncated device descriptor";
  }
  return "unknown";
}

} // namespace usbip
