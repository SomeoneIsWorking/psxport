#!/usr/bin/env python3
"""psx_pad — THE PSX digital-pad bit table, and the .pad replay format, for every tool here.

One source of truth, because there were two and they were not the same shape. `compare_cores.py`
held the bit table for driving cores; Tomba! 2's `tools/pad_decode.py` held its own copy for reading
recorded routes, and an incomplete copy of a bit table makes a rebuilt route drop input silently —
which is exactly how that file's own comment records it being found (mask 0xFEFF, L2 held, that it
could not name).

A frame's mask is one little-endian uint16, ACTIVE-LOW: a pressed button CLEARS its bit, so a neutral
frame is 0xFFFF. The runtime writes them (`runtime/psx/input/pad_recording.cpp`) and only ever consumes a
PREFIX from boot — `pad_recording.h`: "Only a PREFIX is ever offered: a recording's first phase is
the one the game boots into, and a suffix would start elsewhere."

THERE ARE TWO FILE FORMATS, AND THIS MODULE REFUSES THE OLD ONE.

    v1 (current)  phase-keyed. A versioned header (magic PSXPADPH, u32 version, u32 card kind,
                  32-byte sha256), then 'P' u64 phase and 'R' u16 mask u32 frames records. Each
                  frame sits at an offset from the entry of the phase it was recorded in, so boot,
                  load and CD timing absorb at phase boundaries instead of shifting every press.
    pre-v1        one u16 LE mask per frame counted from boot, with no phase and no card identity.

`masks` and `schedule` read ABSOLUTE frame sequences, so they serve the pre-v1 shape and REFUSE a v1
file by name rather than flattening a phase-keyed recording to frames-whose-counter-happened-to-match.
`migrate` converts a pre-v1 file to v1 with a single explicitly UNKEYED segment and an UNKNOWN card,
which preserves its from-boot meaning and states it in the file. Reading a recorded route's buttons
to drive something else is a different thing and is what `schedule` is for.
"""

from __future__ import annotations

import argparse
import struct
import sys
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


# ---- the v1 phase-keyed container ------------------------------------------------------------
# Mirrors runtime/psx/input/pad_recording.h. Kept as named constants so a change on either side that
# forgets the other shows up as a refusal here rather than as a file that decodes to plausible junk.
MAGIC = b"PSXPADPH"
VERSION = 1
HEADER_BYTES = 8 + 4 + 4 + 32
CARD_KINDS = {0: "unknown", 1: "no card image", 2: "sha256"}
PHASE_TAG = b"P"
RUN_TAG = b"R"
PHASE_RECORD_BYTES = 1 + 8
RUN_RECORD_BYTES = 1 + 2 + 4
RUN_FRAMES_OFFSET = 1 + 2
# The runtime's reserved "no phase" (input_phase.h). A migrated file's single segment carries it, so
# it replays absolutely from boot and says so rather than pretending to be keyed.
UNKEYED_PHASE = 0xFFFFFFFFFFFFFFFF


def is_phase_keyed(data: bytes) -> bool:
    """Whether these bytes carry the v1 magic.

    A FILE TOO SHORT TO HOLD THE MAGIC is not phase-keyed, not "malformed v1": a 12-byte pre-v1
    recording is a real pre-v1 recording, and calling it a broken v1 would send a reader looking for
    a v1 defect that is not there.
    """
    return data[:len(MAGIC)] == MAGIC


