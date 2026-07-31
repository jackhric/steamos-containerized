// Drives synthetic chords into a virtual pad so Steam's reaction can be observed
// without a Moonlight client. Needs /dev/uinput; run inside the session container
// while Steam is up, then watch ~/.steam/*/logs/controller_ui.txt.
//
// Usage: test_chord [hold_ms]

#include <charconv>
#include <helpers/logger.hpp>
#include <server/uinput.hpp>
#include <thread>

using namespace std::chrono_literals;

namespace {
constexpr std::uint32_t kHome = 0x0400;
constexpr std::uint32_t kA = 0x1000;
constexpr std::uint32_t kX = 0x4000;

// Steam samples the pad asynchronously, so a chord has to be held across several
// of its polls to register -- a single update() is invisible.
void hold(input::VirtualGamepad &pad, std::uint32_t flags, int ms, short rx = 0, short ry = 0) {
  for (int elapsed = 0; elapsed < ms; elapsed += 16) {
    pad.update(flags, 0, 0, 0, 0, rx, ry);
    std::this_thread::sleep_for(16ms);
  }
}
} // namespace

int main(int argc, char **argv) {
  logs::init(logs::info);
  int hold_ms = 900;
  if (argc > 1)
    std::from_chars(argv[1], argv[1] + std::char_traits<char>::length(argv[1]), hold_ms);

  if (!input::uinput_available()) {
    logs::log(logs::error, "/dev/uinput not available");
    return 1;
  }
  auto pad = input::VirtualGamepad::create();
  if (!pad) {
    logs::log(logs::error, "failed to create virtual gamepad");
    return 1;
  }

  // Steam ignores a pad that appears and immediately acts; let it enumerate first.
  logs::log(logs::info, "pad created; settling for 3s before driving chords");
  hold(*pad, 0, 3000);

  logs::log(logs::info, "[1/4] Guide alone ({}ms)", hold_ms);
  hold(*pad, kHome, hold_ms);
  hold(*pad, 0, 700);

  logs::log(logs::info, "[2/4] X alone ({}ms)", hold_ms);
  hold(*pad, kX, hold_ms);
  hold(*pad, 0, 700);

  logs::log(logs::info, "[3/4] Guide+X chord ({}ms) -- expect SHOW_KEYBOARD", hold_ms);
  hold(*pad, kHome, 250);          // Guide down first, as a real chord is pressed
  hold(*pad, kHome | kX, hold_ms); // then X while Guide is still held
  hold(*pad, kHome, 150);          // release X before Guide
  hold(*pad, 0, 700);

  logs::log(logs::info, "[4/4] Guide+right-stick (known-good chord)", hold_ms);
  hold(*pad, kHome, 250);
  hold(*pad, kHome, hold_ms, 28000, 0);
  hold(*pad, 0, 700);

  logs::log(logs::info, "done -- check controller_ui.txt for chord/OSK lines");
  return 0;
}
