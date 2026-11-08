#!/usr/bin/env python3
"""Minimal loopback RTSP server for tests (H.264 over RTP/TCP interleaved, Digest auth).

Test helper only: binds 127.0.0.1, serves a synthetic ffmpeg stream, never touches the network.
Run with `python3 -I tests/tools/rtsp_loopback.py --port 18554 [--user U --password P] [--pattern testsrc2]`.

Why not `ffmpeg -f rtsp -rtsp_flags listen`? That flag only works for the demuxer; the RTSP muxer
ignores it and tries to connect out. So this tiny server exists instead. One shared encoder feeds
all clients; each client starts at the next IDR (like a real camera, the first frame waits for a
keyframe, so --gop sets the worst-case wait).
"""
import argparse
import base64
import hashlib
import queue
import re
import select
import socket
import struct
import subprocess
import sys
import threading
import time

p = argparse.ArgumentParser()
p.add_argument("--port", type=int, required=True)
p.add_argument("--user")
p.add_argument("--password")
p.add_argument("--pattern", default="testsrc", help="ffmpeg lavfi source: testsrc, testsrc2, smptebars")
p.add_argument("--size", default="704x576")
p.add_argument("--fps", type=int, default=25)
p.add_argument("--gop", type=int, default=25)
p.add_argument("--stall-after", type=float, default=0, help="stop sending media N s after PLAY (keeps socket open)")
p.add_argument("--drop-after", type=float, default=0, help="close the connection N s after PLAY")
args = p.parse_args()

REALM, NONCE = "loopback", "0123456789abcdef0123456789abcdef"
clients, clients_lock = [], threading.Lock()
sps_pps = {}


def log(msg):
    print(f"[rtsp:{args.port}] {msg}", file=sys.stderr, flush=True)


def md5(s):
    return hashlib.md5(s.encode()).hexdigest()


def auth_ok(method, hdrs):
    if not args.user:
        return True
    a = hdrs.get("authorization", "")
    if not a.lower().startswith("digest "):
        return False
    kv = dict(re.findall(r'(\w+)="?([^",]*)"?', a[7:]))
    if kv.get("username") != args.user or kv.get("nonce") != NONCE:
        return False
    ha1 = md5(f"{args.user}:{REALM}:{args.password}")
    ha2 = md5(f"{method}:{kv.get('uri', '')}")
    return kv.get("response") == md5(f"{ha1}:{NONCE}:{ha2}")


# ---------- encoder -> access units ----------
def split_nals(buf):
    """Yield complete NAL units (without start codes) from an Annex-B bytearray, keeping the tail."""
    out, i, start = [], 0, None
    while True:
        j = buf.find(b"\x00\x00\x01", i)
        if j < 0:
            break
        if start is not None:
            end = j - 1 if j > 0 and buf[j - 1] == 0 else j
            out.append(bytes(buf[start:end]))
        start = j + 3
        i = start
    del buf[: (start - 3) if start is not None else 0]
    return out


def rtp_packets(nal, ts, seq, marker):
    pkts = []
    if len(nal) <= 1400:
        hdr = bytes([0x80, (0x80 if marker else 0) | 96]) + (seq & 0xFFFF).to_bytes(2, "big") + ts.to_bytes(4, "big") + b"\x00\x00\x00\x01"
        pkts.append(hdr + nal)
        seq += 1
    else:
        ind, typ = (nal[0] & 0xE0) | 28, nal[0] & 0x1F
        data, off = nal[1:], 0
        while off < len(data):
            chunk = data[off : off + 1400]
            first, last = off == 0, off + len(chunk) >= len(data)
            fu = (0x80 if first else 0) | (0x40 if last else 0) | typ
            m = marker and last
            hdr = bytes([0x80, (0x80 if m else 0) | 96]) + (seq & 0xFFFF).to_bytes(2, "big") + ts.to_bytes(4, "big") + b"\x00\x00\x00\x01"
            pkts.append(hdr + bytes([ind, fu]) + chunk)
            seq += 1
            off += len(chunk)
    return pkts, seq


def encoder_thread():
    cmd = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-re", "-f", "lavfi", "-i",
           f"{args.pattern}=size={args.size}:rate={args.fps}", "-c:v", "libx264", "-preset", "ultrafast",
           "-pix_fmt", "yuv420p", "-g", str(args.gop), "-bf", "0", "-x264-params",
           "rc-lookahead=0:sync-lookahead=0:threads=1:repeat-headers=1", "-f", "h264", "-"]
    pr = subprocess.Popen(cmd, stdout=subprocess.PIPE, stdin=subprocess.DEVNULL)
    buf, pending, frame = bytearray(), [], 0
    while True:
        chunk = pr.stdout.read1(65536)
        if not chunk:
            log("encoder ended")
            return
        buf += chunk
        for nal in split_nals(buf):
            if not nal:
                continue
            t = nal[0] & 0x1F
            if t in (7, 8):
                sps_pps[t] = nal
            if t in (1, 5):
                au = (frame, t == 5, pending + [nal])
                pending, frame = [], frame + 1
                with clients_lock:
                    for c in clients:
                        c.put(au)
            else:
                pending.append(nal)


