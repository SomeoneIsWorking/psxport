"""SPU state at an aligned guest state, read from Beetle's own `SPU` state section (docs/oracle.md
"Audio comparison").

Both cores run Beetle's spu.c, and `SPU_StateAction` is the one description of its state: the product
writes that section through the REPL `spu` verb (runtime/psx/state/beetle_device_state.cpp) and the
console reference cuts it out of `retro_serialize`. This module decodes the section by entry name and
compares the two per voice and per register. A field is decisive when it is guest-visible state the
two guests must agree on at the same game frame; sample-clock position (envelope level, decode
address, sweep progress) is informational, because loading removal means the cores' SPU clocks never
line up sample for sample, and so are the parameters of a voice silent on both cores and the reverb
configuration while both cores' reverb output volume is zero.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass

STATE_MAGIC = b"MDFNSVST"
STATE_HEADER = 32
SECTION_NAME = 32
VOICES = 24
ADSR_RELEASE = 3
REVERB_REGISTERS = 0xE0  # regs.Regs index of 0x1F801DC0, the reverb configuration block
SPUCNT_TRANSFER_MODE = 0x0030  # DMA/manual transfer mode bits change mid-upload; not mix state
KEYED, SOUNDING = 0, 1  # positions of a voice's key and envelope fields in SpuState.voices
REVERB_OUTPUT = ("reverb.volume.left", "reverb.volume.right")
REVERB_CONFIGURATION = tuple(f"reverb.{0x1C0 + 2 * i:03X}" for i in range(0x20))


def sections(state: bytes) -> dict[str, bytes]:
    """Every section of a Mednafen state chunk: a whole `MDFNSVST` state or bare sections."""
    offset = STATE_HEADER if state.startswith(STATE_MAGIC) else 0
    found: dict[str, bytes] = {}
    while offset + SECTION_NAME + 4 <= len(state):
        name = state[offset:offset + SECTION_NAME].split(b"\0", 1)[0].decode("ascii")
        (size,) = struct.unpack_from("<I", state, offset + SECTION_NAME)
        start = offset + SECTION_NAME + 4
        if start + size > len(state):
            raise ValueError(f"state section {name!r} runs {start + size - len(state)} bytes past the end")
        found[name] = state[start:start + size]
        offset = start + size
    return found


def entries(section: bytes) -> dict[str, bytes]:
    """A section's named entries: u8 name length, name, u32 size, data."""
    found: dict[str, bytes] = {}
    offset = 0
    while offset < len(section):
        length = section[offset]
        name = section[offset + 1:offset + 1 + length].decode("ascii")
        (size,) = struct.unpack_from("<I", section, offset + 1 + length)
        start = offset + 5 + length
        found[name] = section[start:start + size]
        offset = start + size
    return found


def _unsigned(data: bytes) -> int:
    return int.from_bytes(data, "little")


def _words(data: bytes, width: int) -> list[int]:
    return [int.from_bytes(data[i:i + width], "little") for i in range(0, len(data), width)]


@dataclass(frozen=True)
class Field:
    name: str
    value: int
    decisive: bool


