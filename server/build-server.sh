#!/usr/bin/env bash
# Build the steam-stream-server binary + unit tests. Third-party deps are git submodules under
# server/third_party, so builds work offline once they are checked out.
set -euo pipefail

ROOT=/home/jackh/steam-stream
SRV="$ROOT/server"
OUT="$ROOT/build/server"
IMAGE=steam-stream-builder:m1

mkdir -p "$OUT"

if [[ ! -f "$SRV/third_party/nanors/rs.c" ]]; then
  git -C "$ROOT" submodule update --init
fi

echo "==> compiling steam-stream-server + tests (CMake, in $IMAGE)"
docker run --rm \
  -v "$SRV":/work -v "$OUT":/out \
  "$IMAGE" -c '
    set -e
    cmake -S /work -B /tmp/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_SERVER=ON
    cmake --build /tmp/build -j"$(nproc)" \
      --target steam-stream-server test_pairing test_rtsp test_control \
               test_fec test_aes test_xml test_session_key test_uinput test_chord
    for b in steam-stream-server test_pairing test_rtsp test_control \
             test_fec test_aes test_xml test_session_key test_uinput test_chord; do
      cp -v "/tmp/build/$b" /out/
    done
  '

echo "==> done: $OUT/{steam-stream-server,test_pairing,test_rtsp,test_control,test_fec,test_aes,test_xml,test_session_key,test_uinput}"
echo "    (full structured suite: server/tests/run-tests.sh)"
