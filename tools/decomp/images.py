"""Which bytes a guest address names, per title, as DATA the tool reads.

ONE CONCEPT: the mapping between a guest address and a file offset, and the load base Ghidra is
pointed at. Nothing here knows any title's numbers; the numbers live in ``manifest.json`` beside
this file and are read through :func:`load_manifest`. That is the difference between a capability
the per-title agents share and a fifth private copy: adding a title is a data edit, and a wrong
number is a data error the manifest's own cross-check names.

THE ARITHMETIC, once, because it is easy to get wrong in a way that does not fail loudly::

    file_offset(address) = text_file_offset + (address - text_load_address)
    ghidra_base           = text_load_address - text_file_offset

Ghidra's Raw Binary loader maps FILE OFFSET 0 at the base, so the base is chosen to put the
executable's own header below the text window instead of shifting every guest address. For Spyro 1
that is ``0x80010000 - 0x800 = 0x8000F800``, and it is the formula that repo verified against
62,183 of 62,183 recorded instructions. Getting it wrong produces a project full of plausible
functions at addresses 0x800 lower than the ones every document names, so it is asserted here
against the image's own PS-X EXE header rather than trusted.
"""

from __future__ import annotations

import json
import struct
from dataclasses import dataclass
from pathlib import Path

MANIFEST = Path(__file__).resolve().parent / "manifest.json"

# The PS-X EXE header is fixed-size and its text always begins 0x800 bytes into the file.
PSX_EXE_MAGIC = b"PS-X EXE"
PSX_EXE_TEXT_FILE_OFFSET = 0x800
PSX_EXE_HEADER_SIZE = 0x800


class ImageRefusal(Exception):
    """The image is absent, is not a PS-X EXE, or disagrees with the manifest.

    Raised, never returned as a short read. A refusal that surfaced as an empty inventory would be
    indistinguishable from an image whose functions are all bodyless, which is the failure this
    pipeline exists to make visible.
    """


@dataclass(frozen=True)
class ImageSpec:
    """One title's load geometry, read from the manifest."""

    title: str
    serial: str
    text_load_address: int
    text_file_offset: int
    text_size: int | None = None
    note: str = ""

    @property
    def ghidra_base(self) -> int:
        """Where Ghidra's Raw Binary loader must be pointed for this title's addresses to line up."""
        return self.text_load_address - self.text_file_offset

    @property
    def text_end(self) -> int:
        return self.text_load_address + (self.text_size or 0)

    def file_offset(self, address: int) -> int:
        return self.text_file_offset + (address - self.text_load_address)

    def covers(self, address: int, length: int = 4) -> bool:
        return address >= self.text_load_address and address + length <= self.text_end

    def as_dict(self) -> dict:
        return {
            "title": self.title,
            "serial": self.serial,
            "text_load_address": "0x%08X" % self.text_load_address,
            "text_file_offset": "0x%X" % self.text_file_offset,
            "ghidra_base": "0x%08X" % self.ghidra_base,
            "text_size": self.text_size,
            "note": self.note,
        }


@dataclass(frozen=True)
class PsxExeHeader:
    entry: int
    gp: int
    load: int
    text_size: int
    sp_base: int
    sp_offset: int
    file_size: int


def read_psx_exe_header(path: Path) -> PsxExeHeader:
    """The PS-X EXE header, or REFUSE. A raw RAM dump has no header and is a different thing."""
    if not path.is_file():
        raise ImageRefusal(
            f"no image at {path}. This tool needs the admitted PS-X EXE (provision it first); "
            "refusing rather than reporting an empty inventory, because 'the image is absent' and "
            "'the image analyses to nothing' would otherwise print the same line."
        )
    with path.open("rb") as handle:
        head = handle.read(PSX_EXE_HEADER_SIZE)
        size = path.stat().st_size
    if len(head) < 0x40:
        raise ImageRefusal(f"{path} is {len(head)} bytes, too short to hold a PS-X EXE header")
    if head[:8] != PSX_EXE_MAGIC:
        raise ImageRefusal(
            f"{path} starts {head[:8]!r}, not {PSX_EXE_MAGIC!r}. A raw RAM dump is not a PS-X EXE; "
            "point --image at the extracted executable, or at an overlay with its own manifest entry."
        )
    entry, gp = struct.unpack_from("<II", head, 0x10)
    load, text_size = struct.unpack_from("<II", head, 0x18)
    sp_base, sp_offset = struct.unpack_from("<II", head, 0x30)
    return PsxExeHeader(entry, gp, load, text_size, sp_base, sp_offset, size)