class Recording:
    """A decoded v1 recording: its card identity and its phase segments, in file order."""

    def __init__(self, card_kind: int, card_sha256: bytes,
                 segments: list[tuple[int, list[tuple[int, int]]]]) -> None:
        self.card_kind = card_kind
        self.card_sha256 = card_sha256
        self.segments = segments

    @property
    def card(self) -> str:
        name = CARD_KINDS.get(self.card_kind, f"kind {self.card_kind} (unknown to this build)")
        return f"{name} {self.card_sha256.hex()}" if self.card_kind == 2 else name

    @property
    def keyed(self) -> bool:
        """Whether any segment carries a real phase. A migrated recording answers False, and says so."""
        return any(phase != UNKEYED_PHASE for phase, _ in self.segments)

    @property
    def frames(self) -> int:
        return sum(frames for _, runs in self.segments for _, frames in runs)

    def summary(self) -> str:
        """One line with every denominator: segments, frames, and the presses inside them."""
        pressed = sum(frames for _, runs in self.segments for mask, frames in runs if mask != NEUTRAL)
        phases = " ".join(f"{phase:#x}:{sum(f for _, f in runs)}" for phase, runs in self.segments)
        return (f"{len(self.segments)} segment(s), {self.frames} frame(s), {pressed} holding a button, "
                f"card {self.card}, {'phase-keyed' if self.keyed else 'UNKEYED (absolute from boot)'}"
                f"; segments {phases}")


def _refuse(why: str) -> ValueError:
    return ValueError(why)


def decode(data: bytes, source: str = "<bytes>") -> Recording:
    """Decode v1 bytes, or raise naming the defect. Every refusal names what was compared."""
    if not is_phase_keyed(data):
        raise _refuse(
            f"{source} is not a phase-keyed recording: it does not start with the {MAGIC.decode()} "
            f"magic. A pre-v1 recording (one uint16 per frame from boot) carries no phase keys and no "
            f"card identity, so it is refused rather than read blind; convert it with "
            f"`psx_pad.py migrate {source} <out>` (one explicitly unkeyed, absolute segment)")
    if len(data) < HEADER_BYTES:
        raise _refuse(f"{source}: truncated header, {len(data)} of {HEADER_BYTES} bytes")
    (version,) = struct.unpack_from("<I", data, len(MAGIC))
    if version != VERSION:
        raise _refuse(f"{source}: pad recording format version {version} is not supported; "
                      f"this build reads version {VERSION}")
    (card_kind,) = struct.unpack_from("<I", data, len(MAGIC) + 4)
    if card_kind not in CARD_KINDS:
        raise _refuse(f"{source}: unknown card identity kind {card_kind} in the header")
    card_sha256 = data[len(MAGIC) + 8:HEADER_BYTES]

    segments: list[tuple[int, list[tuple[int, int]]]] = []
    at = HEADER_BYTES
    while at < len(data):
        tag = data[at:at + 1]
        if tag == PHASE_TAG:
            if len(data) - at < PHASE_RECORD_BYTES:
                raise _refuse(f"{source}: truncated phase record at byte {at}")
            if segments and not segments[-1][1]:
                raise _refuse(f"{source}: phase record at byte {at} follows a phase that holds no frames")
            (phase,) = struct.unpack_from("<Q", data, at + 1)
            segments.append((phase, []))
            at += PHASE_RECORD_BYTES
        elif tag == RUN_TAG:
            if len(data) - at < RUN_RECORD_BYTES:
                raise _refuse(f"{source}: truncated run record at byte {at}")
            if not segments:
                raise _refuse(f"{source}: run record at byte {at} precedes any phase record")
            mask, frames = struct.unpack_from("<HI", data, at + 1)
            if not frames:
                raise _refuse(f"{source}: zero-length run record at byte {at}")
            segments[-1][1].append((mask, frames))
            at += RUN_RECORD_BYTES
        else:
            raise _refuse(f"{source}: unknown record tag {data[at]:#04x} at byte {at}")
    if not segments or not segments[-1][1]:
        raise _refuse(f"{source}: the recording holds no frames (or ends on a phase with none)")
    return Recording(card_kind, card_sha256, segments)


def read(path: Path) -> Recording:
    """Decode a v1 recording from disk. A missing or empty file is refused by name."""
    data = Path(path).read_bytes()
    if not data:
        raise _refuse(f"{path} is empty; a replay with no frames is not a route")
    return decode(data, str(path))


