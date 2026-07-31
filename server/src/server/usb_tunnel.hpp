#pragma once

// Session-scoped mTLS listener for USB device forwarding.
//
// Deliberately NOT SimpleWeb: ServerBase<HTTPS> parses a request line and owns the stream, and
// there is no clean way to hijack it into a raw byte pipe. This is a plain OpenSSL accept loop
// using the same certificate.
//
// Authentication is the whole security argument. USB/IP has no auth and no encryption, and its two
// worst CVEs (CVE-2016-3955, CVE-2026-31607, both CVSS 9.8 heap out-of-bounds writes) run
// server->client -- i.e. straight at our host kernel's URB parser. So a connection that does not
// present a cert already in the paired store is dropped before a single byte of USB/IP is read.
//
// Direction is inverted from what you would expect: the SERVER needs to initiate an import, but
// the CLIENT dialled the TLS connection. So the server sends NEED_DATA over the long-lived CONTROL
// connection and the client dials a second connection carrying that token. The token is what
// authorizes the DATA connection to be spliced to a device -- it is single-use and short-lived.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <crypto/crypto.hpp>
#include <server/usb_transport.hpp>
#include <server/usb_tunnel_proto.hpp>

namespace usbip {

class TunnelServer {
public:
  // Returns true when the cert belongs to a paired client. Injected rather than taking an
  // AppState so this TU stays testable and does not drag the whole server in.
  using CertVerifier = std::function<bool(const x509::x509_ptr &)>;

  TunnelServer();
  ~TunnelServer();

  // Binds and starts accepting. Returns false if the port is taken or the cert cannot be loaded;
  // the caller carries on without USB support rather than failing to start.
  bool start(int port, const std::string &cert_path, const std::string &key_path, CertVerifier verify);
  void stop();

  bool running() const;
  int port() const;

  // A CONTROL connection is up and has said HELLO. Until then there is nothing to import.
  bool has_client() const;

  // What the connected client offered, i.e. what its filter UI ticked. Empty when no client.
  std::vector<tunnel::OfferedDevice> offered() const;

  // The session id the client reported in HELLO. 0 if it did not know one yet.
  std::size_t client_session() const;

  // A transport bound to the currently connected client. Valid only while that client stays
  // connected; open() fails cleanly once it goes.
  std::unique_ptr<Transport> transport();

  TunnelServer(const TunnelServer &) = delete;
  TunnelServer &operator=(const TunnelServer &) = delete;

  // Public only so the Transport implementation in the .cpp can name it.
  struct Impl;

private:
  // Runs on a per-connection thread: TLS handshake, paired-cert check, preamble, then dispatch to
  // the control loop or to whoever is waiting on this DATA token.
  void handshake(int fd);
  void control_loop();

  std::unique_ptr<Impl> impl_;
};

} // namespace usbip
