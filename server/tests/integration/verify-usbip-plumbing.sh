#!/usr/bin/env bash
# Container-side preconditions for USB/IP import. Checks the plumbing, not the feature -- runnable
# before any of the import code is wired in.
#
# Run against the LIVE container:  bash verify-usbip-plumbing.sh [container-name]
#
# Every one of these has a failure mode that surfaces much later as something unhelpful: a bare
# EPERM deep inside a stream, or a controller that silently never appears.

set -uo pipefail
C="${1:-steam-stream}"

PASS=0; FAIL=0
ok()   { printf '\033[1;32m[ OK ]\033[0m %s\n' "$*"; PASS=$((PASS+1)); }
no()   { printf '\033[1;31m[FAIL]\033[0m %s\n' "$*"; FAIL=$((FAIL+1)); }
skip() { printf '\033[1;33m[skip]\033[0m %s\n' "$*"; }

dex() { docker exec "$C" sh -c "$1" 2>/dev/null; }

docker ps --filter "name=^${C}$" --format '{{.Names}}' | grep -q "^${C}$" \
  || { no "container '$C' is not running"; exit 1; }

echo "=== udev ==="
# The one the netlink test caught: libudev binds a monitor only if this exists at monitor-creation
# time. Steam creates its monitor at startup, so this must be present from boot.
dex 'test -e /run/udev/control' \
  && ok "/run/udev/control exists (Steam's udev monitor can bind)" \
  || no "/run/udev/control MISSING -- a monitor created now would be permanently deaf"
dex 'test -d /run/udev/data' && ok "/run/udev/data exists" || no "/run/udev/data missing"
dex 'test -d /run/host-udev/data' \
  && ok "host udev database mounted at /run/host-udev/data" \
  || no "/run/host-udev/data not mounted (imported devices will fall back to synthesized hwdb)"
dex 'test -r /run/host-udev/data' && ok "host udev database is readable" \
  || no "host udev database not readable"

echo
echo "=== sysfs / vhci ==="
if dex 'test -d /sys/devices/platform/vhci_hcd.0'; then
  ok "vhci_hcd sysfs visible in-container"
  dex 'test -w /sys/devices/platform/vhci_hcd.0/attach' \
    && ok "vhci attach is WRITABLE (the /sys remount worked)" \
    || no "vhci attach not writable -- /sys is still read-only, import will be disabled"
  n=$(dex 'grep -c "^hs\|^ss" /sys/devices/platform/vhci_hcd.0/status')
  [ "${n:-0}" -gt 0 ] && ok "vhci status readable ($n ports)" || no "vhci status unreadable"
else
  skip "vhci_hcd not loaded on the host (modprobe vhci-hcd) -- import unavailable"
fi

echo
echo "=== device majors + cgroup authorisation ==="
HID_MAJOR=$(dex 'awk "\$2==\"hidraw\"{print \$1}" /proc/devices' | head -1)
[ -n "$HID_MAJOR" ] && ok "hidraw char major is $HID_MAJOR" || no "hidraw major not in /proc/devices"

# The real test of a device_cgroup_rule is whether an mknod'd node can be OPENED. The rule is
# invisible from inside the container, so this is the only way to check it.
probe_major() { # <major> <minor> <label>
  local maj="$1" min="$2" label="$3"
  local node="/tmp/.usbip-probe-$maj-$min"
  if ! dex "rm -f $node && mknod $node c $maj $min"; then
    no "$label: mknod failed (CAP_MKNOD missing?)"
    return
  fi
  if dex "exec 3<>$node"; then
    ok "$label: major $maj is authorised by the device cgroup (open succeeded)"
  else
    # ENXIO/ENODEV means "no such device behind that node" -- the cgroup DID allow it, which is
    # what we are testing. EPERM means the cgroup rule is missing.
    local err
    err=$(docker exec "$C" sh -c "exec 3<>$node" 2>&1)
    if echo "$err" | grep -qi 'not permitted\|Operation not permitted'; then
      no "$label: EPERM -- add 'c $maj:* rmw' to device_cgroup_rules in docker-compose.yml"
    else
      ok "$label: major $maj authorised by the cgroup (no device behind it, which is expected)"
    fi
  fi
  dex "rm -f $node"
}
probe_major 189 0 "usb_device"
[ -n "$HID_MAJOR" ] && probe_major "$HID_MAJOR" 0 "hidraw"
probe_major 13 63 "input (existing, regression check)"

echo
echo "=== node creation targets ==="
dex 'test -d /dev/bus/usb' && ok "/dev/bus/usb exists for mknod" || no "/dev/bus/usb missing"
dex 'test -w /dev' && ok "/dev is writable (container-private tmpfs)" || no "/dev not writable"

echo
echo "=== environment ==="
for v in STEAM_STREAM_HOST_UDEV_DATA; do
  val=$(dex "tr '\\0' '\\n' < /proc/1/environ | grep '^$v=' | cut -d= -f2-")
  [ -n "$val" ] && ok "$v=$val" || no "$v not exported to the server process"
done

echo
echo "-------------------------------------------------------------"
echo "PASS/FAIL: $PASS/$((PASS+FAIL))"
[ "$FAIL" -eq 0 ] && { echo "RESULT: GREEN"; exit 0; } || { echo "RESULT: RED"; exit 1; }
