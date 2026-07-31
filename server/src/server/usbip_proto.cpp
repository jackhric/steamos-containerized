#include <server/usbip_proto.hpp>

#include <algorithm>

namespace usbip {

namespace {

void put_be16(std::string &s, std::uint16_t v) {
  s.push_back(static_cast<char>((v >> 8) & 0xff));
  s.push_back(static_cast<char>(v & 0xff));
}

void put_be32(std::string &s, std::uint32_t v) {
  s.push_back(static_cast<char>((v >> 24) & 0xff));
  s.push_back(static_cast<char>((v >> 16) & 0xff));
  s.push_back(static_cast<char>((v >> 8) & 0xff));
  s.push_back(static_cast<char>(v & 0xff));
}

std::uint16_t be16(const std::uint8_t *p) {
  return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8) | p[1]);
}

std::uint32_t be32(const std::uint8_t *p) {
  return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
         (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

// Fixed-width NUL-padded char field. The wire always carries the full width; a field with no NUL
// at all is still valid and simply uses every byte.
std::string fixed_str(const std::uint8_t *p, std::size_t width) {
  auto end = std::find(p, p + width, '\0');
  return std::string(reinterpret_cast<const char *>(p), static_cast<std::size_t>(end - p));
}

std::string op_header(std::uint16_t code) {
  std::string s;
  s.reserve(kOpCommonSize);
  put_be16(s, kVersion);
  put_be16(s, code);
  put_be32(s, 0);
  return s;
}

} // namespace

std::string encode_req_devlist() { return op_header(OP_REQ_DEVLIST); }

std::string encode_req_import(const std::string &busid) {
  auto s = op_header(OP_REQ_IMPORT);
  // Truncate rather than overrun: a busid this long is a caller bug, and the field is fixed width.
  auto id = busid.substr(0, kBusIdSize - 1);
  s.append(id);
  s.append(kBusIdSize - id.size(), '\0');
  return s;
}

bool decode_op_common(const std::uint8_t *buf, std::size_t len, OpCommon &out) {
  if (!buf || len < kOpCommonSize)
    return false;
  out.version = be16(buf);
  out.code = be16(buf + 2);
  out.status = be32(buf + 4);
  return true;
}

bool decode_usb_device(const std::uint8_t *buf, std::size_t len, UsbDevice &out) {
  if (!buf || len < kUsbDeviceSize)
    return false;

  const std::uint8_t *p = buf;
  out.path = fixed_str(p, kPathSize);
  p += kPathSize;
  out.busid = fixed_str(p, kBusIdSize);
  p += kBusIdSize;

  out.busnum = be32(p);
  p += 4;
  out.devnum = be32(p);
  p += 4;
  out.speed = be32(p);
  p += 4;

  out.id_vendor = be16(p);
  p += 2;
  out.id_product = be16(p);
  p += 2;
  out.bcd_device = be16(p);
  p += 2;

  out.dev_class = *p++;
  out.dev_subclass = *p++;
  out.dev_protocol = *p++;
  out.config_value = *p++;
  out.num_configurations = *p++;
  out.num_interfaces = *p++;

  out.interfaces.clear();
  return true;
}

bool decode_devlist_body(const std::uint8_t *buf, std::size_t len, std::vector<UsbDevice> &out) {
  out.clear();
  if (!buf || len < 4)
    return false;

  std::uint32_t ndev = be32(buf);
  // ndev is remote-controlled, so bound it by what the buffer could possibly hold before
  // allocating anything. Each device costs at least kUsbDeviceSize.
  if (ndev > (len - 4) / kUsbDeviceSize)
    return false;

  std::size_t off = 4;
  for (std::uint32_t i = 0; i < ndev; i++) {
    if (off > len)
      return false;

    UsbDevice d;
    if (!decode_usb_device(buf + off, len - off, d))
      return false;
    off += kUsbDeviceSize;

    // Interface descriptors follow each device, so the stride depends on the device's own
    // bNumInterfaces -- you cannot index this list by a fixed record size.
    std::size_t need = static_cast<std::size_t>(d.num_interfaces) * kUsbInterfaceSize;
    if (len - off < need)
      return false;
    d.interfaces.reserve(d.num_interfaces);
    for (std::uint8_t k = 0; k < d.num_interfaces; k++) {
      const std::uint8_t *ip = buf + off + static_cast<std::size_t>(k) * kUsbInterfaceSize;
      d.interfaces.push_back({ip[0], ip[1], ip[2]}); // 4th byte is padding
    }
    off += need;

    out.push_back(std::move(d));
  }
  return true;
}

} // namespace usbip