def cross_check(spec: ImageSpec, header: PsxExeHeader, path: Path) -> None:
    """Refuse when the manifest disagrees with the image's own header.

    This is the assertion that catches a wrong base address, and it is the seeded difference the
    selftest uses. A wrong base does not fail on its own: Ghidra imports happily and every function
    lands at a plausible address, just not the one the title's documents name.
    """
    if spec.text_load_address != header.load:
        raise ImageRefusal(
            f"{spec.title} ({spec.serial}) manifest says text loads at 0x{spec.text_load_address:08X} "
            f"but {path} says 0x{header.load:08X}. Every guest address the pipeline reports would be "
            "wrong. Fix the manifest entry, not the base."
        )
    if spec.text_file_offset != PSX_EXE_TEXT_FILE_OFFSET:
        raise ImageRefusal(
            f"{spec.title} manifest says text starts at file offset 0x{spec.text_file_offset:X}, but "
            f"a PS-X EXE always starts it at 0x{PSX_EXE_TEXT_FILE_OFFSET:X}. Overlays may differ; a "
            "main executable may not."
        )
    if spec.text_size is not None and spec.text_size != header.text_size:
        raise ImageRefusal(
            f"{spec.title} manifest says text size 0x{spec.text_size:X} but {path} says "
            f"0x{header.text_size:X}. The window would cover the wrong bytes."
        )
    if PSX_EXE_TEXT_FILE_OFFSET + header.text_size > header.file_size:
        raise ImageRefusal(
            f"{path} holds {header.file_size} bytes but its header declares {header.text_size} bytes "
            f"of text at offset 0x{PSX_EXE_TEXT_FILE_OFFSET:X}, which runs past the end of the file. "
            "The image is truncated."
        )


def load_manifest(path: Path | None = None) -> dict[str, ImageSpec]:
    """Read the per-title manifest. Refuses an absent, malformed, or EMPTY manifest.

    An empty manifest is refused because every title lookup against it would answer "not found" and
    read as "this tool has no data for your title" rather than "the data file is empty" — the same
    zero-means-nothing defect the rest of this pipeline exists to prevent.
    """
    manifest_path = Path(path) if path is not None else MANIFEST
    if not manifest_path.is_file():
        raise ImageRefusal(
            f"no image manifest at {manifest_path}. The per-title load geometry lives there; it is "
            "data, not code, so adding a title is a JSON edit."
        )
    try:
        raw = json.loads(manifest_path.read_text())
    except json.JSONDecodeError as error:
        raise ImageRefusal(f"{manifest_path} is not valid JSON: {error}") from error
    titles = raw.get("titles") if isinstance(raw, dict) else None
    if not isinstance(titles, dict) or not titles:
        raise ImageRefusal(
            f"{manifest_path} has no non-empty 'titles' object, so no title can be resolved. "
            "Refusing rather than reporting every lookup as unknown."
        )
    specs: dict[str, ImageSpec] = {}
    for title, entry in titles.items():
        specs[title] = ImageSpec(
            title=title,
            serial=entry["serial"],
            text_load_address=int(entry["text_load_address"], 0),
            text_file_offset=int(entry["text_file_offset"], 0),
            text_size=None if entry.get("text_size") in (None, "") else int(entry["text_size"], 0),
            note=entry.get("note", ""),
        )
    return specs


def select(manifest: dict[str, ImageSpec], title: str | None) -> ImageSpec:
    """One spec by title, or a refusal that NAMES the titles that exist.

    The name list is the denominator: "unknown title" and "this tool knows five titles" are
    different facts, and a reader who gets the first has no way to learn the second.
    """
    if title:
        if title in manifest:
            return manifest[title]
        raise ImageRefusal(
            f"no manifest entry titled {title!r}. Known titles: "
            + ", ".join(sorted(manifest)) + f" ({len(manifest)} known). Add the title to "
            "tools/decomp/manifest.json; the load geometry is data, not a code change."
        )
    if len(manifest) == 1:
        return next(iter(manifest.values()))
    raise ImageRefusal(
        f"--title is required: the manifest knows {len(manifest)} titles "
        f"({', '.join(sorted(manifest))}), so there is no single default to guess."
    )


class ImageWindow:
    """The admitted image, read whole, with its guest<->file mapping.

    One instance per run. ``data`` is the only payload; every address question is asked of this
    object so there is exactly one offset formula in the process.
    """

    def __init__(self, spec: ImageSpec, path: Path):
        self.spec = spec
        self.path = Path(path)
        self.header = read_psx_exe_header(self.path)
        cross_check(spec, self.header, self.path)
        self.data = self.path.read_bytes()
        if self.header.entry == 0:
            raise ImageRefusal(
                f"{self.path} declares entry point 0. A PS-X EXE with no entry cannot seed "
                "disassembly, so every function would be unreachable and the inventory would read as "
                "an empty image rather than a malformed one."
            )

    @property
    def text_size(self) -> int:
        return self.header.text_size

    @property
    def text_end(self) -> int:
        return self.header.load + self.header.text_size

    def covers(self, address: int, length: int = 4) -> bool:
        return (self.header.load <= address
                and address + length <= self.header.load + self.header.text_size
                and self.file_offset(address) + length <= len(self.data))

    def file_offset(self, address: int) -> int:
        return self.spec.file_offset(address)

    def word(self, address: int) -> int:
        """The 32-bit instruction word, LITTLE-endian, which is how a PSX executable stores them.

        The data half of the image is big-endian, so the wrong half of this choice decodes into
        valid-looking garbage instead of failing.
        """
        if not self.covers(address, 4):
            raise ImageRefusal(
                f"0x{address:08X} is outside the text window "
                f"0x{self.header.load:08X}..0x{self.text_end:08X}")
        return struct.unpack_from("<I", self.data, self.file_offset(address))[0]
