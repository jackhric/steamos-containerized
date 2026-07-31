#pragma once

// The OP_REQ_IMPORT exchange, over an abstract Channel rather than an fd.
//
// Split out of usb_import.cpp for two reasons: it is the same on both transports, and it has no
// I/O of its own once the Channel is injected -- so a fake channel replaying captured bytes tests
// every failure branch with no socket, no kernel and no exporter.
//
// It does not log. Reporting the reason is the caller's job precisely so this TU links nothing
// (no Boost.Log, no fmt) and stays in the unit tier.

#include <server/usb_transport.hpp>
#include <server/usbip_proto.hpp>

namespace usbip {

enum class HandshakeResult {
  OK,
  WRITE_FAILED,
  NO_REPLY,      // nothing, or a short header
  BAD_HEADER,    // did not decode as op_common
  BAD_VERSION,   // exporter speaks a version we do not
  REFUSED,       // status != 0 -- almost always "not bound"
  TRUNCATED_BODY // header promised a device, body did not arrive
};

// Non-zero exporter status, meaningful only when the result is REFUSED.
struct Handshake {
  HandshakeResult result = HandshakeResult::OK;
  std::uint32_t status = 0;
  std::uint16_t peer_version = 0;
};

// On OK, `out` is filled and the channel now carries URBs -- nothing else may read from it.
Handshake do_import_handshake(Channel &ch, const std::string &busid, UsbDevice &out);

const char *describe(HandshakeResult r);

} // namespace usbip
