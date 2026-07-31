#!/usr/bin/env bash
# steam-stream container entrypoint: GOW cont-init.d as root, then PulseAudio as retro,
# then exec the server (root, setuid-drops to retro per app launch).
set -uo pipefail

source /opt/gow/bash-lib/utils.sh

PUID="${PUID:-1000}"
PGID="${PGID:-1000}"
UNAME="${UNAME:-retro}"
HOME="${HOME:-/home/retro}"
# A private runtime dir we own end-to-end; created before cont-init so 10-setup_user can chown it.
export XDG_RUNTIME_DIR="/tmp/sockets"
mkdir -p "${XDG_RUNTIME_DIR}/pulse"

# GOW container init (root only); guard each script so a non-essential failure never aborts boot.
if [ "$(id -u)" = "0" ]; then
  for init_script in /etc/cont-init.d/*.sh; do
    gow_log "[entrypoint] cont-init: ${init_script}"
    # shellcheck source=/dev/null
    source "${init_script}" || gow_log "[entrypoint] WARN: ${init_script} returned non-zero (continuing)"
  done
fi

# libgbm needs nvidia-drm_gbm.so for GPU glamor, but GOW's 30-nvidia.sh only looks for the
# allocator under /usr/lib/x86_64-linux-gnu/ while CDI injects it elsewhere; resolve + symlink it.
if [ "$(id -u)" = "0" ]; then
  GBM_DIR=/usr/lib/x86_64-linux-gnu/gbm
  if [ ! -e "${GBM_DIR}/nvidia-drm_gbm.so" ]; then
    ALLOC="$(ldconfig -p 2>/dev/null | awk '/libnvidia-allocator\.so\.1/{print $NF; exit}')"
    if [ -n "${ALLOC}" ] && [ -e "${ALLOC}" ]; then
      mkdir -p "${GBM_DIR}"
      ln -sf "${ALLOC}" "${GBM_DIR}/nvidia-drm_gbm.so"
      gow_log "[entrypoint] created GBM backend ${GBM_DIR}/nvidia-drm_gbm.so -> ${ALLOC}"
    else
      gow_log "[entrypoint] WARN: libnvidia-allocator.so.1 not found; Xwayland glamor may be software-only"
    fi
  fi
fi

chown -R "${PUID}:${PGID}" "${XDG_RUNTIME_DIR}"
chmod 700 "${XDG_RUNTIME_DIR}"

# Clear stale X sockets a hard-killed Xwayland may have left; sticky dir so uid retro can rebind.
mkdir -p /tmp/.X11-unix
chmod 1777 /tmp/.X11-unix
rm -f /tmp/.X11-unix/X* 2>/dev/null || true

# The Moonlight identity (cert.pem/key.pem/uuid + clients/) gets its own mount, deliberately
# independent of /home/retro: Steam churns that tree constantly and anything that resets the
# retro account takes the pairings with it. Resolved rather than baked into the image ENV so an
# old compose file without the new mount keeps using its still-persistent legacy location
# instead of silently landing on the container layer.
on_mount() {
  [ -d "$1" ] || return 1
  [ "$(stat -c %d "$1" 2>/dev/null)" != "$(stat -c %d / 2>/dev/null)" ]
}

DEFAULT_STATE_DIR=/var/lib/steam-stream
LEGACY_STATE_DIR="${HOME}/.steam-stream"
if [ -n "${STEAM_STREAM_STATE_DIR:-}" ]; then
  STATE_DIR="${STEAM_STREAM_STATE_DIR}"
elif on_mount "${DEFAULT_STATE_DIR}"; then
  STATE_DIR="${DEFAULT_STATE_DIR}"
else
  STATE_DIR="${LEGACY_STATE_DIR}"
fi
mkdir -p "${STATE_DIR}/clients"

# cp, not mv: the legacy copy stays behind as a rollback path if the image is downgraded.
if [ "${STATE_DIR}" != "${LEGACY_STATE_DIR}" ] && [ ! -s "${STATE_DIR}/key.pem" ] \
   && [ -s "${LEGACY_STATE_DIR}/key.pem" ]; then
  gow_log "[entrypoint] migrating Moonlight state ${LEGACY_STATE_DIR} -> ${STATE_DIR}"
  cp -a "${LEGACY_STATE_DIR}/." "${STATE_DIR}/" \
    || gow_log "[entrypoint] WARN: state migration failed -- clients may need to re-pair"
fi

if ! on_mount "${STATE_DIR}"; then
  gow_log "[entrypoint] ###################################################################"
  gow_log "[entrypoint] WARNING: state dir ${STATE_DIR} is on the container filesystem."
  gow_log "[entrypoint] The server identity and all Moonlight pairings will be LOST when this"
  gow_log "[entrypoint] container is recreated. Mount a host dir at ${DEFAULT_STATE_DIR}."
  gow_log "[entrypoint] ###################################################################"
fi

chown -R "${PUID}:${PGID}" "${STATE_DIR}" 2>/dev/null || true
gow_log "[entrypoint] state dir: ${STATE_DIR}"

# No udevd runs in-container, so the server injects fake-udev hotplug events + hwdb entries for its
# virtual gamepad (see fake_udev.cpp). SDL/Steam (uid retro) read /run/udev/data to classify the
# pad; the server (root) writes it. World-rwx so both sides work regardless of uid.
mkdir -p /run/udev/data
# /run/udev/control MUST exist before anything creates a udev monitor. libudev binds a monitor to
# the netlink group only if this file is present AT MONITOR-CREATION TIME; a monitor created
# before it exists is permanently deaf and never reports an error. Steam/SDL creates its monitor
# at startup, so without this the controller can silently never appear. fake_udev also creates it
# on first plug(), but that only saves us because a pad happens to be cold-plugged before Steam
# launches -- luck of ordering, not design. Verified by test_fake_udev_netlink.
touch /run/udev/control
chmod -R 0777 /run/udev

# USB/IP device import: vhci_hcd's attach/detach live in sysfs, which Docker mounts read-only for
# non-privileged containers. The sysfs SUPERBLOCK is rw (only the mount carries `ro`) and we hold
# CAP_SYS_ADMIN, so a remount is enough. Optional -- warn and continue if it fails, since only USB
# passthrough needs it.
if [ -d /sys/devices/platform/vhci_hcd.0 ]; then
  if [ ! -w /sys/devices/platform/vhci_hcd.0/attach ]; then
    if mount -o remount,rw /sys 2>/dev/null; then
      gow_log "[entrypoint] remounted /sys rw (USB/IP device import)"
    else
      gow_log "[entrypoint] WARN: /sys is read-only -- USB device import will be disabled"
    fi
  fi
else
  gow_log "[entrypoint] vhci_hcd not loaded on the host -- USB device import unavailable"
fi

# The server mknods imported devices' usbfs nodes under here, into the container's PRIVATE /dev
# (never the host's devtmpfs -- we chown them, and doing that through a bind mount would mutate
# the host).
mkdir -p /dev/bus/usb
export STEAM_STREAM_HOST_UDEV_DATA="${STEAM_STREAM_HOST_UDEV_DATA:-/run/host-udev/data}"

# PULSE_SERVER must be unset while the daemon starts (it refuses to autospawn otherwise);
# exported only once the daemon is up.
PULSE_SOCK="unix:${XDG_RUNTIME_DIR}/pulse/native"
gow_log "[entrypoint] starting PulseAudio + steam-stream null sink as ${UNAME}"
gosu "${UNAME}" env XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR}" PULSE_SERVER= \
  pulseaudio --start --exit-idle-time=-1 --log-target=stderr \
  || gow_log "[entrypoint] WARN: pulseaudio --start returned non-zero"
for _ in $(seq 1 20); do
  gosu "${UNAME}" env XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR}" pactl info >/dev/null 2>&1 && break
  sleep 0.5
done
# Default stereo sink so audio works before the first stream; the server re-runs pulse-sink.sh
# with the client's negotiated channel count (2/6/8) at each fresh session launch.
gosu "${UNAME}" env XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR}" \
  /opt/steam-stream/pulse-sink.sh 2 \
  || gow_log "[entrypoint] WARN: could not create steam-stream null sink"
gow_log "[entrypoint] pulse sinks: $(gosu "${UNAME}" env XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR}" pactl list short sinks 2>/dev/null | tr '\n' ';')"

export PULSE_SERVER="${PULSE_SOCK}"
export PULSE_SINK="steam-stream"

# Hand off to the server (runs as root, drops to retro per launch).
export GST_PLUGIN_PATH="${GST_PLUGIN_PATH:-/usr/local/lib/x86_64-linux-gnu/gstreamer-1.0}"
export LD_LIBRARY_PATH="/usr/local/nvidia/lib:/usr/local/nvidia/lib64:${LD_LIBRARY_PATH:-}"
export STEAM_STREAM_RENDER_NODE="${STEAM_STREAM_RENDER_NODE:-/dev/dri/renderD129}"
export STEAM_STREAM_STATE_DIR="${STATE_DIR}"
export STEAM_STREAM_RUN_UID="${PUID}" STEAM_STREAM_RUN_GID="${PGID}" STEAM_STREAM_RUN_USER="${UNAME}"
export STEAM_STREAM_HOME="${HOME}"

gow_log "[entrypoint] exec steam-stream-server (as root; setuid-drops to ${UNAME} per launch)"
exec steam-stream-server
