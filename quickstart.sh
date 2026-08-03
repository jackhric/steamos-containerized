#!/usr/bin/env bash
# SteamOS-Containerized quickstart: install Docker if needed, detect the GPU,
# generate a matching docker-compose.yml, and bring the container up.
#
# Run as root:  curl -fsSL <raw-url>/quickstart.sh | bash
#
# Tunables (env vars):
#   STEAM_STREAM_IMAGE        image to pull/run  (default: see IMAGE_REF below)
#   STEAM_STREAM_INSTALL_DIR  install dir        (default: /opt/steam-stream)
#   STEAM_STREAM_GPU          force vendor: nvidia | amd | intel
#   STEAM_STREAM_RENDER_NODE  force render node, e.g. /dev/dri/renderD128
#   STEAM_STREAM_LIBRARIES    colon-separated host dirs to mount as game libraries
#                             (used instead of prompting when set or non-interactive)

set -euo pipefail

# Public image ref. Blank until the image is published; falls back to a locally
# built local/steam-stream:latest if present.
IMAGE_REF="${STEAM_STREAM_IMAGE:-}"
LOCAL_IMAGE="local/steam-stream:latest"

INSTALL_DIR="${STEAM_STREAM_INSTALL_DIR:-/opt/steam-stream}"
CONTAINER_NAME="steam-stream"
HTTP_PORT=47989
HEALTH_TIMEOUT=120

# --- helpers -----------------------------------------------------------------

info() { printf '\033[1;32m[quickstart]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[quickstart]\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31m[quickstart]\033[0m ERROR: %s\n' "$*" >&2; exit 1; }

# Piped via curl|bash means stdin is the script itself; prompt on /dev/tty.
# Must test a real open: permission bits pass access(2) even without a
# controlling terminal (ssh -T, cron, systemd), where open() fails with ENXIO.
have_tty() { { : < /dev/tty; } 2>/dev/null; }
ask() { # ask <prompt> <varname> [default]
  local prompt="$1" var="$2" default="${3:-}" reply
  if have_tty; then
    printf '%s' "$prompt" > /dev/tty
    IFS= read -r reply < /dev/tty || reply=""
  else
    reply=""
  fi
  printf -v "$var" '%s' "${reply:-$default}"
}

pkg_family() {
  # shellcheck disable=SC1091
  . /etc/os-release 2>/dev/null || true
  case "${ID:-} ${ID_LIKE:-}" in
    *debian*|*ubuntu*) echo deb ;;
    *rhel*|*fedora*|*centos*) echo rpm ;;
    *suse*) echo suse ;;
    *arch*) echo arch ;;
    *) echo unknown ;;
  esac
}

# --- preflight ---------------------------------------------------------------

[ "$(id -u)" -eq 0 ] || die "must run as root (sudo)"
[ "$(uname -s)" = "Linux" ] || die "Linux only"
[ "$(uname -m)" = "x86_64" ] || die "x86_64 only (Steam has no native arm64 build)"

for tool in curl grep stat; do
  command -v "$tool" >/dev/null || die "missing required tool: $tool"
done

# uinput is how the container injects virtual gamepads/keyboard/mouse.
if [ ! -e /dev/uinput ]; then
  info "loading uinput kernel module"
  modprobe uinput || die "could not load uinput module (custom kernel without CONFIG_INPUT_UINPUT?)"
fi
[ -e /dev/uinput ] || die "/dev/uinput still missing after modprobe"

# vhci_hcd is the virtual host controller that materializes a USB device imported from the
# Moonlight client. Optional: warn rather than die, since everything else works without it.
if [ ! -d /sys/devices/platform/vhci_hcd.0 ]; then
  info "loading vhci-hcd kernel module (USB device passthrough)"
  modprobe vhci-hcd 2>/dev/null || warn "vhci-hcd unavailable — USB device passthrough disabled"
fi

