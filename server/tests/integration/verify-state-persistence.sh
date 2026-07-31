#!/usr/bin/env bash
# Regression guard for the /home/retro wipe: the GOW base image's 10-setup_user.sh ran
# `userdel -r $(id -nu $PUID)`, which on the SECOND start of a container (once uid 1000 is
# `retro`, home /home/retro) recursively deleted the contents of the mounted home -- taking
# Steam's login and the Moonlight identity with it.
#
# The bug only reproduces on a restart of the SAME container, because the `retro` passwd entry
# lives in the container layer. `docker run --rm` twice does NOT reproduce it: use
# `docker create` + two `docker start`s.
#
# No GPU needed for cases 1 and 3; case 2 boots the real entrypoint and only needs the server
# to reach init.
set -uo pipefail

IMAGE="${1:-local/steam-stream:latest}"
NAME="ss-state-verify"
WORK="$(mktemp -d)"
trap 'docker rm -f "$NAME" >/dev/null 2>&1; rm -rf "$WORK"' EXIT
docker rm -f "$NAME" >/dev/null 2>&1 || true

fail=0
chk() { if [ "$1" = 1 ]; then echo "PASS: $2"; else echo "FAIL: $2"; fail=1; fi; }

echo "################ 1: cont-init user setup must not delete \$HOME ################"
HOME_DIR="$WORK/home"
mkdir -p "$HOME_DIR/.steam-stream/clients" "$HOME_DIR/.steam"
echo MARKER > "$HOME_DIR/MARKER"
echo fake-cert > "$HOME_DIR/.steam-stream/cert.pem"
echo fake-client > "$HOME_DIR/.steam-stream/clients/test.pem"
chmod -R 777 "$HOME_DIR"

docker create --name "$NAME" \
  -v "$HOME_DIR":/home/retro \
  -e PUID=1000 -e PGID=1000 -e UNAME=retro -e HOME=/home/retro \
  -e XDG_RUNTIME_DIR=/tmp/sockets \
  --entrypoint /bin/bash "$IMAGE" \
  -c 'set -uo pipefail; mkdir -p /tmp/sockets; source /opt/gow/bash-lib/utils.sh; \
      source /etc/cont-init.d/10-setup_user.sh; \
      echo "passwd1000: $(getent passwd 1000)"; echo "home: $(ls -A /home/retro | tr "\n" " ")"' >/dev/null

for boot in 1 2; do
  echo "-- boot $boot --"
  docker start -a "$NAME" 2>&1 | sed 's/^/   /'
done
docker rm -f "$NAME" >/dev/null 2>&1

survived=1
for f in MARKER .steam-stream/cert.pem .steam-stream/clients/test.pem .steam; do
  [ -e "$HOME_DIR/$f" ] || { echo "   missing after 2 boots: $f"; survived=0; }
done
chk "$survived" "mounted home survives two container starts"

echo "################ 2: server identity is stable across a restart ################"
STATE="$WORK/state"
HOME2="$WORK/home2"
mkdir -p "$STATE/clients" "$HOME2"
openssl req -x509 -newkey rsa:2048 -nodes -keyout "$WORK/cli.key" -out "$WORK/cli.crt" \
  -days 3650 -subj "/CN=state-verify-client" >/dev/null 2>&1
cp "$WORK/cli.crt" "$STATE/clients/seed.pem"
# Seed a complete server identity too, so ANY "Generating a NEW" line is a real regression.
openssl req -x509 -newkey rsa:2048 -nodes -keyout "$STATE/key.pem" -out "$STATE/cert.pem" \
  -days 3650 -subj "/CN=state-verify-server" >/dev/null 2>&1
printf '%s' "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee" > "$STATE/uuid"
chmod -R 777 "$STATE" "$HOME2"

docker create --name "$NAME" --network none \
  -v "$HOME2":/home/retro -v "$STATE":/var/lib/steam-stream \
  -e STEAM_STREAM_LOG_LEVEL=INFO \
  "$IMAGE" >/dev/null

