#!/usr/bin/env python3
"""GP0 differ — compare two 1024x512x16 VRAM dumps (ours vs Beetle) from replaying the SAME
captured GP0 trace. Identical input ⇒ any pixel difference is a pure rasterizer-fidelity
difference (blend/modulation), with no live game-state alignment.

Usage:
  tools/gpu_differ/diff.py OURS.vram BEETLE.vram [--region X,Y,W,H] [--top N] [--ppm OUTDIR]

VRAM is 1024x512 little-endian uint16 (BGR555 + mask bit15). --region restricts the compare to a
rectangle (e.g. the displayed framebuffer); default = full VRAM. --ppm writes ours/beetle/absdiff
PPMs of the region for eyeballing. Differing display coords are printed ready to feed PROVAT/POLYAT
on the live port to identify the responsible primitive.
"""
import sys, struct, argparse

VW, VH = 1024, 512

def load(path):
    with open(path, "rb") as f:
        data = f.read()
    n = VW * VH
    if len(data) < n * 2:
        sys.exit(f"{path}: short VRAM ({len(data)} bytes, need {n*2})")
    return struct.unpack(f"<{n}H", data[:n * 2])

def rgb(p):  # BGR555 -> (r,g,b) 0..31, mask stripped
    return (p & 31, (p >> 5) & 31, (p >> 10) & 31)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ours"); ap.add_argument("beetle")
    ap.add_argument("--region", default=None, help="X,Y,W,H")
    ap.add_argument("--top", type=int, default=25)
    ap.add_argument("--tol", type=int, default=0,
                    help="ignore pixels whose every per-channel |Δ| <= tol (filters dither/rounding noise)")
    ap.add_argument("--ppm", default=None, help="dir to write ours/beetle/absdiff PPMs")
    ap.add_argument("--mask", action="store_true", help="also compare the mask bit (bit15)")
    a = ap.parse_args()

    A, B = load(a.ours), load(a.beetle)
    if a.region:
        x0, y0, w, h = (int(v) for v in a.region.split(","))
    else:
        x0, y0, w, h = 0, 0, VW, VH

    keep = 0x8000 if a.mask else 0x7FFF
    diffs = []   # (chan_dist, x, y, pa, pb)
    chanhist = [0] * 32
    for y in range(y0, y0 + h):
        row = y * VW
        for x in range(x0, x0 + w):
            pa, pb = A[row + x] & keep, B[row + x] & keep
            if pa == pb:
                continue
            ra, ga, ba = rgb(pa); rb, gb, bb = rgb(pb)
            mx = max(abs(ra-rb), abs(ga-gb), abs(ba-bb))
            if mx <= a.tol:
                continue
            d = abs(ra - rb) + abs(ga - gb) + abs(ba - bb)
            diffs.append((d, x, y, A[row + x], B[row + x]))
            chanhist[min(31, mx)] += 1

    total = w * h
    print(f"region ({x0},{y0}) {w}x{h} = {total} px  |  differing: {len(diffs)} "
          f"({100.0*len(diffs)/total:.3f}%)  mask-compared={a.mask}")
    if not diffs:
        print("IDENTICAL ✓")
        return
    xs = [d[1] for d in diffs]; ys = [d[2] for d in diffs]
    print(f"bbox of diffs: ({min(xs)},{min(ys)})-({max(xs)},{max(ys)})")
    print("max per-channel-delta histogram (5-bit units):")
    for i, c in enumerate(chanhist):
        if c:
            print(f"  |Δ|={i:2d}: {c}")
    diffs.sort(reverse=True)
    print(f"top {a.top} pixels by channel-distance (x,y in VRAM | display coord = subtract region origin):")
    print(f"  {'vram(x,y)':>14} {'ours BGR555':>22} {'beetle BGR555':>22}  dist")
    for d, x, y, pa, pb in diffs[:a.top]:
        ra, ga, ba = rgb(pa); rb, gb, bb = rgb(pb)
        print(f"  ({x:4d},{y:3d})  ours={pa&0x7FFF:04X}({ra:2d},{ga:2d},{ba:2d}) "
              f"beetle={pb&0x7FFF:04X}({rb:2d},{gb:2d},{bb:2d})  d={d}")

    if a.ppm:
        import os
        os.makedirs(a.ppm, exist_ok=True)
        def write_ppm(name, fn):
            with open(os.path.join(a.ppm, name), "wb") as f:
                f.write(f"P6\n{w} {h}\n255\n".encode())
                buf = bytearray()
                for y in range(y0, y0 + h):
                    for x in range(x0, x0 + w):
                        buf += bytes(fn(A[y*VW+x], B[y*VW+x]))
                f.write(buf)
        write_ppm("ours.ppm",   lambda pa, pb: [c << 3 for c in rgb(pa)])
        write_ppm("beetle.ppm", lambda pa, pb: [c << 3 for c in rgb(pb)])
        # absdiff: amplify x4 so subtle blend diffs are visible
        def adf(pa, pb):
            ra, ga, ba = rgb(pa & 0x7FFF); rb, gb, bb = rgb(pb & 0x7FFF)
            return [min(255, abs(ra-rb) << 5), min(255, abs(ga-gb) << 5), min(255, abs(ba-bb) << 5)]
        write_ppm("absdiff.ppm", adf)
        print(f"wrote {a.ppm}/ours.ppm beetle.ppm absdiff.ppm")

if __name__ == "__main__":
    main()