# Per-module and idempotent. The old form guarded the whole block on the file not existing, so an
# upgrade over an existing install would never add a newly-required module — presenting as
# "worked yesterday, broken after a reboot".
if [ -d /etc/modules-load.d ]; then
  MLD=/etc/modules-load.d/steam-stream.conf
  touch "$MLD"
  for _m in uinput vhci-hcd; do
    if ! grep -qx "$_m" "$MLD"; then
      echo "$_m" >> "$MLD"
      info "persisted $_m autoload ($MLD)"
    fi
  done
fi

# Imported USB/IP devices are visible host-wide (Linux has no USB namespace), so without this
# rule the host desktop reads the streamed controller -- a Steam Controller in lizard mode moves
# your cursor while you play. Must sort before 73-seat-late.rules, which applies the uaccess ACL.
if [ -d /etc/udev/rules.d ] && [ -f "$(dirname "$0")/udev/72-steam-stream-usbip.rules" ]; then
  install -m 0644 "$(dirname "$0")/udev/72-steam-stream-usbip.rules" \
    /etc/udev/rules.d/72-steam-stream-usbip.rules
  udevadm control --reload 2>/dev/null || true
  info "installed udev rule keeping USB/IP devices out of the host desktop"
fi

# hidraw's char major is allocated at boot, not fixed. Resolve it now so the generated compose
# file authorises the right one; a wrong value shows up much later as a bare EPERM on open.
HIDRAW_MAJOR="$(awk '$2=="hidraw"{print $1}' /proc/devices 2>/dev/null | head -1)"
if [ -z "$HIDRAW_MAJOR" ]; then
  HIDRAW_MAJOR=243
  warn "hidraw major not listed in /proc/devices (module not loaded yet?) — assuming $HIDRAW_MAJOR"
else
  info "hidraw char major is $HIDRAW_MAJOR"
fi

[ -d /dev/dri ] || die "/dev/dri missing — no GPU driver with DRM support is loaded"
ls /dev/dri/renderD* >/dev/null 2>&1 || die "no /dev/dri/renderD* nodes — GPU driver lacks a render node"

# --- docker ------------------------------------------------------------------

if ! command -v docker >/dev/null; then
  info "Docker not found — installing"
  case "$(pkg_family)" in
    # get.docker.com does not support Arch or openSUSE.
    arch) pacman -Syu --noconfirm docker docker-compose || die "Docker install failed" ;;
    suse) zypper --non-interactive install docker docker-compose || die "Docker install failed" ;;
    *)    curl -fsSL https://get.docker.com | sh || die "Docker install failed" ;;
  esac
fi
if ! docker info >/dev/null 2>&1; then
  info "starting Docker daemon"
  systemctl enable --now docker 2>/dev/null || true
  docker info >/dev/null 2>&1 || die "Docker daemon is not running and could not be started"
fi
docker compose version >/dev/null 2>&1 || die "docker compose plugin missing (install docker-compose-plugin)"

# --- GPU detection -----------------------------------------------------------

# Map render nodes to vendors via PCI vendor id.
declare -A NODE_VENDOR=()
for node in /dev/dri/renderD*; do
  sysfs="/sys/class/drm/$(basename "$node")/device/vendor"
  [ -r "$sysfs" ] || continue
  case "$(cat "$sysfs")" in
    0x10de) NODE_VENDOR[$node]=nvidia ;;
    0x1002) NODE_VENDOR[$node]=amd ;;
    0x8086) NODE_VENDOR[$node]=intel ;;
  esac
done
[ "${#NODE_VENDOR[@]}" -gt 0 ] || die "no recognized GPU (NVIDIA/AMD/Intel) behind /dev/dri"

GPU_VENDOR="${STEAM_STREAM_GPU:-}"
if [ -z "$GPU_VENDOR" ]; then
  # Prefer discrete: nvidia > amd > intel.
  for pref in nvidia amd intel; do
    for node in "${!NODE_VENDOR[@]}"; do
      [ "${NODE_VENDOR[$node]}" = "$pref" ] && GPU_VENDOR="$pref" && break 2
    done
  done
  vendors_present="$(printf '%s\n' "${NODE_VENDOR[@]}" | sort -u | tr '\n' ' ')"
  if [ "$(printf '%s\n' "${NODE_VENDOR[@]}" | sort -u | wc -l)" -gt 1 ] && have_tty; then
    ask "Multiple GPUs found (${vendors_present}) — which to use? [${GPU_VENDOR}]: " GPU_VENDOR "$GPU_VENDOR"
  fi
