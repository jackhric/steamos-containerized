// vhci status-table parsing, free-port selection and attach-argument formatting. Pure: no
// syscalls, no root, and vhci_hcd does not need to be loaded.
//
// The status tables below are in the exact format drivers/usb/usbip/vhci_sysfs.c emits --
// "hs  %04u %03u " then either "%03u %08x " + sockfd + local_busid, or the all-zero placeholder.
// Ports 0..7 are the high-speed root hub, 8..15 the SuperSpeed one.

#include <cassert>
#include <cstdio>
#include <iostream>
#include <server/vhci.hpp>
#include <string>

using namespace vhci;

static void ok(const char *what) { std::cout << "[ OK ] " << what << "\n"; }

static const char *kHeader = "hub port sta spd dev      sockfd local_busid\n";

static std::string free_row(bool ss, int port) {
  char buf[128];
  std::snprintf(buf, sizeof(buf), "%s  %04u %03u 000 00000000 0000000000000000 0-0\n",
                ss ? "ss" : "hs", port, ST_NULL);
  return buf;
}

static std::string used_row(bool ss, int port, unsigned speed, std::uint32_t devid, int sockfd,
                            const char *busid, int status = ST_USED) {
  char buf[160];
  // "%06u" for sockfd, not "%u" -- verified against the sprintf literals in vhci-hcd.ko.
  std::snprintf(buf, sizeof(buf), "%s  %04u %03u %03u %08x %06u %s\n", ss ? "ss" : "hs", port,
                status, speed, devid, sockfd, busid);
  return buf;
}

// A default idle controller: 8 free hs ports, 8 free ss ports.
static std::string idle_table() {
  std::string t = kHeader;
  for (int p = 0; p < 8; p++)
    t += free_row(false, p);
  for (int p = 8; p < 16; p++)
    t += free_row(true, p);
  return t;
}

// The formatter above is ours; this pins the parser against bytes reconstructed from the
// sprintf literals compiled into vhci-hcd.ko on this host (7.1.5-arch1-2), so a kernel-side
// format change shows up here rather than as a mysteriously empty device list.
//
//   header  : "hub port" " sta spd" " dev    " "  sockfd" " local_busid\n"
//   any row : "hs  %04u %03u " / "ss  %04u %03u "
//   used    : + "%03u %08x " + "%06u %s"
//   free    : + "000 00000000 " + "0000000000000000 0-0"
static void test_golden_kernel_format() {
  const std::string golden =
      "hub port sta spd dev      sockfd local_busid\n"
      "hs  0000 004 000 00000000 0000000000000000 0-0\n"
      "hs  0001 006 002 00010002 000009 3-1\n"
      "ss  0008 004 000 00000000 0000000000000000 0-0\n";

  auto rows = parse_status(golden);
  assert(rows.size() == 3);

  assert(!rows[0].super_speed && rows[0].port == 0 && rows[0].is_free());
  assert(rows[0].local_busid == "0-0" && !rows[0].enumerated());

  // The zero-padded sockfd column must not be read as octal.
  assert(rows[1].port == 1 && rows[1].status == ST_USED);
  assert(rows[1].speed == 2 && rows[1].devid == 0x00010002);
  assert(rows[1].sockfd == 9);
  assert(rows[1].local_busid == "3-1" && rows[1].enumerated());

  assert(rows[2].super_speed && rows[2].port == 8 && rows[2].is_free());
  ok("parses the exact byte layout vhci-hcd.ko's sprintf literals produce");

  // The free-row placeholder puts 16 zeros in the sockfd column; it must parse, not poison
  // the row (status is ST_NULL there, so the value itself is irrelevant).
  assert(rows[0].sockfd == 0);
  ok("the 16-zero placeholder sockfd column parses without dropping the row");

  assert(find_free_port(rows, usbip::SPEED_HIGH) == 0);
  assert(find_free_port(rows, usbip::SPEED_SUPER) == 8);
  ok("free-port selection works off a real-format table");
}

