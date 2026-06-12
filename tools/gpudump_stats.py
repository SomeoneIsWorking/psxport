#!/usr/bin/env python3
"""Analyze a DuckStation GPU dump (.psxgpu[.zst]).

Measures the game's presented framerate two ways:
1. Display-buffer flips: GP1(0x05) display-start changes per vsync. This is ground
   truth — a 30fps game flips every 2 vsyncs. (Draw-command bucketing by vsync is
   misleading: submission spans vblanks, so draws for one logic frame straddle two
   vsync buckets.)
2. Per-vsync draw counts + display-list hashes, for inspecting submission patterns.
This is the offline prototype of the wide60 logic-rate detector.
"""

import hashlib
import struct
import sys
from collections import Counter

MAGIC = b"PSXGPUDUMPv1\0\0"

PACKET_NAMES = {
    0x00: "GP0", 0x01: "GP1", 0x02: "VSYNC", 0x03: "DISCARD0",
    0x04: "READBACK0", 0x05: "TRACEBEGIN", 0x06: "GPUVERSION",
    0x10: "GAMEID", 0x11: "VIDEOFORMAT", 0x12: "COMMENT",
}


def load(path: str) -> bytes:
    with open(path, "rb") as f:
        data = f.read()
    if path.endswith(".zst") or data[:4] == b"\x28\xb5\x2f\xfd":
        import zstandard
        data = zstandard.ZstdDecompressor().decompressobj().decompress(data)
    if not data.startswith(MAGIC):
        sys.exit(f"bad magic: {data[:14]!r}")
    return data


def is_draw_command(word: int) -> bool:
    op = word >> 29  # top 3 bits: 1=poly, 2=line, 3=rect
    return op in (1, 2, 3)


def main() -> None:
    data = load(sys.argv[1])
    pos = len(MAGIC)
    in_trace = False
    frames = []  # list of (draw_count, sha1-of-draw-words)
    draws = 0
    hasher = hashlib.sha1()

    vsync = 0
    flips = []  # vsync index of each display-start change
    last_display_start = None

    while pos + 4 <= len(data):
        (hdr,) = struct.unpack_from("<I", data, pos)
        pos += 4
        length_words = hdr & 0xFFFFFF
        ptype = hdr >> 24
        payload = data[pos : pos + length_words * 4]
        pos += length_words * 4

        if ptype == 0x05:
            in_trace = True
        elif not in_trace:
            continue
        elif ptype == 0x00 and length_words > 0:
            (cmd,) = struct.unpack_from("<I", payload, 0)
            if is_draw_command(cmd):
                draws += 1
                hasher.update(payload)
        elif ptype == 0x01 and length_words >= 1:
            (word,) = struct.unpack_from("<I", payload, 0)
            if (word >> 24) == 0x05:  # GP1 set display start
                start = word & 0x7FFFF
                if start != last_display_start:
                    flips.append(vsync)
                    last_display_start = start
        elif ptype == 0x02:
            frames.append((draws, hasher.hexdigest()))
            draws = 0
            hasher = hashlib.sha1()
            vsync += 1

    gaps = Counter(b - a for a, b in zip(flips, flips[1:]))
    print(f"display flips: {len(flips)} over {vsync} vsyncs; "
          f"vsyncs-between-flips histogram: {dict(sorted(gaps.items()))}")
    if gaps:
        dom_gap = gaps.most_common(1)[0][0]
        print(f"=> presented framerate: 60/{dom_gap} = {60 // dom_gap} fps")

    print(f"frames: {len(frames)}")
    counts = [c for c, _ in frames]
    print(f"draw commands/frame: min {min(counts)} max {max(counts)} "
          f"avg {sum(counts)/len(counts):.1f}")

    runs = Counter()  # run length of identical consecutive display lists
    run = 1
    for i in range(1, len(frames)):
        if frames[i][1] == frames[i - 1][1]:
            run += 1
        else:
            runs[run] += 1
            run = 1
    runs[run] += 1
    print(f"identical-display-list run lengths: {dict(sorted(runs.items()))}")
    dominant = runs.most_common(1)[0][0]
    print(f"=> logic rate looks like 60/{dominant} = {60 // dominant} fps "
          f"(dominant run length {dominant})")


if __name__ == "__main__":
    main()