fi
case "$GPU_VENDOR" in nvidia|amd|intel) ;; *) die "unknown GPU vendor '$GPU_VENDOR'" ;; esac

RENDER_NODE="${STEAM_STREAM_RENDER_NODE:-}"
if [ -z "$RENDER_NODE" ]; then
  for node in $(printf '%s\n' "${!NODE_VENDOR[@]}" | sort); do
    [ "${NODE_VENDOR[$node]}" = "$GPU_VENDOR" ] && RENDER_NODE="$node" && break
  done
fi
[ -e "$RENDER_NODE" ] || die "render node $RENDER_NODE does not exist"
info "GPU: $GPU_VENDOR ($RENDER_NODE)"

# --- GPU container runtime (CDI) ---------------------------------------------

# Engine >= 29.2 resolves compose's `gpus: all` through CDI for any vendor, but a CDI spec
# has to exist: nvidia-ctk writes one below, AMD needs amd-container-toolkit. Intel has no
# spec and doesn't need one -- VA-API works off the plain /dev/dri mount.
USE_GPUS_FIELD=0

if [ "$GPU_VENDOR" = "nvidia" ]; then
  [ -e /proc/driver/nvidia/version ] || command -v nvidia-smi >/dev/null \
    || die "NVIDIA GPU selected but no NVIDIA driver is loaded — install the proprietary driver first"

  if ! command -v nvidia-ctk >/dev/null; then
    info "installing NVIDIA Container Toolkit"
    case "$(pkg_family)" in
      deb)
        curl -fsSL https://nvidia.github.io/libnvidia-container/gpgkey \
          | gpg --batch --yes --dearmor -o /usr/share/keyrings/nvidia-container-toolkit-keyring.gpg
        curl -fsSL https://nvidia.github.io/libnvidia-container/stable/deb/nvidia-container-toolkit.list \
          | sed 's#deb https://#deb [signed-by=/usr/share/keyrings/nvidia-container-toolkit-keyring.gpg] https://#' \
          > /etc/apt/sources.list.d/nvidia-container-toolkit.list
        apt-get update -qq || die "apt update failed"
        apt-get install -y -qq nvidia-container-toolkit ;;
      rpm)
        curl -fsSL https://nvidia.github.io/libnvidia-container/stable/rpm/nvidia-container-toolkit.repo \
          > /etc/yum.repos.d/nvidia-container-toolkit.repo
        dnf install -y -q nvidia-container-toolkit ;;
      suse)
        zypper --non-interactive ar https://nvidia.github.io/libnvidia-container/stable/rpm/nvidia-container-toolkit.repo || true
        zypper --gpg-auto-import-keys --non-interactive install nvidia-container-toolkit ;;
      arch)
        # -Syu, not -Sy: installing against refreshed DBs without upgrading is a partial upgrade.
        pacman -Syu --noconfirm nvidia-container-toolkit ;;
      *)
        die "unknown distro — install nvidia-container-toolkit manually, then re-run" ;;
    esac
  fi

  info "generating CDI spec (/etc/cdi/nvidia.yaml)"
  mkdir -p /etc/cdi
  nvidia-ctk cdi generate --output=/etc/cdi/nvidia.yaml || die "CDI spec generation failed"
  USE_GPUS_FIELD=1

elif [ "$GPU_VENDOR" = "amd" ]; then
  amd_spec=(/etc/cdi/amd*)
  if [ -e "${amd_spec[0]}" ]; then
    USE_GPUS_FIELD=1
  else
    info "no AMD CDI spec in /etc/cdi — using the /dev/dri mount (install amd-container-toolkit for CDI)"
  fi
fi

