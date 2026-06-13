#!/usr/bin/env python3
"""Faithful reimplementation of the PSX GTE RTPS projection, verified against
captured (transform, local-vertex) -> screen-vertex tuples (PSXPORT_T2_RTPDUMP).

This proves the projection half of the from-scratch reprojecting renderer: if
our RTPS reproduces the game's SX/SY bit-exactly, we can re-project the same
geometry at an interpolated transform faithfully.

Usage: reproject.py <rtp.csv> [max_rows]
"""
import sys

# --- GTE reciprocal table, generated exactly as beetle's GTE_Init -------------
DivTable = [0] * 0x101
for divisor in range(0x8000, 0x10000, 0x80):
    xa = 512
    for _ in range(1, 5):
        xa = (xa * (1024 * 512 - ((divisor >> 7) * xa))) >> 18
    DivTable[(divisor >> 7) & 0xFF] = ((xa + 1) >> 1) - 0x101
DivTable[0x100] = DivTable[0xFF]


def calc_recip(divisor):  # divisor: uint16 with top bit set
    x = 0x101 + DivTable[((divisor & 0x7FFF) + 0x40) >> 7]
    tmp = (((divisor * -x) + 0x80) >> 8)
    tmp2 = ((x * (131072 + tmp)) + 0x80) >> 8
    return tmp2


def gte_divide(H, Z):
    if Z * 2 > H:
        # count leading zeros of the 16-bit divisor
        d = Z & 0xFFFF
        shift = 0
        if d:
            shift = 16 - d.bit_length()
        else:
            shift = 16
        dividend = (H << shift) & 0xFFFFFFFF
        divisor = (Z << shift) & 0xFFFFFFFF
        r = ((dividend * calc_recip(divisor | 0x8000) + 32768) >> 16)
        return min(r, 0x1FFFF)
    return 0x1FFFF


def sat(v, lo, hi):
    return lo if v < lo else hi if v > hi else v


def project(row):
    r = list(map(int, row))
    (r11, r12, r13, r21, r22, r23, r31, r32, r33,
     trx, tr_y, trz, ofx, ofy, H, sf, vx, vy, vz) = r[1:20]
    # MAC = (TR<<12 + R*V) >> sf  (sf is 0 or 12)
    mac1 = ((trx << 12) + r11 * vx + r12 * vy + r13 * vz) >> sf
    mac2 = ((tr_y << 12) + r21 * vx + r22 * vy + r23 * vz) >> sf
    mac3 = ((trz << 12) + r31 * vx + r32 * vy + r33 * vz) >> sf
    ir1 = sat(mac1, -32768, 32767)
    ir2 = sat(mac2, -32768, 32767)
    sz3 = sat(mac3, 0, 65535)
    h_div_sz = gte_divide(H, sz3)
    sx = sat((ofx + ir1 * h_div_sz) >> 16, -1024, 1023)
    sy = sat((ofy + ir2 * h_div_sz) >> 16, -1024, 1023)
    return sx, sy


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    path = sys.argv[1]
    cap = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    n = ok = 0
    fails = []
    with open(path) as f:
        next(f)  # header
        for line in f:
            cols = line.rstrip("\n").split(",")
            if len(cols) != 22:
                continue
            want = (int(cols[20]), int(cols[21]))
            got = project(cols)
            n += 1
            if got == want:
                ok += 1
            elif len(fails) < 12:
                fails.append((cols[0], got, want, cols[16:20]))
            if cap and n >= cap:
                break
    print(f"rows: {n}  match: {ok}  ({100.0*ok/n:.3f}%)" if n else "no rows")
    for fr, got, want, v in fails:
        print(f"  MISMATCH f{fr} got={got} want={want} V={v}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