@dataclass(frozen=True)
class SpuState:
    voices: tuple[tuple[Field, ...], ...]
    registers: tuple[Field, ...]

    @classmethod
    def from_state(cls, state: bytes) -> "SpuState":
        found = sections(state)
        if "SPU" not in found:
            raise ValueError(f"no SPU section in the state (sections {sorted(found)})")
        values = entries(found["SPU"])

        def get(name: str) -> int:
            if name not in values:
                raise ValueError(f"SPU section has no entry {name!r}; Beetle's SPU_StateAction changed")
            return _unsigned(values[name])

        # Entry names are spu.c's own spellings; its sweep macros expand the per-instance accessor.
        def sweep(n: int, side: int, field: str) -> int:
            return get(f"((spu_cur->Voices)[{n}].Sweep[{side}]).{field}")

        mode = {key: get(key) for key in ("Reverb_Mode", "FM_Mode", "Noise_Mode")}
        voices = []
        for n in range(VOICES):
            def v(field: str) -> int:
                return get(f"Voices[{n}].{field}")
            phase = v("ADSR.Phase")
            voices.append((
                Field("keyed", int(phase != ADSR_RELEASE), True),
                Field("sounding", int(v("ADSR.EnvLevel") != 0), True),
                Field("pitch", v("Pitch"), True),
                Field("start", v("StartAddr"), True),
                Field("adsr", v("ADSRControl"), True),
                Field("volume.left", sweep(n, 0, "Control"), True),
                Field("volume.right", sweep(n, 1, "Control"), True),
                Field("reverb", (mode["Reverb_Mode"] >> n) & 1, True),
                Field("fm", (mode["FM_Mode"] >> n) & 1, True),
                Field("noise", (mode["Noise_Mode"] >> n) & 1, True),
                Field("adsr.phase", phase, False),
                Field("envelope", v("ADSR.EnvLevel"), False),
                Field("volume.left.current", sweep(n, 0, "Current"), False),
                Field("volume.right.current", sweep(n, 1, "Current"), False),
                Field("address", v("CurAddr"), False),
                Field("loop", v("LoopAddr"), False),
            ))
        reverb_volume = _words(values["ReverbVol"], 4)
        cd_volume = _words(values["CDVol"], 4)
        extern_volume = _words(values["ExternVol"], 4)
        registers = [
            Field("spucnt", get("SPUControl") & ~SPUCNT_TRANSFER_MODE, True),
            Field("main.volume.left", get(f"((spu_cur->GlobalSweep)[0]).Control"), True),
            Field("main.volume.right", get(f"((spu_cur->GlobalSweep)[1]).Control"), True),
            Field("reverb.volume.left", reverb_volume[0], True),
            Field("reverb.volume.right", reverb_volume[1], True),
            Field("reverb.base", get("ReverbWA"), True),
            Field("cd.volume.left", cd_volume[0], True),
            Field("cd.volume.right", cd_volume[1], True),
            Field("extern.volume.left", extern_volume[0], True),
            Field("extern.volume.right", extern_volume[1], True),
        ]
        reverb = _words(values["regs.Regs"], 2)[REVERB_REGISTERS:REVERB_REGISTERS + 0x20]
        registers += [Field(name, word, True) for name, word in zip(REVERB_CONFIGURATION, reverb)]
        registers += [Field("irq.address", get("IRQAddr"), False),
                      Field("main.volume.left.current", get(f"((spu_cur->GlobalSweep)[0]).Current"), False),
                      Field("main.volume.right.current", get(f"((spu_cur->GlobalSweep)[1]).Current"), False)]
        return cls(tuple(tuple(fields) for fields in voices), tuple(registers))

    def sounding(self) -> list[int]:
        return [n for n, fields in enumerate(self.voices) if fields[1].value]

    def reverb_heard(self) -> bool:
        return any(field.value for field in self.registers if field.name in REVERB_OUTPUT)


@dataclass(frozen=True)
class Output:
    """Each core's output over the last field it ran: everything the player hears (the console's libretro
    output; the product's SPU mix with the title's own music mixed in)."""
    samples: int
    nonzero: int
    peak: int

    def row(self) -> dict:
        return {"samples": self.samples, "nonzero": self.nonzero, "peak": self.peak}


def compare(native: SpuState, console: SpuState,
            native_output: Output | None = None, console_output: Output | None = None) -> dict:
    """Per-field rows for every difference, decisive first in voice then register order, and the
    first decisive divergence."""
    rows: list[dict] = []
    for n, (ours, theirs) in enumerate(zip(native.voices, console.voices)):
        # A voice silent on both cores is heard by nobody: its pitch, address, ADSR and volumes are what its
        # last note left, and a note played during a load the product removed leaves different ones.
        heard = ours[SOUNDING].value or theirs[SOUNDING].value
        for index, (a, b) in enumerate(zip(ours, theirs)):
            if a.value != b.value:
                decisive = a.decisive and (heard or index in (KEYED, SOUNDING))
                rows.append({"where": f"voice {n}", "field": a.name, "decisive": decisive,
                             "native": a.value, "console": b.value})
    # The reverb configuration is heard only through the reverb output volume; a console boot leaves the BIOS
    # shell's configuration behind, which a title that never turns reverb on keeps silently.
    reverb_heard = native.reverb_heard() or console.reverb_heard()
    for a, b in zip(native.registers, console.registers):
        if a.value != b.value:
            decisive = a.decisive and (reverb_heard or a.name not in REVERB_CONFIGURATION)
            rows.append({"where": "spu", "field": a.name, "decisive": decisive,
                         "native": a.value, "console": b.value})
    if native_output is not None and console_output is not None:
        if bool(native_output.peak) != bool(console_output.peak):
            rows.append({"where": "output", "field": "audible", "decisive": True,
                         "native": int(bool(native_output.peak)), "console": int(bool(console_output.peak))})
    decisive = [row for row in rows if row["decisive"]]
    return {"match": not decisive, "first": decisive[0] if decisive else None,
            "differences": rows,
            "sounding": {"native": native.sounding(), "console": console.sounding()},
            "output": None if native_output is None or console_output is None else
            {"native": native_output.row(), "console": console_output.row()}}


def describe(row: dict) -> str:
    return (f"{row['where']} {row['field']}: native 0x{row['native']:X} console 0x{row['console']:X}"
            f"{'' if row['decisive'] else ' (informational)'}")
