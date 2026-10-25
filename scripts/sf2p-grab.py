#!/usr/bin/env python3
"""Read an SF2P preview stream (from indraw 'stream' mode) and save frames as PPM.

The device sends it on stdout; pipe it here. Each frame is a 16-byte header
"SF2P" + little-endian width, height, byte-count, then width*height*3 RGB bytes.

Usage, on the phone over ssh or locally:
    indraw --log indraw.log stream 0 4096x3072 raw=4096x3072 < /dev/null \
        | python3 sf2p-grab.py --count 5 --out /tmp

Saves frame_000.ppm .. ; --every N saves one in N; --count stops after that many.
"""
import argparse, struct, sys, os

ap = argparse.ArgumentParser()
ap.add_argument("--out", default=".", help="output directory")
ap.add_argument("--count", type=int, default=3, help="how many frames to save")
ap.add_argument("--every", type=int, default=15, help="save one frame in EVERY")
args = ap.parse_args()
os.makedirs(args.out, exist_ok=True)

f = sys.stdin.buffer
seen = saved = 0
buf = b""
def readn(n):
    global buf
    while len(buf) < n:
        chunk = f.read(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    out, buf = buf[:n], buf[n:]
    return out

# Resync to a SF2P magic if needed.
while saved < args.count:
    hdr = readn(16)
    if hdr is None:
        break
    if hdr[:4] != b"SF2P":
        # resync: drop one byte and retry (rare)
        buf = hdr[1:] + buf
        continue
    w, h, nbytes = struct.unpack("<III", hdr[4:16])
    if nbytes != w * h * 3 or w <= 0 or h <= 0 or nbytes > 100_000_000:
        print(f"bad header {w}x{h} {nbytes}", file=sys.stderr)
        continue
    pix = readn(nbytes)
    if pix is None:
        break
    seen += 1
    if seen % args.every == 0:
        path = os.path.join(args.out, f"frame_{saved:03d}.ppm")
        with open(path, "wb") as o:
            o.write(f"P6\n{w} {h}\n255\n".encode()); o.write(pix)
        print(f"saved {path} ({w}x{h})", file=sys.stderr)
        saved += 1
print(f"done: saw {seen} frames, saved {saved}", file=sys.stderr)
