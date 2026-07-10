#!/usr/bin/env python3
"""preseq_flicker.py — 60fps temporal-artifact analyzer over a `preseq` present-sequence dump.

Input: a directory of p%04d.ppm frames captured by the REPL `preseq <N> [dir]` command (every
PRESENTED frame — fps60 interleaves real and interpolated frames, so consecutive files alternate
real/interp when 60fps is on).

Detects the 30Hz-oscillation class of bug (backdrop/parallax flicker): for a horizontal band of the
image, estimate the x/y translation between consecutive frames by 1-D cross-correlation of row/column
intensity profiles. Smooth 60fps motion = displacements with CONSISTENT sign and ~half magnitude on
the interp steps. A flickering layer = displacement sign ALTERNATING frame to frame (+d, -d, +d, ...).

Usage: python3 tools/preseq_flicker.py <dir> [--bands N]
Output: per-band displacement sequence + a FLICKER verdict per band (fraction of sign-alternating
steps; > 0.4 with amplitude >= 1px = flagged).
"""
import os, sys

def readppm(p):
    with open(p, 'rb') as f:
        data = f.read()
    parts = data.split(b'\n', 3)
    w, h = map(int, parts[1].split())
    return w, h, parts[3][:w * h * 3]

def band_profile(w, h, pix, y0, y1, axis):
    """Mean-intensity profile along x (axis=0) or y (axis=1) for rows y0..y1."""
    if axis == 0:
        prof = [0] * w
        for y in range(y0, y1):
            row = y * w * 3
            for x in range(w):
                i = row + x * 3
                prof[x] += pix[i] + pix[i+1] + pix[i+2]
        n = (y1 - y0) * 3
        return [v / n for v in prof]
    else:
        prof = [0] * (y1 - y0)
        for y in range(y0, y1):
            row = y * w * 3
            s = 0
            for x in range(0, w, 2):
                i = row + x * 3
                s += pix[i] + pix[i+1] + pix[i+2]
            prof[y - y0] = s
        return prof

def shift_estimate(a, b, maxd=12):
    """Best integer shift of b vs a by minimum sum-abs-diff over the overlap."""
    best, bestd = None, None
    n = len(a)
    for d in range(-maxd, maxd + 1):
        lo, hi = max(0, d), min(n, n + d)
        if hi - lo < n // 2: continue
        s = sum(abs(a[i] - b[i - d]) for i in range(lo, hi, 2)) / (hi - lo)
        if bestd is None or s < bestd:
            bestd, best = s, d
    return best or 0

def main():
    d = sys.argv[1]
    nbands = 4
    if '--bands' in sys.argv: nbands = int(sys.argv[sys.argv.index('--bands') + 1])
    files = sorted(f for f in os.listdir(d) if f.endswith('.ppm'))
    if len(files) < 6:
        print(f"need >=6 frames, have {len(files)}"); return 1
    frames = [readppm(os.path.join(d, f)) for f in files]
    w, h = frames[0][0], frames[0][1]
    flagged = 0
    for b in range(nbands):
        y0, y1 = h * b // nbands, h * (b + 1) // nbands
        profs = [band_profile(w, h, fr[2], y0, y1, 0) for fr in frames]
        disps = [shift_estimate(profs[i], profs[i + 1]) for i in range(len(profs) - 1)]
        # sign-alternation fraction among non-zero steps
        nz = [x for x in disps if x != 0]
        alt = sum(1 for i in range(len(disps) - 1)
                  if disps[i] * disps[i + 1] < 0 and abs(disps[i]) >= 1 and abs(disps[i + 1]) >= 1)
        frac = alt / max(1, len(disps) - 1)
        verdict = "FLICKER" if (frac > 0.4 and nz) else "ok"
        if verdict == "FLICKER": flagged += 1
        print(f"band {b} (y {y0}-{y1}): dx seq {disps}  alt-frac={frac:.2f}  {verdict}")
    print(f"{'FAIL' if flagged else 'PASS'}: {flagged}/{nbands} bands flagged")
    return 1 if flagged else 0

if __name__ == '__main__':
    sys.exit(main())