# ---------- RTSP session ----------
def serve(conn, addr):
    conn.settimeout(None)
    q, f = queue.Queue(maxsize=200), conn.makefile("rb")
    seq, playing, synced, t_play = 0, False, False, 0.0
    last_ts, last_ts_time, last_sr, npk, noct = 0, 0.0, 0.0, 0, 0

    def send(data):
        conn.sendall(data)

    def reply(code, text, cseq, extra="", body=""):
        if body:
            extra += f"Content-Length: {len(body)}\r\n"
        send(f"RTSP/1.0 {code} {text}\r\nCSeq: {cseq}\r\n{extra}\r\n{body}".encode())

    def read_request():
        # RTP interleaved frames from the client (RTCP) start with '$'
        first = f.read(1)
        if not first:
            return None
        while first == b"$":
            f.read(1)
            n = int.from_bytes(f.read(2), "big")
            f.read(n)
            first = f.read(1)
            if not first:
                return None
        lines = [first + f.readline()]
        while True:
            ln = f.readline()
            if ln in (b"\r\n", b"\n", b""):
                break
            lines.append(ln)
        return [x.decode("latin1").rstrip("\r\n") for x in lines]

    try:
        while True:
            if playing:
                r, _, _ = select.select([conn], [], [], 0.02)
                if not r:
                    pass_media = True
                else:
                    pass_media = False
                if pass_media:
                    now = time.time()
                    if last_ts_time and now - last_sr >= 1.0:  # RTCP sender report: gives the client real PTS
                        ntp = now + 2208988800
                        rtp_now = (last_ts + int((now - last_ts_time) * 90000)) & 0xFFFFFFFF
                        sr = struct.pack("!BBHIIIIII", 0x80, 200, 6, 1, int(ntp), int((ntp % 1) * 2**32), rtp_now, npk, noct)
                        send(b"$\x01" + len(sr).to_bytes(2, "big") + sr)
                        last_sr = now
                    if args.drop_after and time.time() - t_play > args.drop_after:
                        log("dropping connection (test knob)")
                        return
                    stalled = args.stall_after and time.time() - t_play > args.stall_after
                    try:
                        frame, idr, nals = q.get_nowait()
                    except queue.Empty:
                        continue
                    if stalled:
                        continue
                    if not synced and not idr:
                        continue
                    synced = True
                    ts = (frame * 90000 // args.fps) & 0xFFFFFFFF
                    for k, nal in enumerate(nals):
                        pk, seq = rtp_packets(nal, ts, seq, k == len(nals) - 1)
                        for x in pk:
                            send(b"$\x00" + len(x).to_bytes(2, "big") + x)
                            npk, noct = npk + 1, noct + len(x) - 12
                    last_ts, last_ts_time = ts, time.time()
                    continue
            req = read_request()
            if req is None:
                return
            method, uri, _ = (req[0].split(" ") + ["", ""])[:3]
            hdrs = {k.lower(): v.strip() for k, _, v in (h.partition(":") for h in req[1:])}
            cseq = hdrs.get("cseq", "0")
            if method != "OPTIONS" and not auth_ok(method, hdrs):
                reply(401, "Unauthorized", cseq, f'WWW-Authenticate: Digest realm="{REALM}", nonce="{NONCE}"\r\n')
                log(f"{method} -> 401")
                continue
            if method == "OPTIONS":
                reply(200, "OK", cseq, "Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, GET_PARAMETER\r\n")
            elif method == "DESCRIBE":
                while 7 not in sps_pps or 8 not in sps_pps:
                    time.sleep(0.05)
                sprop = base64.b64encode(sps_pps[7]).decode() + "," + base64.b64encode(sps_pps[8]).decode()
                sdp = ("v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\ns=loopback\r\nt=0 0\r\nm=video 0 RTP/AVP 96\r\n"
                       "a=rtpmap:96 H264/90000\r\n"
                       f"a=fmtp:96 packetization-mode=1;sprop-parameter-sets={sprop}\r\na=control:track1\r\n")
                reply(200, "OK", cseq, f"Content-Base: {uri}/\r\nContent-Type: application/sdp\r\n", sdp)
            elif method == "SETUP":
                reply(200, "OK", cseq, "Session: 12345678\r\nTransport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n")
            elif method == "PLAY":
                reply(200, "OK", cseq, "Session: 12345678\r\nRange: npt=0.000-\r\n")
                with clients_lock:
                    clients.append(q)
                playing, t_play = True, time.time()
                log(f"PLAY from {addr[0]}:{addr[1]}")
            elif method == "TEARDOWN":
                reply(200, "OK", cseq, "Session: 12345678\r\n")
                return
            else:
                reply(200, "OK", cseq)
    except (OSError, ValueError):
        pass
    finally:
        with clients_lock:
            if q in clients:
                clients.remove(q)
        log(f"client {addr[0]}:{addr[1]} gone")
        try:
            conn.close()
        except OSError:
            pass


def main():
    threading.Thread(target=encoder_thread, daemon=True).start()
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", args.port))
    srv.listen(8)
    log("listening on 127.0.0.1")
    while True:
        c, a = srv.accept()
        threading.Thread(target=serve, args=(c, a), daemon=True).start()


main()