if [ "$USE_GPUS_FIELD" = 1 ]; then
  # Engine >= 29.2 resolves `gpus:` through CDI; compose >= 2.30 understands the field at all.
  # A preinstalled distro Docker (e.g. Ubuntu 22.04's) can be older than both.
  engine_ver="$(docker version -f '{{.Server.Version}}' 2>/dev/null || echo 0)"
  if [ "$(printf '%s\n' "29.2.0" "$engine_ver" | sort -V | head -n1)" != "29.2.0" ]; then
    die "Docker Engine $engine_ver cannot resolve 'gpus:' via CDI (need >= 29.2) — upgrade Docker"
  fi
  compose_ver="$(docker compose version --short 2>/dev/null | tr -d v)"
  if [ "$(printf '%s\n' "2.30.0" "$compose_ver" | sort -V | head -n1)" != "2.30.0" ]; then
    die "docker compose $compose_ver is too old for the 'gpus:' field (need >= 2.30) — upgrade the compose plugin"
  fi
fi

# --- game library mounts -------------------------------------------------------

LIBRARY_MOUNTS=()
if [ -n "${STEAM_STREAM_LIBRARIES:-}" ]; then
  IFS=':' read -ra libs <<< "$STEAM_STREAM_LIBRARIES"
  for lib in "${libs[@]}"; do
    [ -n "$lib" ] || continue
    [ -d "$lib" ] || die "library path $lib does not exist"
    LIBRARY_MOUNTS+=("$lib")
  done
elif have_tty; then
  info "Optional: mount host directories as game/ROM libraries inside the container."
  while :; do
    ask "Library path (empty to finish): " lib ""
    [ -n "$lib" ] || break
    if [ ! -d "$lib" ]; then
      ask "$lib does not exist — create it? [y/N]: " mk "n"
      # shellcheck disable=SC2154  # mk is assigned by ask() via printf -v
      case "$mk" in y|Y) mkdir -p "$lib" ;; *) continue ;; esac
    fi
    LIBRARY_MOUNTS+=("$lib")
  done
fi

# --- generate docker-compose.yml -----------------------------------------------

mkdir -p "$INSTALL_DIR/home" "$INSTALL_DIR/config" "$INSTALL_DIR/state"
COMPOSE="$INSTALL_DIR/docker-compose.yml"
if [ -e "$COMPOSE" ]; then
  bak="$COMPOSE.bak.$(date +%s)"
  cp "$COMPOSE" "$bak"
  warn "existing $COMPOSE backed up to $bak"
fi

# Resolve which image to run.
if [ -z "$IMAGE_REF" ]; then
  if docker image inspect "$LOCAL_IMAGE" >/dev/null 2>&1; then
    IMAGE_REF="$LOCAL_IMAGE"
    warn "no public image configured — using locally built $LOCAL_IMAGE"
  else
    die "no public image published yet and no local $LOCAL_IMAGE found.
  Either build the image from the repo (docker compose build) or set STEAM_STREAM_IMAGE=<ref> and re-run."
  fi
fi

{
  cat <<EOF
# Generated by quickstart.sh — GPU: $GPU_VENDOR
services:
  steam-stream:
    image: $IMAGE_REF
    container_name: $CONTAINER_NAME
    restart: unless-stopped
    network_mode: host

EOF
  [ "$USE_GPUS_FIELD" = 1 ] && printf '    gpus: all\n\n'
  cat <<EOF
    devices:
      - /dev/uinput:/dev/uinput
      - /dev/dri:/dev/dri

    device_cgroup_rules:
      - "c 13:* rmw"     # input
      - "c 189:* rmw"    # usb_device -> /dev/bus/usb/*
      - "c ${HIDRAW_MAJOR}:* rmw"    # hidraw (major is allocated dynamically at boot)

    environment:
EOF
  # Zero-copy is CUDA-only; VA-API uses the plain capture path.
  if [ "$GPU_VENDOR" = "nvidia" ]; then
    echo '      WOLF_USE_ZERO_COPY: "TRUE"'
  else
    echo '      WOLF_USE_ZERO_COPY: "FALSE"'
  fi
  cat <<EOF
      STEAM_STREAM_RENDER_NODE: $RENDER_NODE
    shm_size: "2gb"
    security_opt:
      - seccomp=unconfined
      - apparmor=unconfined
    cap_add:
      - SYS_ADMIN
      - SYS_NICE
      - SYS_PTRACE
      - NET_RAW
      - MKNOD
      - NET_ADMIN
    # Steam/CEF use host IPC (shared-memory segments) for the webhelper <-> client transport.
    ipc: host
    ulimits:
      nofile:
        soft: 10240
        hard: 10240

    volumes:
      # Steam login/library, persisted on the host.
      - $INSTALL_DIR/home:/home/retro
      # Moonlight identity (cert/key/uuid + paired clients), on its own mount.
      - $INSTALL_DIR/state:/var/lib/steam-stream
      # Encoder config, seeded by the server on first run if absent.
      - $INSTALL_DIR/config:/config
      - /dev/input:/dev/input
      # Host udev database (read-only): imported USB devices already have their properties
      # computed by the host's udevd, so we copy rather than guess.
      - /run/udev/data:/run/host-udev/data:ro
EOF
  for lib in "${LIBRARY_MOUNTS[@]:-}"; do
    [ -n "$lib" ] && echo "      - $lib:$lib"
  done
} > "$COMPOSE"
info "wrote $COMPOSE"

