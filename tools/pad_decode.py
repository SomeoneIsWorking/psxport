#!/usr/bin/env python3
"""pad_decode.py — decode a PSXPORT pad-replay (.pad) into a human button timeline and/or an
SBS_KEYS string, and (inverse) build a .pad from a timeline. This is how you READ a recorded route
(replays/*.pad are 2-byte active-low PSX pad masks per frame) to learn its input, and how you
GENERATE a scripted route for headless SBS driving.

Why: the replays ARE working input sequences to reach a scene. Decoding one shows exactly what it
presses (e.g. hut-entry = boot taps, then walk RIGHT to the door, then hold UP to enter). Combined
with the movement calibration in docs/driving-the-game.md ("Driving by position feedback"), you can
author your own routes to any reachable target without live capture.

USAGE:
  tools/pad_decode.py <file.pad>              # print the button-press timeline (collapsed runs)
  tools/pad_decode.py <file.pad> --keys       # print an SBS_KEYS="FROM-TO:BTN,..." string
  tools/pad_decode.py --keys-from "220-254:right,255-435:up" --out route.pad --frames 500
                                              # build a .pad from an SBS_KEYS-style spec

Pad bit layout (active-low: a PRESSED button CLEARS its bit; neutral frame = 0xFFFF). LITTLE-ENDIAN
uint16 per frame (verified against the replay library — LE yields ~76% neutral frames, BE ~0%)."""
import sys, struct

BTN = {0x0010: "up", 0x0040: "down", 0x0080: "left", 0x0020: "right",
       0x4000: "cross", 0x2000: "circle", 0x8000: "square", 0x1000: "triangle",
       0x0008: "start", 0x0001: "select"}
NAME2BIT = {v: k for k, v in BTN.items()}


def decode(path):
    data = open(path, "rb").read()
    n = len(data) // 2
    return [struct.unpack_from("<H", data, i * 2)[0] for i in range(n)]


def timeline(masks):
    """Collapse frame masks into (from, to, buttons) runs, skipping neutral (0xFFFF) stretches."""
    runs, prev, start = [], None, 0
    for f, m in enumerate(list(masks) + [None]):
        if m != prev:
            if prev is not None and prev != 0xFFFF:
                btns = "+".join(b for bit, b in BTN.items() if not (prev & bit))
                runs.append((start, f - 1, btns or hex(prev)))
            start, prev = f, m
    return runs


def build_pad(spec, nframes):
    """spec = 'FROM-TO:BTN,...' -> bytes of nframes little-endian masks (0xFFFF neutral, bit cleared
    for each active button in its range). Multiple ranges may overlap (ANDs the bits)."""
    masks = [0xFFFF] * nframes
    for part in spec.split(","):
        part = part.strip()
        if not part:
            continue
        rng, name = part.split(":")
        a, b = rng.split("-")
        bit = NAME2BIT.get(name.strip())
        if bit is None:
            raise SystemExit(f"unknown button: {name}")
        for f in range(int(a), min(int(b), nframes - 1) + 1):
            masks[f] &= ~bit & 0xFFFF
    return b"".join(struct.pack("<H", m) for m in masks)


def main():
    a = sys.argv[1:]
    if "--keys-from" in a:
        spec = a[a.index("--keys-from") + 1]
        out = a[a.index("--out") + 1] if "--out" in a else "route.pad"
        nframes = int(a[a.index("--frames") + 1]) if "--frames" in a else 600
        open(out, "wb").write(build_pad(spec, nframes))
        print(f"wrote {out}: {nframes} frames from spec {spec!r}")
        return
    if not a or a[0].startswith("--"):
        print(__doc__)
        return
    path = a[0]
    runs = timeline(decode(path))
    if "--keys" in a:
        print(",".join(f"{f}-{t}:{b}" for f, t, b in runs if b in NAME2BIT))
    else:
        print(f"{path}: {len(decode(path))} frames")
        for f, t, b in runs:
            print(f"  f{f}-{t}: {b}")


if __name__ == "__main__":
    main()
