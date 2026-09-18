#pragma once

// Imports USB devices from the Moonlight client for the life of a stream session.
//
// Sequence per device: connect to the exporter -> OP_REQ_IMPORT -> hand the socket to vhci ->
// wait for enumeration -> mknod the nodes -> copy the host's hwdb entries -> announce via
// fake_udev. Detach reverses it.
//
// Transport today is DIRECT: a plain TCP connection to a usbipd, chosen with
// STEAM_STREAM_USBIP_DIRECT=host[:port]. That lets the whole server side be exercised against a
// real device with no client changes at all. The session-scoped TLS tunnel slots in behind the
// same interface later; the only structural difference is that TLS cannot be handed to the kernel
// directly, so it needs a userspace pump into a socketpair.
//
// Never fails a stream. A controller that does not arrive is a degraded session, not a broken one.

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace usbip {

class TunnelServer;

class ImportManager {
public:
  static ImportManager &instance();

  // Reads config from the environment and logs what is available. Safe to call when vhci is
  // absent -- import is simply disabled.
  void init();

  bool enabled() const;

  // Optional. When a tunnel client is connected it takes precedence over DIRECT: it is
  // authenticated and its device list came from the user's own filter UI, so we never import
  // something they did not tick. The pointer must outlive the manager (main.cpp owns it).
  void set_tunnel(TunnelServer *tunnel);

  // Detach anything a previous run of this process left attached. vhci is host-global and
  // unnamespaced, so an attachment outlives us; only ports WE recorded are touched.
  void reap_stale();

  // Called from MediaSession::start before the app launches, so the device exists for Steam's
  // first scan. Blocking with a hard budget; failures are logged and swallowed.
  void attach_for_session(std::size_t session_id, int timeout_ms = 3000);

  // Called from the RESUME path: re-import only what is no longer live.
  void reconcile_session(std::size_t session_id);

  // Called from MediaSession::stop().
  void detach_session(std::size_t session_id);

  ImportManager(const ImportManager &) = delete;
  ImportManager &operator=(const ImportManager &) = delete;

private:
  ImportManager();
  ~ImportManager();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace usbip