# --- pull + run + health check ---------------------------------------------------

if [ "$IMAGE_REF" != "$LOCAL_IMAGE" ]; then
  info "pulling $IMAGE_REF"
  docker pull "$IMAGE_REF" || die "image pull failed"
fi

info "starting container"
docker compose -f "$COMPOSE" up -d

info "waiting for the server to come up (http://localhost:$HTTP_PORT, ${HEALTH_TIMEOUT}s timeout)"
deadline=$(( $(date +%s) + HEALTH_TIMEOUT ))
healthy=0
while [ "$(date +%s)" -lt "$deadline" ]; do
  state="$(docker inspect -f '{{.State.Status}}' "$CONTAINER_NAME" 2>/dev/null || echo missing)"
  if [ "$state" != "running" ]; then
    warn "container state: $state — recent logs:"
    docker logs --tail 30 "$CONTAINER_NAME" 2>&1 | sed 's/^/    /' || true
    die "container is not running"
  fi
  if curl -fsS -m 3 "http://127.0.0.1:$HTTP_PORT/serverinfo" >/dev/null 2>&1 \
     || curl -fsS -m 3 "http://127.0.0.1:$HTTP_PORT/" >/dev/null 2>&1; then
    healthy=1
    break
  fi
  sleep 2
done

if [ "$healthy" -ne 1 ]; then
  warn "server did not answer on port $HTTP_PORT within ${HEALTH_TIMEOUT}s — recent logs:"
  docker logs --tail 50 "$CONTAINER_NAME" 2>&1 | sed 's/^/    /' || true
  die "health check failed"
fi

# hostname -I is not universal (Arch ships inetutils hostname without it).
host_ip="$(hostname -I 2>/dev/null | awk '{print $1}' || true)"
[ -n "$host_ip" ] || host_ip="$(ip -4 route get 1 2>/dev/null | awk '{for(i=1;i<NF;i++) if($i=="src") print $(i+1); exit}' || true)"
pin_url="$(docker logs "$CONTAINER_NAME" 2>&1 | grep -o 'http://[^ ]*/pin/#[a-zA-Z0-9]*' | tail -n1 || true)"

info "steam-stream is up!"
cat <<EOF

  Next steps:
    1. Open Moonlight on your client and add this host: ${host_ip:-<this-machine>}
    2. When Moonlight shows a PIN, enter it at:
       ${pin_url:-http://${host_ip:-localhost}:$HTTP_PORT/pin/  (exact URL with secret: docker logs $CONTAINER_NAME | grep pin)}

  Install dir:     $INSTALL_DIR
  Compose file:    $COMPOSE
  Steam data:      $INSTALL_DIR/home
  Pairing state:   $INSTALL_DIR/state (server cert/key/uuid + paired clients — back this up)
  Encoder config:  $INSTALL_DIR/config/encoders.toml (seeded on first stream if absent)
  Logs:            docker logs -f $CONTAINER_NAME
EOF