wait_up() {
  for _ in $(seq 1 40); do
    docker logs "$NAME" 2>&1 | grep -q "steam-stream-server starting" && return 0
    sleep 1
  done
  return 1
}

docker start "$NAME" >/dev/null
wait_up || echo "   WARN: server did not log 'starting' on boot 1"
sha_before="$(sha256sum "$STATE/cert.pem" 2>/dev/null | cut -d' ' -f1)"
uuid_before="$(cat "$STATE/uuid" 2>/dev/null)"

docker restart "$NAME" >/dev/null
wait_up || echo "   WARN: server did not log 'starting' on boot 2"
sha_after="$(sha256sum "$STATE/cert.pem" 2>/dev/null | cut -d' ' -f1)"
uuid_after="$(cat "$STATE/uuid" 2>/dev/null)"

LOG="$(docker logs "$NAME" 2>&1)"
echo "$LOG" | grep -E "10-setup_user|state dir|paired client|Generating a NEW" | tail -20 | sed 's/^/   /'

[ -n "$sha_before" ] && [ "$sha_before" = "$sha_after" ] && chk 1 "cert.pem unchanged across restart" \
  || chk 0 "cert.pem unchanged across restart (before=${sha_before:0:12} after=${sha_after:0:12})"
[ -n "$uuid_before" ] && [ "$uuid_before" = "$uuid_after" ] && chk 1 "uuid unchanged across restart" \
  || chk 0 "uuid unchanged across restart"
grep -q "Loaded 1 paired client(s)" <<<"$LOG" && chk 1 "seeded client still paired" \
  || chk 0 "seeded client still paired"
grep -q "Generating a NEW server certificate" <<<"$LOG" && chk 0 "no new identity generated" \
  || chk 1 "no new identity generated"
docker rm -f "$NAME" >/dev/null 2>&1

echo "################ 3: legacy \$HOME/.steam-stream migrates to the new mount ################"
LEGACY_HOME="$WORK/home3"
NEW_STATE="$WORK/state3"
mkdir -p "$LEGACY_HOME/.steam-stream/clients" "$NEW_STATE"
cp "$STATE/cert.pem" "$LEGACY_HOME/.steam-stream/cert.pem" 2>/dev/null
cp "$STATE/key.pem" "$LEGACY_HOME/.steam-stream/key.pem" 2>/dev/null
cp "$STATE/uuid" "$LEGACY_HOME/.steam-stream/uuid" 2>/dev/null
cp "$WORK/cli.crt" "$LEGACY_HOME/.steam-stream/clients/seed.pem"
chmod -R 777 "$LEGACY_HOME" "$NEW_STATE"
legacy_sha="$(sha256sum "$LEGACY_HOME/.steam-stream/cert.pem" | cut -d' ' -f1)"

docker create --name "$NAME" --network none \
  -v "$LEGACY_HOME":/home/retro -v "$NEW_STATE":/var/lib/steam-stream \
  "$IMAGE" >/dev/null
docker start "$NAME" >/dev/null
wait_up || echo "   WARN: server did not log 'starting'"
docker logs "$NAME" 2>&1 | grep -E "migrating|state dir|paired client" | sed 's/^/   /'
migrated_sha="$(sha256sum "$NEW_STATE/cert.pem" 2>/dev/null | cut -d' ' -f1)"
docker rm -f "$NAME" >/dev/null 2>&1

[ "$legacy_sha" = "$migrated_sha" ] && chk 1 "legacy state migrated to /var/lib/steam-stream" \
  || chk 0 "legacy state migrated to /var/lib/steam-stream"

echo "################ result ################"
[ $fail -eq 0 ] && echo "=== STATE PERSISTENCE VERIFY PASSED ===" \
                || echo "=== STATE PERSISTENCE VERIFY FAILED ==="
exit $fail
