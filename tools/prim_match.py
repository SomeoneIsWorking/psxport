#!/usr/bin/env python3
"""wide60 matcher prototype: how well do primitives pair up between consecutive
presented frames of a GPU dump?

Frames are delimited by display-buffer flips (GP1 0x05). Within each consecutive
frame pair, primitives are keyed by (command byte, texpage, clut, vertex count) and
the k-th primitive of a key in frame N is matched to the k-th in frame N+1 (stable
draw order assumption). Reports match rate and vertex displacement stats — the
numbers that decide whether generic display-list interpolation is viable.

Usage: prim_match.py <dump.psxgpu> [--per-frame]
"""

import statistics
import struct
import sys

MAGIC = b"PSXGPUDUMPv1\0\0"


def sext11(v: int) -> int:
    v &= 0x7FF
    return v - 0x800 if v & 0x400 else v


class Prim:
    __slots__ = ("key", "verts", "colors", "raw")

    def __init__(self, key, verts, colors, raw):
        self.key = key
        self.verts = verts      # [(x, y), ...]
        self.colors = colors    # [rgb24, ...]
        self.raw = raw


def decode_prim(payload: bytes):
    """Decode one GP0 draw command packet into a Prim, or None if not a draw."""
    words = struct.unpack_from(f"<{len(payload)//4}I", payload)
    cmd = words[0]
    op = cmd >> 29
    cmd_byte = cmd >> 24

    if op == 1:  # polygon
        gouraud = bool(cmd & (1 << 28))
        quad = bool(cmd & (1 << 27))
        textured = bool(cmd & (1 << 26))
        nverts = 4 if quad else 3
        verts, colors, clut, texpage = [], [], 0, 0
        i = 0
        for v in range(nverts):
            color = words[i] & 0xFFFFFF if (v == 0 or gouraud) else colors[-1]
            if v == 0 or gouraud:
                i += 1
            xy = words[i]; i += 1
            verts.append((sext11(xy), sext11(xy >> 16)))
            colors.append(color)
            if textured:
                uv = words[i]; i += 1
                if v == 0:
                    clut = (uv >> 16) & 0xFFFF
                elif v == 1:
                    texpage = (uv >> 16) & 0xFFFF
        return Prim((cmd_byte, texpage, clut, nverts), verts, colors, payload)

    if op == 3:  # rectangle
        textured = bool(cmd & (1 << 26))
        i = 1
        xy = words[i]; i += 1
        verts = [(sext11(xy), sext11(xy >> 16))]
        clut = 0
        if textured:
            clut = (words[i] >> 16) & 0xFFFF
            i += 1
        return Prim((cmd_byte, 0, clut, 1), verts, [cmd & 0xFFFFFF], payload)

    if op == 2:  # line
        gouraud = bool(cmd & (1 << 28))
        poly = bool(cmd & (1 << 27))
        if poly:  # polyline, terminator-delimited; treat as one prim keyed by length
            return Prim((cmd_byte, 0, 0, len(words)), [], [cmd & 0xFFFFFF], payload)
        verts = []
        if gouraud:
            verts = [(sext11(words[1]), sext11(words[1] >> 16)),
                     (sext11(words[3]), sext11(words[3] >> 16))]
        else:
            verts = [(sext11(words[1]), sext11(words[1] >> 16)),
                     (sext11(words[2]), sext11(words[2] >> 16))]
        return Prim((cmd_byte, 0, 0, 2), verts, [cmd & 0xFFFFFF], payload)

    return None


def parse_frames(path: str):
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(MAGIC):
        sys.exit("bad magic")
    pos = len(MAGIC)
    in_trace = False
    last_display_start = None
    frames = []
    cur = []
    while pos + 4 <= len(data):
        (hdr,) = struct.unpack_from("<I", data, pos)
        pos += 4
        lw = hdr & 0xFFFFFF
        ptype = hdr >> 24
        payload = data[pos : pos + lw * 4]
        pos += lw * 4
        if ptype == 0x05:
            in_trace = True
        elif not in_trace:
            continue
        elif ptype == 0x00 and lw > 0:
            p = decode_prim(payload)
            if p:
                cur.append(p)
        elif ptype == 0x01 and lw >= 1:
            (w,) = struct.unpack_from("<I", payload, 0)
            if (w >> 24) == 0x05:
                start = w & 0x7FFFF
                if start != last_display_start:
                    if last_display_start is not None:
                        frames.append(cur)
                    cur = []
                    last_display_start = start
    if cur:
        frames.append(cur)
    return frames


def match_pair(a, b):
    """Match primitives of frame a to frame b by (key, occurrence index)."""
    from collections import defaultdict
    bidx = defaultdict(list)
    for p in b:
        bidx[p.key].append(p)
    seen = defaultdict(int)
    matches = []
    unmatched = 0
    for p in a:
        lst = bidx.get(p.key)
        k = seen[p.key]
        if lst and k < len(lst):
            matches.append((p, lst[k]))
            seen[p.key] += 1
        else:
            unmatched += 1
    return matches, unmatched


def main():
    path = sys.argv[1]
    per_frame = "--per-frame" in sys.argv
    frames = parse_frames(path)
    print(f"{len(frames)} presented frames, "
          f"{sum(len(f) for f in frames)/max(len(frames),1):.0f} prims/frame avg")

    total, matched, identical = 0, 0, 0
    displacements = []
    for i in range(len(frames) - 1):
        a, b = frames[i], frames[i + 1]
        if not a or not b:
            continue
        ms, _ = match_pair(a, b)
        total += len(a)
        matched += len(ms)
        disp_frame = []
        for pa, pb in ms:
            if pa.raw == pb.raw:
                identical += 1
            if pa.verts and len(pa.verts) == len(pb.verts):
                d = max(abs(xa - xb) + abs(ya - yb)
                        for (xa, ya), (xb, yb) in zip(pa.verts, pb.verts))
                displacements.append(d)
                disp_frame.append(d)
        if per_frame:
            print(f"  frame {i:3d}->{i+1:3d}: {len(ms)}/{len(a)} matched, "
                  f"median disp {statistics.median(disp_frame) if disp_frame else '-'}")

    print(f"match rate: {matched}/{total} = {100*matched/max(total,1):.1f}%")
    print(f"  identical (no motion): {100*identical/max(matched,1):.1f}% of matches")
    if displacements:
        moved = [d for d in displacements if d > 0]
        print(f"  vertex displacement (L1, px): median {statistics.median(displacements)}, "
              f"p90 {statistics.quantiles(displacements, n=10)[8]}, max {max(displacements)}")
        if moved:
            print(f"  moving prims only: median {statistics.median(moved)}, "
                  f"p90 {statistics.quantiles(moved, n=10)[8] if len(moved) >= 10 else max(moved)}, "
                  f"n={len(moved)}")
        big = sum(1 for d in displacements if d > 64)
        print(f"  jumps >64px (should snap, not lerp): {100*big/len(displacements):.2f}%")


if __name__ == "__main__":
    main()
