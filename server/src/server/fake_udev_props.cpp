// Pure property/hash helpers for fake_udev, split out so they can be unit-tested with no netlink
// socket, no /run/udev and no Boost.Log link. The I/O lives in fake_udev.cpp.
//
// Every byte here is ABI with libudev subscribers. A wrong subsystem hash is dropped by the
// kernel's socket filter before any subscriber sees it, with no error reported anywhere -- which
// is exactly why this file exists separately and is pinned by test_fake_udev.

#include <server/fake_udev.hpp>

#include <atomic>
#include <chrono>
#include <cstring>

namespace fake_udev {

namespace detail {

// MurmurHash2 (public domain, Austin Appleby) -- libudev hashes the subsystem string with this
// into the netlink header so kernel socket filters can match it. Must be byte-identical to
// systemd's, so this is copied verbatim from Wolf's fake-udev.
std::uint32_t murmur2(const void *key, int len, std::uint32_t seed) {
  const std::uint32_t m = 0x5bd1e995;
  const int r = 24;
  std::uint32_t h = seed ^ len;
  const unsigned char *data = static_cast<const unsigned char *>(key);
  while (len >= 4) {
    std::uint32_t k;
    std::memcpy(&k, data, 4);
    k *= m;
    k ^= k >> r;
    k *= m;
    h *= m;
    h ^= k;
    data += 4;
    len -= 4;
  }
  switch (len) {
  case 3: h ^= data[2] << 16; [[fallthrough]];
  case 2: h ^= data[1] << 8;  [[fallthrough]];
  case 1: h ^= data[0];
    h *= m;
  }
  h ^= h >> 13;
  h *= m;
  h ^= h >> 15;
  return h;
}

std::vector<std::string> class_props(const Device &dev) {
  if (dev.id_input_class == "mouse")
    return {"ID_INPUT_MOUSE"};
  if (dev.id_input_class == "keyboard")
    return {"ID_INPUT_KEYBOARD", "ID_INPUT_KEY"};
  return {"ID_INPUT_JOYSTICK"};
}

// The udev "add"/"remove" event properties. Mirrors Wolf's gen_udev_base_event + the
// ID_INPUT_* tags SDL/libinput key off of.
std::map<std::string, std::string> base_event(const Device &dev, const char *action) {
  auto now = std::chrono::system_clock::now();
  auto ts = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

  // Was a hardcoded "7". We now emit several events per device instead of one, and some
  // subscribers order or dedupe on this.
  static std::atomic<unsigned> seq{1000};

  std::map<std::string, std::string> ev = {
      {"ACTION", action},
      {"SEQNUM", std::to_string(seq.fetch_add(1))},
      {"USEC_INITIALIZED", std::to_string(ts)},
      {"SUBSYSTEM", dev.subsystem},
      {"DEVPATH", dev.syspath},
  };

  if (!dev.devtype.empty())
    ev["DEVTYPE"] = dev.devtype;

  // A usb_interface has no device node; emitting DEVNAME/MAJOR/MINOR for one would describe a
  // node that does not exist.
  if (dev.has_node()) {
    ev["DEVNAME"] = dev.devnode;
    ev["MAJOR"] = std::to_string(dev.major);
    ev["MINOR"] = std::to_string(dev.minor);
  }

  // The ID_INPUT_*/uaccess block is evdev-specific. Emitting it for a hidraw or usb device would
  // make libinput try to claim something that is not an input device.
  if (dev.subsystem == "input") {
    ev["ID_INPUT"] = "1";
    ev["ID_SERIAL"] = "noserial";
    ev["TAGS"] = ":seat:uaccess:";
    ev["CURRENT_TAGS"] = ":seat:uaccess:";
    for (const auto &p : class_props(dev))
      ev[p] = "1";
  }

  // Last, so a caller can override anything computed above.
  for (const auto &[k, v] : dev.extra_props)
    ev[k] = v;
  return ev;
}

// udev property payload: NUL-separated KEY=VALUE pairs, trailing NUL.
std::string serialize_props(const std::map<std::string, std::string> &props) {
  std::string out;
  for (const auto &[k, v] : props) {
    out += k;
    out += '=';
    out += v;
    out += '\0';
  }
  return out;
}

// udev's own naming, verified against this host's /run/udev/data: char devices are
// "c<major>:<minor>", block devices "b<major>:<minor>", and anything without a node is
// "+<subsystem>:<sysname>" (e.g. "+usb:3-1:1.2").
std::string hwdb_path(const Device &dev) {
  if (!dev.has_node())
    return std::string(kUdevDataDir) + "/+" + dev.subsystem + ":" + dev.sysname;
  return std::string(kUdevDataDir) + "/c" + std::to_string(dev.major) + ":" +
         std::to_string(dev.minor);
}

// Matches Wolf's get_udev_hw_db_entries. E: exported props, G/Q: tags, V: version.
// The mere existence of this file also makes udev_device_get_is_initialized() true,
// which libinput requires before it will accept a path-added device.
std::string hwdb_contents(const Device &dev) {
  // USB/IP devices supply the host's real entry, which udevd already computed by running every
  // rule (ID_VENDOR_ID, ID_MODEL_ID, ID_SERIAL_SHORT...). Synthesizing that would be guesswork.
  if (!dev.hwdb_override.empty())
    return dev.hwdb_override;

  std::string out = "E:ID_INPUT=1\n";
  for (const auto &p : class_props(dev))
    out += "E:" + p + "=1\n";
  out += "E:ID_BUS=usb\n"
         "G:seat\n"
         "G:uaccess\n"
         "Q:seat\n"
         "Q:uaccess\n"
         "V:1\n";
  return out;
}

} // namespace detail

} // namespace fake_udev