static void test_parse() {
  auto rows = parse_status(idle_table());
  assert(rows.size() == 16);
  ok("header line is skipped; all 16 ports parse");

  assert(!rows[0].super_speed && rows[0].port == 0);
  assert(rows[7].port == 7 && !rows[7].super_speed);
  assert(rows[8].super_speed && rows[8].port == 8);
  assert(rows[15].super_speed && rows[15].port == 15);
  ok("ports 0-7 are the hs hub, 8-15 the ss hub");

  for (const auto &r : rows) {
    assert(r.is_free());
    assert(r.status == ST_NULL);
    assert(!r.enumerated());
  }
  ok("an idle table reports every port free and none enumerated");

  // A live port, with the real Puck's devid: busnum 1, devnum 2.
  std::string t = kHeader;
  t += free_row(false, 0);
  t += used_row(false, 1, 2, 0x00010002, 7, "3-1");
  auto live = parse_status(t);
  assert(live.size() == 2);
  assert(live[1].port == 1);
  assert(live[1].status == ST_USED);
  assert(live[1].speed == 2);
  assert(live[1].devid == 0x00010002); // hex column, not decimal
  assert(live[1].sockfd == 7);
  assert(live[1].local_busid == "3-1");
  assert(live[1].enumerated());
  assert(!live[1].is_free());
  ok("a used row parses speed/devid(hex)/sockfd/local_busid");
}

static void test_parse_robustness() {
  // Garbage, short rows and unknown hub types must be skipped, not fatal -- the format has
  // changed across kernel versions and we would rather lose a row than the whole table.
  std::string t = kHeader;
  t += "\n";
  t += "hs  0000\n";                                    // truncated
  t += "xx  0001 006 002 00010002 7 3-1\n";             // unknown hub
  t += "hs  zzzz 006 002 00010002 7 3-1\n";             // unparseable port
  t += free_row(false, 3);
  t += "   \n";
  auto rows = parse_status(t);
  assert(rows.size() == 1);
  assert(rows[0].port == 3);
  ok("malformed rows are skipped without dropping the valid ones");

  assert(parse_status("").empty());
  assert(parse_status(kHeader).empty());
  ok("empty input and a header-only table yield no rows");

  // Older kernels printed sockfd as a %16p pointer. The row must still parse; only sockfd is lost.
  std::string p = kHeader;
  p += "hs  0001 006 002 00010002 ffff8881005a2c00 3-1\n";
  auto ptr_rows = parse_status(p);
  assert(ptr_rows.size() == 1);
  assert(ptr_rows[0].sockfd == -1);
  assert(ptr_rows[0].local_busid == "3-1");
  assert(ptr_rows[0].enumerated());
  ok("a pointer-formatted sockfd column degrades to -1 without losing the row");

  // Missing trailing newline on the last row.
  std::string nonl = std::string(kHeader) + "hs  0002 004 000 00000000 0000000000000000 0-0";
  assert(parse_status(nonl).size() == 1);
  ok("a final row without a trailing newline still parses");
}

static void test_not_yet_enumerated() {
  // Between attach and enumeration the kernel reports NOTASSIGNED with no local_busid. That is
  // what wait_local_busid polls on, so it must read as "not ready" rather than "done".
  std::string t = kHeader;
  t += used_row(false, 1, 0, 0x00010002, 7, "0-0", ST_NOTASSIGNED);
  auto rows = parse_status(t);
  assert(rows.size() == 1);
  assert(rows[0].status == ST_NOTASSIGNED);
  assert(!rows[0].is_free());
  assert(!rows[0].enumerated());
  ok("ST_NOTASSIGNED reads as neither free nor enumerated");

  // ST_USED but local_busid still the "0-0" placeholder is also not ready.
  std::string t2 = kHeader;
  t2 += used_row(false, 1, 2, 0x00010002, 7, "0-0");
  auto r2 = parse_status(t2);
  assert(r2[0].status == ST_USED);
  assert(!r2[0].enumerated());
  ok("ST_USED with a 0-0 placeholder busid is not yet enumerated");
}

