#!/usr/bin/env python3
"""psx_pad — THE PSX digital-pad bit table, and the .pad replay format, for every tool here.

One source of truth, because there were two and they were not the same shape. `compare_cores.py`
held the bit table for driving cores; Tomba! 2's `tools/pad_decode.py` held its own copy for reading
recorded routes, and an incomplete copy of a bit table makes a rebuilt route drop input silently —
which is exactly how that file's own comment records it being found (mask 0xFEFF, L2 held, that it
could not name).

A .pad replay is one little-endian uint16 per frame, ACTIVE-LOW: a pressed button CLEARS its bit, so
a neutral frame is 0xFFFF. The runtime writes them (`runtime/psx/pad_input.cpp`) and only ever
consumes them from frame 0 — `pad_input.h`: "A suffix is never offered: replays are only valid from
boot." That constraint is the RUNTIME's, about its own replay cursor. Reading a recorded route's
buttons to drive something else is a different thing and is what `schedule` below is for.
"""

from __future__ import annotations

import struct
from pathlib import Path

# The complete SCPH digital-pad word. Shoulder and stick bits included: an incomplete table cannot
# name a mask it meets, and silently dropping input is worse than refusing to read the file.
PSX_BUTTON_BITS = {
    "select": 0x0001, "l3": 0x0002, "r3": 0x0004, "start": 0x0008,
    "up": 0x0010, "right": 0x0020, "down": 0x0040, "left": 0x0080,
    "l2": 0x0100, "r2": 0x0200, "l1": 0x0400, "r1": 0x0800,
    "triangle": 0x1000, "circle": 0x2000, "cross": 0x4000, "square": 0x8000,
}
BITS_TO_NAME = {bit: name for name, bit in PSX_BUTTON_BITS.items()}
NEUTRAL = 0xFFFF


def mask_of(buttons: frozenset[str]) -> int:
    """The active-low word for a held set."""
    return NEUTRAL & ~sum(PSX_BUTTON_BITS[name] for name in buttons)


def buttons_of(mask: int) -> frozenset[str]:
    """The held set for an active-low word. Every cleared bit must be nameable: an unknown one means
    the table is incomplete or the file is not a .pad, and either way reading on would drop input."""
    held, unknown = set(), mask ^ NEUTRAL
    for bit, name in BITS_TO_NAME.items():
        if not mask & bit:
            held.add(name)
            unknown &= ~bit
    if unknown:
        raise ValueError(f"pad mask {mask:04X} clears unnamed bit(s) {unknown:04X}; "
                         f"the button table is incomplete or this is not a .pad")
    return frozenset(held)


def masks(path: Path) -> list[int]:
    """Every frame's active-low word, in order."""
    data = Path(path).read_bytes()
    if not data:
        raise ValueError(f"{path} is empty; a replay with no frames is not a route")
    if len(data) % 2:
        raise ValueError(f"{path} has {len(data)} bytes, not a whole number of 2-byte frames")
    return [value for (value,) in struct.iter_unpack("<H", data)]


def schedule(path: Path, start: int = 0) -> list[frozenset[str]]:
    """A recorded route as a per-frame held-button schedule, from frame `start`.

    This is for driving something that is NOT the runtime's replay cursor — an oracle feeding the
    same buttons to two cores, say. A suffix is meaningful there because both cores receive the same
    input from the same state; what the runtime forbids is resuming ITS cursor mid-file.

    The caller is responsible for the state the suffix starts from being the state the recording had
    at that frame. Nothing here can check that, and a suffix taken from the wrong place produces a
    route that does something else — identically on both cores, so a comparison stays valid while
    the route stops meaning what its name says.
    """
    frames = masks(path)
    if not 0 <= start < len(frames):
        raise ValueError(f"{path} has {len(frames)} frames; cannot start at frame {start}")
    return [buttons_of(value) for value in frames[start:]]
