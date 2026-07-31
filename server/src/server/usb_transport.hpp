#pragma once

// How the importer reaches the exporter. Two implementations:
//
//   DIRECT  a plain TCP connection to a usbipd. No auth, no encryption -- a debug escape hatch
//           that exercises the entire server side with no client changes at all.
//   TUNNEL  a session-scoped mTLS connection the client dialled, authenticated by the cert it
//           already paired with.
//
// The seam is deliberately narrow: everything above it -- the import handshake, vhci attach, node
// creation, uevents -- is identical either way. The one structural difference is that a TLS stream
// cannot be handed to the kernel, so TUNNEL pumps it into a socketpair and gives the kernel the
// other end. That is what into_kernel_fd() hides.

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace usbip {

// A byte stream to the exporter, carrying the USB/IP protocol.
class Channel {
public:
  virtual ~Channel() = default;

  virtual bool read_exact(void *buf, std::size_t n) = 0;
  virtual bool write_all(std::string_view s) = 0;

  // Hands the stream to the kernel. Returns an fd whose peer carries these bytes -- for DIRECT the
  // socket itself, for TUNNEL one end of a socketpair with a pump on the other. The caller passes
  // it to vhci::attach and then closes it (the kernel took its own reference).
  //
  // After this the Channel no longer speaks the protocol; the kernel owns the stream. -1 on
  // failure.
  virtual int into_kernel_fd() = 0;
};

class Transport {
public:
  virtual ~Transport() = default;

  // For logs: "direct 127.0.0.1:3240" or "tunnel session 3 (htpc)".
  virtual std::string describe() const = 0;

  // Busids ON THE EXPORTER to import. DIRECT reads them from the environment; TUNNEL takes them
  // from the client's OFFER, which is what the client's device-filter UI produced -- so the server
  // never sees a device the user did not tick.
  virtual std::vector<std::string> busids() = 0;

  virtual std::unique_ptr<Channel> open(const std::string &busid, int timeout_ms) = 0;
};

} // namespace usbip