static void test_find_free_port() {
  auto idle = parse_status(idle_table());

  assert(find_free_port(idle, usbip::SPEED_HIGH) == 0);
  assert(find_free_port(idle, usbip::SPEED_FULL) == 0);
  assert(find_free_port(idle, usbip::SPEED_LOW) == 0);
  ok("low/full/high speed devices land on the hs hub");

  auto ss = find_free_port(idle, usbip::SPEED_SUPER);
  assert(ss.has_value() && *ss == 8);
  ok("SuperSpeed lands on the ss hub (port 8)");

  // vhci has no SUPER_PLUS support; we downgrade rather than let the attach be rejected, and it
  // must still land on the ss hub -- upstream's usbip puts it on hs, which enumerates wrong.
  assert(clamp_speed(usbip::SPEED_SUPER_PLUS) == usbip::SPEED_SUPER);
  auto ssp = find_free_port(idle, usbip::SPEED_SUPER_PLUS);
  assert(ssp.has_value() && *ssp == 8);
  ok("SUPER_PLUS clamps to SUPER and still lands on the ss hub");

  // Skip over ports already in use.
  std::string t = kHeader;
  t += used_row(false, 0, 2, 0x00010002, 7, "3-1");
  t += used_row(false, 1, 2, 0x00010003, 8, "3-2");
  for (int p = 2; p < 8; p++)
    t += free_row(false, p);
  for (int p = 8; p < 16; p++)
    t += free_row(true, p);
  auto partial = parse_status(t);
  assert(find_free_port(partial, usbip::SPEED_HIGH) == 2);
  ok("the first free hs port is chosen, skipping used ones");
}

static void test_exhaustion() {
  // All 8 hs ports used, ss hub entirely free. A high-speed request must fail rather than
  // silently fall through to a SuperSpeed port, which would enumerate incorrectly.
  std::string t = kHeader;
  for (int p = 0; p < 8; p++)
    t += used_row(false, p, 2, 0x00010002 + p, 7 + p, "3-1");
  for (int p = 8; p < 16; p++)
    t += free_row(true, p);
  auto rows = parse_status(t);

  assert(!find_free_port(rows, usbip::SPEED_HIGH).has_value());
  assert(!find_free_port(rows, usbip::SPEED_FULL).has_value());
  ok("hs exhaustion returns nullopt and does NOT fall through to an ss port");

  assert(find_free_port(rows, usbip::SPEED_SUPER) == 8);
  ok("the ss hub is still usable while the hs hub is full");

  // And the mirror case.
  std::string t2 = kHeader;
  for (int p = 0; p < 8; p++)
    t2 += free_row(false, p);
  for (int p = 8; p < 16; p++)
    t2 += used_row(true, p, 5, 0x00020001, 20 + p, "4-1");
  auto rows2 = parse_status(t2);
  assert(!find_free_port(rows2, usbip::SPEED_SUPER).has_value());
  assert(find_free_port(rows2, usbip::SPEED_HIGH) == 0);
  ok("ss exhaustion does not fall through to an hs port");

  assert(!find_free_port({}, usbip::SPEED_HIGH).has_value());
  ok("an empty table yields no free port");
}

static void test_format_attach() {
  // attach_store parses with sscanf(buf, "%u %u %u %u") -- port, sockfd, devid, speed, all
  // decimal. devid in particular is decimal here even though the status table prints it as hex.
  assert(format_attach(0, 7, 0x00010002, usbip::SPEED_FULL) == "0 7 65538 2");
  assert(format_attach(3, 42, 0x00030001, usbip::SPEED_HIGH) == "3 42 196609 3");
  ok("format_attach emits the exact decimal '%u %u %u %u' attach_store expects");

  assert(format_attach(8, 9, 1, usbip::SPEED_SUPER_PLUS) == "8 9 1 5");
  ok("format_attach clamps SUPER_PLUS to SUPER on the wire");
}

int main() {
  test_golden_kernel_format();
  test_parse();
  test_parse_robustness();
  test_not_yet_enumerated();
  test_find_free_port();
  test_exhaustion();
  test_format_attach();

  std::cout << "\nALL VHCI TESTS PASSED\n";
  return 0;
}
