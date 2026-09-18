#!/usr/bin/env bash
# Resume/reconnect self-test: LAUNCH, disconnect, then /resume and assert the app is reused
# (not relaunched), RTP is re-targeted to the new source port, an IDR is forced once the new
# client pings, the RTP seq/frame numbering restarts at that IDR (M11: Moonlight's queue starts
# at seq 0 / frame 1 and rejects a mid-count join), video keeps flowing, and the AES key rotates.
# RTP "clients" are UDP sockets pinging from port A then port B.
set -uo pipefail
BIN="${SERVER_BIN:-/out/steam-stream-server}"
ST=/tmp/ss-m9
rm -rf "$ST"; mkdir -p "$ST/clients"

openssl req -x509 -newkey rsa:2048 -nodes -keyout "$ST/cli.key" -out "$ST/cli.crt" \
  -days 3650 -subj "/CN=test-client" >/dev/null 2>&1
cp "$ST/cli.crt" "$ST/clients/manual.pem"

# A long-lived stand-in app: the resume path only reuses the pipeline while the launched app is
# alive (M11), and phase 3 kills it to assert the fall-through to a relaunch.
printf '#!/bin/sh\nexec sleep 300\n' > "$ST/app.sh"; chmod 755 "$ST/app.sh"
export STEAM_STREAM_STATE_DIR="$ST" STEAM_STREAM_LOG_LEVEL=INFO STEAM_STREAM_AUDIO_TEST=1 \
       STEAM_STREAM_LAUNCH_SCRIPT="$ST/app.sh"
export GST_PLUGIN_PATH=/plugins
"$BIN" >"$ST/server.log" 2>&1 &
SRV=$!
sleep 2

python3 - "$ST" 2>&1 <<'PY' | tee "$ST/client.log"
import http.client, ssl, sys, time, socket, re, subprocess
ST=sys.argv[1]
def https_ctx(crt,key):
    c=ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT); c.check_hostname=False
    c.verify_mode=ssl.CERT_NONE; c.load_cert_chain(crt,key); return c
ctx=https_ctx(ST+"/cli.crt",ST+"/cli.key")

def rest(path):
    h=http.client.HTTPSConnection("127.0.0.1",47984,context=ctx,timeout=5)
    h.request("GET",path); return h.getresponse().read().decode()

def rtsp(msg):
    s=socket.create_connection(("127.0.0.1",48010),timeout=5)
    s.sendall(msg.encode()); s.shutdown(socket.SHUT_WR)
    d=b""
    while True:
        c=s.recv(4096)
        if not c: break
        d+=c
    s.close(); return d.decode(errors="replace")

SDP=("v=0\na=x-nv-video[0].clientViewportWd:1280 \na=x-nv-video[0].clientViewportHt:720 \n"
     "a=x-nv-video[0].maxFPS:60 \na=x-nv-video[0].packetSize:1024 \n"
     "a=x-nv-vqos[0].bw.maximumBitrateKbps:15500 \na=x-nv-vqos[0].fec.minRequiredFecPackets:2 \n"
     "a=x-nv-general.featureFlags:167 \na=x-nv-audio.surround.numChannels:2 \n"
     "a=x-nv-aqos.packetDuration:5 \na=x-nv-video[0].encoderCscMode:0 \na=x-nv-vqos[0].bitStreamFormat:0 \n")

def handshake_and_play(fake_ip):
    assert "200 OK" in rtsp("OPTIONS rtsp://%s:48010 RTSP/1.0\r\nCSeq:1\r\nHost:%s\r\n\r\n"%(fake_ip,fake_ip))
    rtsp("DESCRIBE rtsp://%s:48010 RTSP/1.0\r\nCSeq:2\r\nHost:%s\r\nAccept: application/sdp\r\n\r\n"%(fake_ip,fake_ip))
    for sid in ("video","audio","control"):
        rtsp("SETUP streamid=%s/0/0 RTSP/1.0\r\nCSeq:3\r\nHost:%s\r\n\r\n"%(sid,fake_ip))
    assert "200 OK" in rtsp("ANNOUNCE streamid=control/13/0 RTSP/1.0\r\nCSeq:6\r\nHost:%s\r\n"
                            "Content-type: application/sdp\r\nContent-length:%d\r\n\r\n%s"%(fake_ip,len(SDP),SDP))
    assert "200 OK" in rtsp("PLAY rtsp://%s:48010 RTSP/1.0\r\nCSeq:7\r\nHost:%s\r\n\r\n"%(fake_ip,fake_ip))