def encode(card_kind: int, card_sha256: bytes,
           segments: list[tuple[int, list[tuple[int, int]]]]) -> bytes:
    """The v1 bytes for `segments`. The mirror of `decode`, and used by `migrate`."""
    if len(card_sha256) != 32:
        raise _refuse(f"a card image identity is 32 bytes of sha256, not {len(card_sha256)}")
    out = bytearray(MAGIC + struct.pack("<II", VERSION, card_kind) + card_sha256)
    for phase, runs in segments:
        out += PHASE_TAG + struct.pack("<Q", phase)
        for mask, frames in runs:
            if not frames:
                raise _refuse(f"a zero-length run cannot be written (phase {phase:#x})")
            out += RUN_TAG + struct.pack("<HI", mask, frames)
    return bytes(out)


def _absolute_masks(path: Path) -> list[int]:
    """The pre-v1 u16-per-frame masks, refusing a v1 file by name.

    The refusal matters: flattening a phase-keyed file's runs would produce a plausible frame list
    whose frame numbers mean nothing without the phase source that produced them, and the caller
    would replay it as absolute input — the exact defect the format change exists to end.
    """
    data = Path(path).read_bytes()
    if not data:
        raise _refuse(f"{path} is empty; a replay with no frames is not a route")
    if is_phase_keyed(data):
        raise _refuse(
            f"{path} is PHASE-KEYED (v1) and this reader wants an absolute frame sequence. Its frames "
            f"are offsets from each phase's entry, so numbering them from boot would answer a "
            f"different route; read it with `psx_pad.py info {path}`, or drive it through the "
            f"runtime's PSXPORT_PAD_REPLAY, which has the title's phase source")
    if len(data) % 2:
        raise _refuse(f"{path} has {len(data)} bytes, not a whole number of 2-byte frames")
    return [value for (value,) in struct.iter_unpack("<H", data)]


def masks(path: Path) -> list[int]:
    """Every ABSOLUTE frame's active-low word, in order. Refuses a phase-keyed file."""
    return _absolute_masks(path)


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


def migrate(source: Path, out: Path) -> Recording:
    """Convert a pre-v1 recording to v1: ONE UNKEYED segment, card UNKNOWN.

    The conversion is a statement about meaning, not a reformat. The source carried neither phase keys
    nor a card identity, so the only honest v1 file for it is a single unkeyed segment — which
    replays absolutely from boot, exactly as the source did — with an unknown card, which the runtime
    then WARNS about rather than pretending to know. A tool that wants real phase keying has to
    re-record: only the running product can say which phase each frame belonged to.
    """
    frames = _absolute_masks(source)
    runs: list[tuple[int, int]] = []
    for value in frames:
        if runs and runs[-1][0] == value:
            runs[-1] = (value, runs[-1][1] + 1)
        else:
            runs.append((value, 1))
    recording = Recording(0, bytes(32), [(UNKEYED_PHASE, runs)])
    Path(out).write_bytes(encode(0, bytes(32), recording.segments))
    return recording


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="psx_pad.py", description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)
    info = sub.add_parser("info", help="decode a phase-keyed recording and report its denominators")
    info.add_argument("path", type=Path)
    mig = sub.add_parser("migrate", help="convert a pre-v1 absolute recording to the v1 format")
    mig.add_argument("source", type=Path)
    mig.add_argument("out", type=Path)
    args = parser.parse_args(argv)
    if args.command == "info":
        recording = read(args.path)
        print(f"{args.path}: {recording.summary()}")
        for index, (phase, runs) in enumerate(recording.segments):
            name = "unkeyed" if phase == UNKEYED_PHASE else f"{phase:#x}"
            for mask, frames in runs:
                held = ", ".join(sorted(buttons_of(mask))) or "neutral"
                print(f"  segment {index + 1} phase {name}: {frames} frame(s) {held}")
        return 0
    recording = migrate(args.source, args.out)
    print(f"migrated {args.source} -> {args.out}: {recording.summary()}")
    print("  This file replays ABSOLUTELY from boot, as the source did. It has no phase keys, so a "
          "timing change still shifts every press; only re-recording under a title that declares an "
          "input phase produces a keyed file.")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ValueError, OSError) as failure:
        print(f"psx_pad: {failure}", file=sys.stderr)
        sys.exit(1)
