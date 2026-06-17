#!/usr/bin/env python3
# Diff the headless VK captures from vk_depth_diff.sh. Reports, per frame:
#   ctl%  = default-vs-default (determinism control; should be ~0 — else captures are noisy)
#   diff% = default-vs-native-depth (the real change)  + max channel delta
# and writes an amplified red diff mask (default vs depth) per frame.
import sys, os

def readppm(p):
    with open(p, 'rb') as f:
        assert f.readline().strip() == b'P6'
        w, h = map(int, f.readline().split()); f.readline()
        return w, h, f.read()

def diff(a, b):
    n = 0; mx = 0
    for i in range(0, len(a), 3):
        d = max(abs(a[i]-b[i]), abs(a[i+1]-b[i+1]), abs(a[i+2]-b[i+2]))
        if d: n += 1
        if d > mx: mx = d
    return n, mx, len(a)//3

def mask(a, b, w, h, path):
    out = bytearray(len(a))
    for i in range(0, len(a), 3):
        d = max(abs(a[i]-b[i]), abs(a[i+1]-b[i+1]), abs(a[i+2]-b[i+2]))
        out[i] = 255 if d else a[i]//3
        out[i+1] = 0 if d else a[i+1]//3
        out[i+2] = 0 if d else a[i+2]//3
    with open(path, 'wb') as f:
        f.write(b'P6\n%d %d\n255\n' % (w, h) + bytes(out))

def main():
    out, first, last, step = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
    print(f"  frame |   ctl%%  | default-vs-depth%%  maxΔ")
    print(f"  ------+---------+-------------------------")
    for fr in range(first, last+1, step):
        name = f"vk_{fr:05d}.ppm"
        po, pn, pc = (os.path.join(out, d, name) for d in ('off', 'on', 'ctl'))
        if not (os.path.exists(po) and os.path.exists(pn)):
            print(f"  {fr:5d} | (missing capture)"); continue
        w, h, a = readppm(po); _, _, b = readppm(pn)
        nd, mx, tot = diff(a, b)
        ctl = ""
        if os.path.exists(pc):
            _, _, c = readppm(pc); nc, _, _ = diff(a, c)
            ctl = f"{100*nc/tot:6.2f}%"
        mask(a, b, w, h, os.path.join(out, f"diff_{fr:05d}.ppm"))
        print(f"  {fr:5d} | {ctl:>7} | {100*nd/tot:14.2f}%  {mx:4d}")
    print(f"\n  masks: {out}/diff_*.ppm (red = pixels where native-depth differs from default)")

if __name__ == '__main__':
    main()