def parse_video(d):
    # RTP(12) + reserved(4) + NV_VIDEO_PACKET(16) + payload; frame_type is byte 3 of the payload's
    # VideoShortHeader on SOF packets (see gst-plugin/video.hpp, moonlight/data-structures.hpp).
    if len(d) < 36: return None
    flags=d[24]
    return dict(seq=int.from_bytes(d[2:4],"big"), frame=int.from_bytes(d[20:24],"little"),
                sof=bool(flags & 0x4), ftype=d[35] if flags & 0x4 else None)

def ping_from(src_port, n=10, capture=0):
    # Pings video+audio from src_port/src_port+1; collects up to `capture` video packets received
    # on the video socket while pinging (the server only sends once it has seen a ping).
    socks=[]
    for sport,dport in ((src_port,48100),(src_port+1,48200)):
        s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
        s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
        s.bind(("127.0.0.1",sport)); socks.append((s,dport))
    socks[0][0].settimeout(0.01)
    got=[]
    for _ in range(n):
        for s,dport in socks:
            s.sendto(b"\x00"*16,("127.0.0.1",dport))
        end=time.time()+0.1
        while time.time()<end:
            try:
                d,_=socks[0][0].recvfrom(4096)
            except socket.timeout:
                continue
            if len(got)<capture:
                pk=parse_video(d)
                if pk: got.append(pk)
    for s,_ in socks: s.close()
    return got

# ---- LAUNCH ----
body=rest("/launch?appid=1&mode=1280x720x60&rikey=9d804e9d3a7d8f1b2c3d4e5f60718293&rikeyid=16909060&rtspport=48010")
FAKE_IP=re.search(r"rtsp://([0-9.]+):48010",body).group(1)
print("launch -> rtsp://%s:48010"%FAKE_IP)
handshake_and_play(FAKE_IP)
print("LAUNCH PLAY ok; pinging RTP from source port 51111 (client A)")
pk=ping_from(51111, n=12, capture=8)
print("LAUNCH client A first video packets: %s"%pk[:3])
time.sleep(2)

# ---- DISCONNECT (session persists) ----
print("=== MARKER: client A disconnected, resuming ===")
time.sleep(1)

# ---- RESUME with a rotated AES key (B != A) so re-capture is observable ----
body=rest("/resume?rtspport=48010&rikey=00112233445566778899aabbccddeeff&rikeyid=305419896")
assert "<resume>1" in body, "resume did not return <resume>1: %r"%body
FAKE_IP2=re.search(r"rtsp://([0-9.]+):48010",body).group(1)
print("resume -> rtsp://%s:48010 (same session=%s)"%(FAKE_IP2, FAKE_IP2==FAKE_IP))
handshake_and_play(FAKE_IP2)
print("RESUME PLAY ok; pinging RTP from NEW source port 52222 (client B)")
def packet_check(label, pk):
    if not pk:
        print("%s_PACKET_CHECK FAIL: no video packets received"%label); return
    f=pk[0]
    ok = f["seq"]==0 and f["frame"]==1 and f["sof"] and f["ftype"]==2
    print("%s_PACKET_CHECK %s: first packet seq=%d frame=%d sof=%s ftype=%s (want seq=0 frame=1 sof=True ftype=2 IDR)"
          %(label, "PASS" if ok else "FAIL", f["seq"], f["frame"], f["sof"], f["ftype"]))

pk=ping_from(52222, n=25, capture=8)
print("RESUME client B first video packets: %s"%pk[:3])
packet_check("RESUME", pk)
time.sleep(2)
print("=== MARKER: resume done ===")

# ---- RESUME after the app died: must relaunch (not stream the empty compositor) ----
subprocess.call(["pkill","-f","sleep 300"])
time.sleep(1)
print("=== MARKER: app killed, resuming again ===")
body=rest("/resume?rtspport=48010&rikey=ffeeddccbbaa99887766554433221100&rikeyid=305419897")
assert "<resume>1" in body, "second resume did not return <resume>1: %r"%body
FAKE_IP3=re.search(r"rtsp://([0-9.]+):48010",body).group(1)
handshake_and_play(FAKE_IP3)
print("RESUME#2 PLAY ok; pinging RTP from source port 53333 (client C)")
pk=ping_from(53333, n=40, capture=8)
print("RESUME#2 client C first video packets: %s"%pk[:3])
packet_check("RELAUNCH", pk)
time.sleep(1)
print("=== MARKER: relaunch done ===")
PY

kill $SRV 2>/dev/null; sleep 1

echo "############ relevant server log ############"
sed 's/\x1b\[[0-9;]*m//g' "$ST/server.log" \
  | grep -nE "PLAY for session|RESUME|LAUNCH:|has exited|stopping launched app|starting (video|audio) pipeline|RTP client (discovered|re-targeted)|forcing IDR|payloader armed|RTP counters reset|video appsink produced|AES key (ROTATED|UNCHANGED)|audio payloader AES key refreshed|EVP_DecryptFinal_ex failed"

