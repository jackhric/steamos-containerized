#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <helpers/logger.hpp>
#include <sys/stat.h>
#include <sys/wait.h>
#include <memory>
#include <mutex>
#include <optional>
#include <server/control/control.hpp>
#include <server/media/encoder_config.hpp>
#include <server/media/media.hpp>
#include <server/rtsp/server.hpp>
#include <server/http/servers.hpp>
#include <server/session/session.hpp>
#include <server/session/state.hpp>
#include <server/input/uinput.hpp>
#include <server/usb/import.hpp>
#include <server/usb/tunnel.hpp>
#include <thread>
#include <unistd.h>

static std::string env_or(const char *k, const std::string &def) {
  const char *v = std::getenv(k);
  return v ? std::string(v) : def;
}

// PID 1 must reap SIGCHLD or launched app trees pile up as zombies.
static void reap_children(int) {
  int saved = errno;
  while (::waitpid(-1, nullptr, WNOHANG) > 0) {
  }
  errno = saved;
}

static int term_pipe[2] = {-1, -1};

static void on_term(int sig) {
  int saved = errno;
  char c = static_cast<char>(sig);
  (void)!::write(term_pipe[1], &c, 1);
  errno = saved;
}

int main() {
  {
    struct sigaction sa{};
    sa.sa_handler = reap_children;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    ::sigaction(SIGCHLD, &sa, nullptr);
  }
  // As PID 1 an unhandled SIGTERM is silently dropped, so `docker stop` would always sit out its
  // grace period. A handler rather than a blocked mask + sigwait: masks survive exec, and the
  // launched Steam/gamescope must still honour the SIGTERM MediaSession::stop() sends them.
  if (::pipe2(term_pipe, O_CLOEXEC) == 0) {
    struct sigaction sa{};
    sa.sa_handler = on_term;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    ::sigaction(SIGTERM, &sa, nullptr);
    ::sigaction(SIGINT, &sa, nullptr);
  }

  logs::init(logs::parse_level(env_or("STEAM_STREAM_LOG_LEVEL", "INFO")));

  auto state_dir = env_or("STEAM_STREAM_STATE_DIR", "/var/lib/steam-stream");

  // Same st_dev as "/" means the state dir sits on the container's writable layer: it survives
  // a restart but is destroyed by `docker rm` / --force-recreate, taking every pairing with it.
  {
    struct stat sd{}, root{};
    if (::stat(state_dir.c_str(), &sd) == 0 && ::stat("/", &root) == 0 && sd.st_dev == root.st_dev)
      logs::log(logs::warning,
                "state dir {} is NOT on a mounted volume -- the server identity and all Moonlight "
                "pairings will be lost when this container is recreated",
                state_dir);
  }

  std::optional<state::AppState> state_opt;
  try {
    state_opt = state::AppState::init(state_dir);
  } catch (const std::exception &e) {
    logs::log(logs::error, "cannot initialise state from {}: {}", state_dir, e.what());
    return 1;
  }
  auto &state = *state_opt;
  state.http_port = std::stoi(env_or("STEAM_STREAM_HTTP_PORT", "47989"));
  state.https_port = std::stoi(env_or("STEAM_STREAM_HTTPS_PORT", "47984"));
  state.rtsp_port = std::stoi(env_or("STEAM_STREAM_RTSP_PORT", "48010"));
  auto render_node = env_or("STEAM_STREAM_RENDER_NODE", "/dev/dri/renderD129");

  logs::log(logs::info, "steam-stream-server starting: host={} uuid={} state_dir={}",
            state.hostname, state.uuid, state_dir);

  media::MediaSession::global_init();

  // Seed the host-mounted encoder config now (if absent) so it exists for editing before the
  // first stream, and log which encoders are actually available on this box.
  {
    auto cfg_path = encoder_config::config_path();
    encoder_config::create_default_if_missing(cfg_path);
    auto avail = encoder_config::available_encoders(encoder_config::load_or_seed());
    logs::log(logs::info, "[ENCODER-CFG] config={} available: h264={} hevc={} av1={}", cfg_path,
              avail.h264, avail.hevc, avail.av1);
    // Advertise HEVC/AV1 to the client only if this box can actually encode them; otherwise the
    // client's ANNOUNCE falls back to H264 (bitStreamFormat=0) even when HEVC is selected.
    state.support_hevc = avail.hevc;
    state.support_av1 = avail.av1;
  }

  if (!control::ControlServer::global_init())
    logs::log(logs::warning, "ENet init failed -- control channel will not work");
  if (!input::uinput_available())
    logs::log(logs::warning, "/dev/uinput not writable -- input injection will fail "
                             "(run container with --device /dev/uinput)");

  // USB/IP device import. Reap first: vhci is host-global and unnamespaced, so an attachment
  // from a previous run of this process outlives it.
  usbip::TunnelServer usb_tunnel;
  {
    auto &usb = usbip::ImportManager::instance();
    usb.init();
    usb.reap_stale();

    // The tunnel only makes sense if there is somewhere to put a device, so it follows vhci.
    if (usb.enabled() && env_or("STEAM_STREAM_USBIP_TUNNEL", "1") != "0") {
      int port = std::stoi(env_or("STEAM_STREAM_USBIP_PORT", std::to_string(usbip::tunnel::kDefaultPort)));
      auto verify = [&state](const x509::x509_ptr &cert) {
        return state.get_client_via_ssl(cert).has_value();
      };
      if (usb_tunnel.start(port, state.cert_path, state.key_path, verify)) {
        usb.set_tunnel(&usb_tunnel);
        // Only now is the port worth advertising; a client that dials a dead one just waits.
        state.usb_bridge_port = port;
      }
    }
  }

  auto media_mtx = std::make_shared<std::mutex>();
  std::shared_ptr<media::MediaSession> media_holder;

  // id match so a stale STOP can't kill a newer session.
  auto stop_session = [&](std::size_t sid) {
    std::shared_ptr<media::MediaSession> victim;
    {
      std::lock_guard<std::mutex> lk(*media_mtx);
      if (media_holder && media_holder->session_id() == sid)
        victim.swap(media_holder);
    }
    if (victim) {
      logs::log(logs::info, "[MAIN] stopping session {} (STOP/cancel)", sid);
      victim->stop();
    }
  };
  state.stop_session = stop_session;

  // Graceful stop gives Steam its cloud-save window and hands imported USB devices back; the
  // listener threads have no clean exit, so the process just ends once that's done.
  std::thread([&]() {
    char sig = 0;
    while (::read(term_pipe[0], &sig, 1) < 0 && errno == EINTR) {
    }
    logs::log(logs::info, "[MAIN] signal {} -- shutting down", int(sig));
    std::shared_ptr<media::MediaSession> victim;
    {
      std::lock_guard<std::mutex> lk(*media_mtx);
      victim.swap(media_holder);
    }
    if (victim)
      victim->stop();
    ::_exit(0);
  }).detach();

  control::ControlServer control_server(state.control_stream_port, state.sessions);
  control_server.set_idr_callback([&](std::size_t sid) {
    std::lock_guard<std::mutex> lk(*media_mtx);
    if (media_holder)
      media_holder->force_idr();
  });
  control_server.set_media_accessor([&]() -> std::shared_ptr<media::MediaSession> {
    std::lock_guard<std::mutex> lk(*media_mtx);
    return media_holder;
  });
  control_server.set_stop_callback(stop_session);
  std::thread control_thread([&control_server]() { control_server.run(); });

  std::thread rtsp_thread([&]() {
    rtsp::run_server(state.rtsp_port, *state.sessions, [&](session::StreamSession &s) {
      logs::log(logs::info,
                "[RTSP] PLAY for session {} -- starting media pipeline ({}x{}@{} {}kbps fec={}% "
                "audio={}ch encrypt={})",
                s.session_id, s.video.width, s.video.height, s.video.fps, s.video.bitrate_kbps,
                s.video.fec_percentage, s.audio.channels, s.audio.encrypt);
      auto sess = state.sessions->get_by_id(s.session_id);
      if (!sess) {
        logs::log(logs::error, "[RTSP] PLAY: session {} vanished from registry", s.session_id);
        return;
      }

      // RESUME: must NOT tear down/relaunch the app -- reuse the pipeline, re-target RTP at the
      // reconnected client, and force an IDR so the new decoder syncs.
      {
        std::shared_ptr<media::MediaSession> existing;
        {
          std::lock_guard<std::mutex> lk(*media_mtx);
          existing = media_holder;
        }
        bool reusable = existing && existing->session_id() == s.session_id && existing->is_active();
        if (reusable && !existing->app_alive()) {
          // Steam exited (or crashed) under the still-running compositor: resuming would stream
          // an empty desktop. Fall through to a fresh launch, which sweeps the leftover group.
          logs::log(logs::warning,
                    "[RTSP] PLAY for session {} -- app (pgid {}) has exited; relaunching instead "
                    "of resuming",
                    s.session_id, existing->app_pid());
        } else if (reusable) {
          logs::log(logs::info,
                    "[RTSP] PLAY for session {} -- RESUME: reusing running app (pgid {}); "
                    "re-targeting RTP, IDR + counter reset on the client's first ping (no relaunch)",
                    s.session_id, existing->app_pid());
          // Apply the renegotiated bitrate before the IDR so the keyframe uses the new rate.
          existing->update_bitrate(s.video.bitrate_kbps, s.video.fps);
          // A resume can renegotiate the audio layout; the Opus stream config is baked into the
          // pipeline, so rebuild it or the client can't decode. The sink keeps its old layout
          // (pulse remixes) -- true surround applies once the app is next relaunched.
          if (existing->audio_channels() != s.audio.channels) {
            logs::log(logs::info,
                      "[RTSP] resume changed audio {}ch -> {}ch: rebuilding audio pipeline "
                      "(relaunch the app for native surround)",
                      existing->audio_channels(), s.audio.channels);
            existing->rebuild_audio(s);
          }
          // The tunnel dies with the client's network, so vhci will already have unplugged
          // anything whose link dropped. Re-import only those; a device still attached must be
          // left alone, since re-plugging reads as a disconnect to the game.
          usbip::ImportManager::instance().reconcile_session(s.session_id);
          // No IDR here: the client hasn't pinged yet, so it would be dropped. The UDP listener
          // forces one when the client's first ping arrives.
          existing->retarget();
          return;
        }
      }

      // FRESH LAUNCH: tear down the previous session BEFORE launching -- else the two collide on
      // the X11 display (:0 "Address already in use"). Drop the lock during the blocking teardown.
      std::shared_ptr<media::MediaSession> previous;
      {
        std::lock_guard<std::mutex> lk(*media_mtx);
        previous.swap(media_holder);
      }
      if (previous)
        previous->stop();
      previous.reset();

      logs::log(logs::info, "[RTSP] PLAY for session {} -- LAUNCH: starting pipeline + app",
                s.session_id);
      auto ms = media::MediaSession::start(sess, render_node);
      std::lock_guard<std::mutex> lk(*media_mtx);
      media_holder = ms;
    });
  });

  std::thread http_thread([&state]() { HTTPServers::start_http(state); });
  HTTPServers::start_https(state); // blocks
  http_thread.join();
  rtsp_thread.join();
  control_server.stop();
  control_thread.join();
  return 0;
}
