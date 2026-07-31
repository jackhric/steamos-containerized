#pragma once

// USB/IP wire format -- kernel Documentation/usb/usbip_protocol.rst, version 1.1.1.
//
// We are the IMPORTER (the usbip *client* role): the Moonlight client machine exports its
// peripheral via usbipd, and we pull it in through vhci_hcd. That inverts the streaming roles,
// which is the single most common source of confusion here.
//
// Encode/decode only -- no sockets, no fds -- so the whole format is unit-testable without a
// kernel or a network. The I/O lives in usb_tunnel.cpp, the attach mechanics in vhci.cpp.
//
// Every multi-byte field on the wire is BIG-endian. The two char arrays (path, busid) are byte
// strings and are NOT swapped; that asymmetry is easy to get wrong in both directions.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace usbip {

constexpr std::uint16_t kVersion = 0x0111;

constexpr std::uint16_t OP_REQ_DEVLIST = 0x8005;
constexpr std::uint16_t OP_REP_DEVLIST = 0x0005;
constexpr std::uint16_t OP_REQ_IMPORT = 0x8003;
constexpr std::uint16_t OP_REP_IMPORT = 0x0003;

constexpr int kDefaultPort = 3240;

// enum usb_device_speed (uapi/linux/usb/ch9.h). NB: sysfs `speed` is unrelated -- it holds the
// bitrate as a decimal string ("12", "480", "5000"), so it cannot be fed to the wire directly.
enum Speed : std::uint32_t {
  SPEED_UNKNOWN = 0,
  SPEED_LOW = 1,
  SPEED_FULL = 2,
  SPEED_HIGH = 3,
  SPEED_WIRELESS = 4,
  SPEED_SUPER = 5,
  SPEED_SUPER_PLUS = 6,
};

constexpr std::size_t kOpCommonSize = 8;
constexpr std::size_t kUsbDeviceSize = 312; // packed struct usbip_usb_device
constexpr std::size_t kUsbInterfaceSize = 4;
constexpr std::size_t kPathSize = 256;
constexpr std::size_t kBusIdSize = 32;

struct OpCommon {
  std::uint16_t version = 0;
  std::uint16_t code = 0;
  std::uint32_t status = 0; // non-zero means the peer refused; no body follows
};

struct UsbInterface {
  std::uint8_t iface_class = 0;
  std::uint8_t iface_subclass = 0;
  std::uint8_t iface_protocol = 0;
};

struct UsbDevice {
  std::string path;
  std::string busid;
  std::uint32_t busnum = 0;
  std::uint32_t devnum = 0;
  std::uint32_t speed = 0; // usb_device_speed, not a bitrate
  std::uint16_t id_vendor = 0;
  std::uint16_t id_product = 0;
  std::uint16_t bcd_device = 0;
  std::uint8_t dev_class = 0;
  std::uint8_t dev_subclass = 0;
  std::uint8_t dev_protocol = 0;
  std::uint8_t config_value = 0;
  std::uint8_t num_configurations = 0;
  std::uint8_t num_interfaces = 0;

  // Only a DEVLIST reply carries these; an IMPORT reply does not.
  std::vector<UsbInterface> interfaces;

  // What vhci's `attach` wants: the device's identity on the EXPORTER's bus, not ours.
  std::uint32_t devid() const { return (busnum << 16) | devnum; }
};

// ---- encode (we only ever send requests) ----
std::string encode_req_devlist();
std::string encode_req_import(const std::string &busid);

// ---- decode ----
// All of these are total: they never read past `len` and return false rather than throwing.
// Everything they parse is attacker-controlled -- the exporter is the remote end.
bool decode_op_common(const std::uint8_t *buf, std::size_t len, OpCommon &out);
bool decode_usb_device(const std::uint8_t *buf, std::size_t len, UsbDevice &out);
// The DEVLIST body, i.e. everything AFTER the op header: u32 ndev, then ndev repetitions of
// (usb_device + bNumInterfaces x interface). The per-device stride is variable.
bool decode_devlist_body(const std::uint8_t *buf, std::size_t len, std::vector<UsbDevice> &out);

} // namespace usbip