echo "############ assertions ############"
log=$(sed 's/\x1b\[[0-9;]*m//g' "$ST/server.log") # strip ANSI colour codes
fail=0
# Phases 1+2 (launch, live resume) end where phase 3's dead-app resume begins.
full_log="$log"
log=$(sed -n '1,/has exited; relaunching/p' <<<"$full_log")

grep -q "RESUME: reusing running app" <<<"$log" || { echo "FAIL(c): resume did not hit RESUME reuse path"; fail=1; }
# (c) IDR is forced when client B is discovered (not before -- it would be dropped), and the
# payloader restarts its counters at that IDR so B's first packet is seq 0 / frame 1 / IDR.
grep -q "video payloader armed" <<<"$log" || { echo "FAIL(c): resume did not arm the payloader counter reset"; fail=1; }
sed -En '/(discovered|re-targeted) on :48100 -> 127.0.0.1:52222/,$p' <<<"$log" | grep -qE "video client (discovered|re-targeted): forcing IDR" \
  || { echo "FAIL(c): no IDR forced after client B (52222) was discovered"; fail=1; }
grep -q "video RTP counters reset (seq 0, frame 1) on keyframe" <<<"$log" \
  || { echo "FAIL(c): payloader did not reset its counters on the keyframe"; fail=1; }
grep -q "RESUME_PACKET_CHECK PASS" "$ST/client.log" \
  || { echo "FAIL(c): $(grep RESUME_PACKET_CHECK "$ST/client.log" || echo 'client B packet check missing')"; fail=1; }

if grep -q "stopping launched app" <<<"$log"; then echo "FAIL(a): app was stopped on resume"; fail=1; fi
n_launch=$(grep -c "LAUNCH: starting pipeline" <<<"$log")
[ "$n_launch" = "1" ] || { echo "FAIL(a): expected exactly 1 LAUNCH, got $n_launch"; fail=1; }
n_vpipe=$(grep -c "starting video pipeline" <<<"$log")
[ "$n_vpipe" = "1" ] || { echo "FAIL(a): video pipeline started $n_vpipe times (expected 1 -- reuse on resume)"; fail=1; }

# (b) RTP re-targeted from client A (51111) to client B (52222); both must appear.
grep -qE "RTP client (discovered|re-targeted) on :48100 -> 127.0.0.1:51111" <<<"$log" \
  || { echo "FAIL(b): launch RTP target (51111) not seen"; fail=1; }
grep -qE "RTP client (discovered|re-targeted) on :48100 -> 127.0.0.1:52222" <<<"$log" \
  || { echo "FAIL(b): resume did NOT re-target RTP to client B port 52222"; fail=1; }

# (d) Video RTP kept flowing after resume: client B actually received packets on its new port.
grep -qE "RESUME client B first video packets: \[\{" "$ST/client.log" \
  || { echo "FAIL(d): client B received no video RTP after resume"; fail=1; }

# AES key rotation on resume: session key rotates to B, audio payloader refreshes, and no
# EVP_DecryptFinal_ex failures (the flood the stale-key bug produced).
grep -qE "AES key ROTATED .*-> 00112233445566778899aabbccddeeff" <<<"$log" \
  || { echo "FAIL(M10-a): resume did not rotate sess->aes_key to the new key B"; fail=1; }
grep -q "audio payloader AES key refreshed for resume" <<<"$log" \
  || { echo "FAIL(M10-c): audio payloader AES key was not refreshed on resume"; fail=1; }
if grep -q "EVP_DecryptFinal_ex failed" <<<"$log"; then
  echo "FAIL(M10-b): EVP_DecryptFinal_ex failures present (stale AES key on resume)"; fail=1
fi

# (M11) Resume with the app gone falls through to a relaunch: the old group is swept, a second
# pipeline+app start, and client C gets a fresh seq 0 / frame 1 / IDR.
grep -q "has exited; relaunching instead of resuming" <<<"$full_log" \
  || { echo "FAIL(M11-app): resume with a dead app did not fall through to relaunch"; fail=1; }
n_launch_all=$(grep -c "LAUNCH: starting pipeline" <<<"$full_log")
[ "$n_launch_all" = "2" ] || { echo "FAIL(M11-app): expected 2 LAUNCHes over the whole run, got $n_launch_all"; fail=1; }
grep -q "RELAUNCH_PACKET_CHECK PASS" "$ST/client.log" \
  || { echo "FAIL(M11-app): $(grep RELAUNCH_PACKET_CHECK "$ST/client.log" || echo 'client C packet check missing')"; fail=1; }

[ $fail -eq 0 ] && echo "=== M9/M10/M11 RESUME SELF-TEST PASSED ===" || echo "=== M9/M10/M11 RESUME SELF-TEST FAILED ==="
exit $fail
