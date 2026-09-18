#include <server/pointer_sync.hpp>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <helpers/logger.hpp>
#include <map>
#include <set>
#include <string>
#include <xcb/xcb.h>

namespace input {

namespace {

std::int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Only gamescope's Xwayland sets this atom on its root. Belt-and-braces: we only
// dial the container's own socket files, so this should never see anything else.
bool is_gamescope(xcb_connection_t *c, xcb_window_t root) {
  const char *name = "GAMESCOPE_XWAYLAND_SERVER_ID";
  auto ick = xcb_intern_atom(c, 1 /*only_if_exists*/, std::strlen(name), name);
  auto *ir = xcb_intern_atom_reply(c, ick, nullptr);
  if (!ir)
    return false;
  xcb_atom_t atom = ir->atom;
  std::free(ir);
  if (atom == XCB_ATOM_NONE)
    return false;
  auto pck = xcb_get_property(c, 0, root, atom, XCB_GET_PROPERTY_TYPE_ANY, 0, 4);
  auto *pr = xcb_get_property_reply(c, pck, nullptr);
  bool present = pr && pr->type != XCB_ATOM_NONE;
  std::free(pr);
  return present;
}

struct Conn {
  xcb_connection_t *c = nullptr;
  xcb_window_t root = 0;
  // gamescope's XWayland screen size. Root coordinates are in this space, which is
  // not necessarily the compositor's output space.
  int root_w = 0, root_h = 0;
  bool primed = false; // first poll only records the position
  std::int16_t last_x = 0, last_y = 0;
  bool injected = false;
  double inj_x = 0, inj_y = 0;
};

// Display numbers of the X sockets in the container's private /tmp/.X11-unix.
// Never ":N": under network_mode: host the abstract socket @/tmp/.X11-unix/XN is
// shared with the host, and libxcb tries it first, so ":0" reaches the host's X
// server. The host never provides X to this container.
std::set<int> local_displays() {
  std::set<int> out;
  if (DIR *d = ::opendir("/tmp/.X11-unix")) {
    while (auto *e = ::readdir(d)) {
      char *end = nullptr;
      if (e->d_name[0] == 'X') {
        long n = std::strtol(e->d_name + 1, &end, 10);
        if (end != e->d_name + 1 && *end == '\0')
          out.insert(static_cast<int>(n));
      }
    }
    ::closedir(d);
  }
  return out;
}

} // namespace

PointerSync::PointerSync(InjectFn inject_abs, int out_w, int out_h)
    : inject_(std::move(inject_abs)), out_w_(out_w), out_h_(out_h) {
  thread_ = std::thread([this] { run(); });
}

PointerSync::~PointerSync() {
  stop_.store(true);
  if (thread_.joinable())
    thread_.join();
}

void PointerSync::note_client_mouse() {
  last_client_ms_.store(now_ms(), std::memory_order_relaxed);
}

void PointerSync::run() {
  std::map<int, Conn> conns; // display number -> connection
  std::set<int> rejected;    // reachable, but not gamescope -- never probe again
  std::int64_t next_discovery = 0;

  while (!stop_.load(std::memory_order_relaxed)) {
    if (now_ms() >= next_discovery) {
      next_discovery = now_ms() + 2000;
      for (int n : local_displays()) {
        if (conns.count(n) || rejected.count(n))
          continue;
        // A path DISPLAY makes libxcb open exactly that file, skipping the abstract socket.
        std::string disp = "/tmp/.X11-unix/X" + std::to_string(n);
        xcb_connection_t *c = xcb_connect(disp.c_str(), nullptr);
        if (xcb_connection_has_error(c)) {
          xcb_disconnect(c);
          continue;
        }
        xcb_window_t root = xcb_setup_roots_iterator(xcb_get_setup(c)).data->root;
        if (!is_gamescope(c, root)) {
          xcb_disconnect(c);
          rejected.insert(n);
          continue;
        }
        Conn cn{c, root};
        auto gck = xcb_get_geometry(c, root);
        if (auto *gr = xcb_get_geometry_reply(c, gck, nullptr)) {
          cn.root_w = gr->width;
          cn.root_h = gr->height;
          std::free(gr);
        }
        logs::log(logs::info, "[PTRSYNC] tracking gamescope Xwayland {} root {}x{} (output {}x{})",
                  disp, cn.root_w, cn.root_h, out_w_, out_h_);
        if (cn.root_w > 0 && out_w_ > 0 && (cn.root_w != out_w_ || cn.root_h != out_h_))
          logs::log(logs::warning,
                    "[PTRSYNC] Xwayland {} mode {}x{} != compositor output {}x{} -- gamescope's "
                    "surface may not cover the pointer space, which kills mouse input",
                    disp, cn.root_w, cn.root_h, out_w_, out_h_);
        conns[n] = cn;
      }
    }

    for (auto it = conns.begin(); it != conns.end();) {
      Conn &cn = it->second;
      if (xcb_connection_has_error(cn.c)) {
        logs::log(logs::warning, "[PTRSYNC] Xwayland :{} gone", it->first);
        xcb_disconnect(cn.c);
        it = conns.erase(it);
        continue;
      }
      // Re-read every poll: Steam retargets the Xwayland mode after launch (e.g. 4K -> 1080p via
      // GAMESCOPE_XWAYLAND_MODE_CONTROL), which leaves a size cached at connect time stale.
      auto gck = xcb_get_geometry(cn.c, cn.root);
      auto qck = xcb_query_pointer(cn.c, cn.root);
      if (auto *gr = xcb_get_geometry_reply(cn.c, gck, nullptr)) {
        if (gr->width != cn.root_w || gr->height != cn.root_h) {
          logs::log(logs::info, "[PTRSYNC] Xwayland :{} mode {}x{} -> {}x{}", it->first, cn.root_w,
                    cn.root_h, gr->width, gr->height);
          cn.root_w = gr->width;
          cn.root_h = gr->height;
          cn.primed = false;
          cn.injected = false;
        }
        std::free(gr);
      }
      if (auto *r = xcb_query_pointer_reply(cn.c, qck, nullptr)) {
        std::int16_t x = r->root_x, y = r->root_y;
        std::free(r);
        if (!cn.primed) {
          cn.primed = true;
          cn.last_x = x;
          cn.last_y = y;
        } else if (x != cn.last_x || y != cn.last_y) {
          cn.last_x = x;
          cn.last_y = y;
          // Xwayland reports (0,0) while its surface has no pointer focus (focus
          // flapping between the two Xwaylands, cursor hide). Injecting it yanks the
          // cursor to the top-left corner; record-but-skip so real moves away from
          // the origin still register as changes.
          if (x == 0 && y == 0) {
            ++it;
            continue;
          }
          // Suppressed while the client is actively mousing: the parent pointer is
          // already where the client put it; syncing then would fight the user.
          bool client_quiet = now_ms() - last_client_ms_.load(std::memory_order_relaxed) > 250;
          bool far = !cn.injected ||
                     std::abs(x - cn.inj_x) >= 2.0 || std::abs(y - cn.inj_y) >= 2.0;
          if (client_quiet && far) {
            // Converges: injected absolute parent motion propagates through the
            // compositor into gamescope, which sets its internal cursor to the same
            // coordinates, so the next poll reads back the injected position and
            // the loop settles.
            double sx = x, sy = y;
            if (cn.root_w > 0 && cn.root_h > 0 && out_w_ > 0 && out_h_ > 0) {
              sx = x * (double)out_w_ / cn.root_w;
              sy = y * (double)out_h_ / cn.root_h;
            }
            inject_(sx, sy);
            cn.injected = true;
            cn.inj_x = x;
            cn.inj_y = y;
          }
        }
      }
      ++it;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }

  for (auto &kv : conns)
    xcb_disconnect(kv.second.c);
}

} // namespace input
