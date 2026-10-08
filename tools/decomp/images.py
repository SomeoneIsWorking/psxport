"""Which bytes a guest address names, per title and per module, as DATA the tool reads.

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

TWO KINDS OF IMAGE, because a title is rarely one file:

  ``resident``  a PS-X EXE. It carries its OWN geometry in its header -- load address, text size and
                entry point -- so the manifest's numbers are CROSS-CHECKED against it and a
                disagreement is a refusal.
  ``module``    an overlay: Vagrant Story's ``.PRG`` files, Crash Bash's ``.BIN`` overlays. NO header,
                NO entry point, and no address derivable from the file alone. The load base and the
                code window come from the OWNING PORT's measured facts, as manifest DATA, because a
                reader that guessed them would be guessing.

The distinction is a FIELD, not a branch in the logic: :class:`ImageSpec` carries whichever fields
apply and both kinds go through the same offset arithmetic, so an overlay and a resident executable
are read by ONE implementation rather than two that drift.
"""

from __future__ import annotations

import hashlib
import json
import struct
from dataclasses import dataclass
from pathlib import Path

MANIFEST = Path(__file__).resolve().parent / "manifest.json"

# The PS-X EXE header is fixed-size and its text always begins 0x800 bytes into the file.
PSX_EXE_MAGIC = b"PS-X EXE"
PSX_EXE_TEXT_FILE_OFFSET = 0x800
PSX_EXE_HEADER_SIZE = 0x800

# A guest RAM image lives in 2 MiB of KSEG0. A load base outside it, or one that would run a module
# off the end of RAM, cannot be right, and is refused rather than slowly analysed.
KSEG0_FIRST = 0x80000000
KSEG0_LAST = 0x80200000

RESIDENT = "resident"
MODULE = "module"


class ImageRefusal(Exception):
    """The image is absent, is the wrong kind, or disagrees with the manifest.

    Raised, never returned as a short read. A refusal that surfaced as an empty inventory would be
    indistinguishable from an image whose functions are all bodyless, which is the failure this
    pipeline exists to make visible.
    """


@dataclass(frozen=True)
class ImageSpec:
    """One image's load geometry, read from the manifest.

    ``kind`` is ``resident`` for a PS-X EXE and ``module`` for an overlay. The fields that do not
    apply to a kind are None, and :meth:`validate` names the FIELD a manifest entry is missing,
    because an entry missing a required field must not analyse as an image with nothing in it.
    """

    name: str
    title: str
    serial: str
    kind: str
    # Resident geometry, cross-checked against the file's own header.
    text_load_address: int | None = None
    text_file_offset: int | None = None
    text_size: int | None = None
    # Module geometry, from the owning port's measured facts.
    load_base: int | None = None
    code_first: int | None = None
    code_last: int | None = None
    # Optional identity gate, so a wrong FILE cannot produce a measurement.
    sha1: str | None = None
    note: str = ""

    # -- derived ------------------------------------------------------------------------------------

    @property
    def first_address(self) -> int:
        """The lowest guest address this image's window covers."""
        return self.text_load_address if self.kind == RESIDENT else self.load_base

    @property
    def last_address(self) -> int:
        """The highest guest address this image's window covers."""
        if self.kind == RESIDENT:
            return self.text_load_address + self.text_size
        return self.load_base + self.code_last

    @property
    def ghidra_base(self) -> int:
        """Where Ghidra's Raw Binary loader must be pointed for this image's addresses to line up.

        For a resident EXE that is ``load - text_file_offset``, so the executable's own header lands
        BELOW the text window instead of shifting every guest address. For a module, whose text
        begins at file offset 0, it is the load base itself.
        """
        if self.kind == RESIDENT:
            return self.text_load_address - self.text_file_offset
        return self.load_base

    def file_offset(self, address: int) -> int:
        if self.kind == RESIDENT:
            return self.text_file_offset + (address - self.text_load_address)
        return address - self.load_base

    def covers(self, address: int, length: int = 4) -> bool:
        return self.first_address <= address and address + length <= self.last_address

    def missing_fields(self) -> list[str]:
        """The geometry fields this entry's kind requires and does not have."""
        required = {
            RESIDENT: ("text_load_address", "text_file_offset", "text_size"),
            MODULE: ("load_base", "code_first", "code_last"),
        }.get(self.kind, ())
        return [name for name in required if getattr(self, name) is None]

    def validate(self) -> None:
        """Refuse an entry that cannot describe an image. Names the FIELD, not the file."""
        if self.kind not in (RESIDENT, MODULE):
            raise ImageRefusal(
                f"manifest entry {self.name!r} has kind {self.kind!r}; it must be {RESIDENT!r} or "
                f"{MODULE!r}. A third kind would need a branch in the code, which is exactly what the "
                "manifest exists to avoid.")
        missing = self.missing_fields()
        if missing:
            raise ImageRefusal(
                f"manifest entry {self.name!r} is a {self.kind} and is missing {', '.join(missing)}."
                + (" For a module those are the owning port's MEASURED load base and code window: a "
                   "module carries no header to read them from, so guessing is the only alternative "
                   "and a guess here is invisible rather than loud." if self.kind == MODULE else ""))
        if self.kind == MODULE:
            if self.code_last < self.code_first:
                raise ImageRefusal(
                    f"manifest entry {self.name!r} has a code window 0x{self.code_first:X}.."
                    f"0x{self.code_last:X} that ends before it starts.")
            if not KSEG0_FIRST <= self.load_base < KSEG0_LAST:
                raise ImageRefusal(
                    f"manifest entry {self.name!r} has load base 0x{self.load_base:08X}, outside the "
                    f"2 MiB of KSEG0 RAM (0x{KSEG0_FIRST:08X}..0x{KSEG0_LAST:08X}). A PSX module "
                    "cannot load there.")
            if self.last_address > KSEG0_LAST:
                raise ImageRefusal(
                    f"manifest entry {self.name!r} loads at 0x{self.load_base:08X} and its code runs "
                    f"to 0x{self.last_address:08X}, past the end of RAM at 0x{KSEG0_LAST:08X}. The "
                    "base and the window disagree, and at least one of them is wrong.")
        if self.sha1 is not None and len(self.sha1) != 40:
            raise ImageRefusal(
                f"manifest entry {self.name!r} declares sha1 {self.sha1!r}, which is not a SHA-1 "
                "(40 hex digits). An identity gate that cannot be right is not a gate.")

    def as_dict(self) -> dict:
        missing = self.missing_fields()
        unmeasured = "unmeasured (no %s)" % ", ".join(missing)
        return {
            "name": self.name,
            "title": self.title,
            "serial": self.serial,
            "kind": self.kind,
            "ghidra_base": unmeasured if missing else "0x%08X" % self.ghidra_base,
            "window": unmeasured if missing else "0x%08X..0x%08X" % (self.first_address, self.last_address),
            "sha1": self.sha1,
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
    """The PS-X EXE header, or REFUSE. A raw module has no header and is a different kind."""
    if not path.is_file():
        raise ImageRefusal(
            f"no image at {path}. This tool needs the admitted image (provision it first); refusing "
            "rather than reporting an empty inventory, because 'the image is absent' and 'the image "
            "analyses to nothing' would otherwise print the same line."
        )
    with path.open("rb") as handle:
        head = handle.read(PSX_EXE_HEADER_SIZE)
        size = path.stat().st_size
    if len(head) < 0x40:
        raise ImageRefusal(f"{path} is {len(head)} bytes, too short to hold a PS-X EXE header")
    if head[:8] != PSX_EXE_MAGIC:
        raise ImageRefusal(
            f"{path} starts {head[:8]!r}, not {PSX_EXE_MAGIC!r}. A raw module is not a PS-X EXE: it "
            "has no header, so its load base and code window must come from the manifest as "
            "measured data. Point --image at the right file, or declare it as kind 'module'.")
    entry, gp = struct.unpack_from("<II", head, 0x10)
    load, text_size = struct.unpack_from("<II", head, 0x18)
    sp_base, sp_offset = struct.unpack_from("<II", head, 0x30)
    return PsxExeHeader(entry, gp, load, text_size, sp_base, sp_offset, size)


def cross_check_resident(spec: ImageSpec, header: PsxExeHeader, path: Path) -> None:
    """Refuse when the manifest disagrees with a PS-X EXE's own header.

    This is the assertion that catches a wrong base address, and it is the seeded difference the
    selftest uses. A wrong base does not fail on its own: Ghidra imports happily and every function
    lands at a plausible address, just not the one the title's documents name.
    """
    if spec.text_load_address != header.load:
        raise ImageRefusal(
            f"{spec.name} ({spec.serial}) manifest says text loads at 0x{spec.text_load_address:08X} "
            f"but {path} says 0x{header.load:08X}. Every guest address the pipeline reports would be "
            "wrong. Fix the manifest entry, not the base.")
    if spec.text_file_offset != PSX_EXE_TEXT_FILE_OFFSET:
        raise ImageRefusal(
            f"{spec.name} manifest says text starts at file offset 0x{spec.text_file_offset:X}, but "
            f"a PS-X EXE always starts it at 0x{PSX_EXE_TEXT_FILE_OFFSET:X}. Overlays may differ; a "
            "main executable may not.")
    if spec.text_size != header.text_size:
        raise ImageRefusal(
            f"{spec.name} manifest says text size 0x{spec.text_size:X} but {path} says "
            f"0x{header.text_size:X}. The window would cover the wrong bytes.")
    if PSX_EXE_TEXT_FILE_OFFSET + header.text_size > header.file_size:
        raise ImageRefusal(
            f"{path} holds {header.file_size} bytes but its header declares {header.text_size} bytes "
            f"of text at offset 0x{PSX_EXE_TEXT_FILE_OFFSET:X}, which runs past the end of the file. "
            "The image is truncated.")


def check_module_geometry(spec: ImageSpec, path: Path, size: int) -> None:
    """A module has no header, so its window is checked against the FILE instead.

    The load base cannot be checked against anything -- that is what "measured by the owning port"
    means, and the SHA-1 gate below is what keeps a wrong FILE from being read. What CAN be checked
    is that the code window lies inside the file, and a window that runs past the end is the shape a
    wrong base produces, so it is refused rather than analysed to nothing.
    """
    if spec.sha1 is not None:
        digest = hashlib.sha1(path.read_bytes()).hexdigest()
        if digest != spec.sha1:
            raise ImageRefusal(
                f"{path} hashes to SHA-1 {digest}, but manifest entry {spec.name!r} declares "
                f"{spec.sha1}. Refusing to read a file that is not the one the measured geometry "
                "belongs to: every address would be a plausible address of the wrong code."
            )
    # `code_last` is the offset of the last CODE BYTE, so the largest legal value is `size - 1` and
    # a window ending exactly at the file size is one byte past the end. MEASURED: the manifest
    # records whole-file windows for the Vagrant modules, whose `code_last` is therefore the file
    # size MINUS ONE, and an earlier `>= size` check here refused all three of them.
    if spec.code_last >= size:
        raise ImageRefusal(
            f"{path} is {size} bytes (0x{size:X}) but manifest entry {spec.name!r} runs code to file "
            f"offset 0x{spec.code_last:X}, which is past the end. `code_last` is the last CODE BYTE, "
            f"so for a whole-file window it is 0x{size - 1:X}. At load base 0x{spec.load_base:08X} a "
            "window past the end means the base and the window disagree, and at least one is wrong."
        )
    if spec.code_first < 0 or spec.code_last < spec.code_first:
        raise ImageRefusal(
            f"manifest entry {spec.name!r} has a code window 0x{spec.code_first:X}..0x{spec.code_last:X}"
            " that is not a forward range inside the file.")


def load_manifest(path: Path | None = None) -> dict[str, ImageSpec]:
    """Read the per-image manifest. Refuses an absent, malformed, or EMPTY manifest.

    An empty manifest is refused because every lookup against it would answer "not found" and read
    as "this tool has no data for your title" rather than "the data file is empty" -- the same
    zero-means-nothing defect the rest of this pipeline exists to prevent.
    """
    manifest_path = Path(path) if path is not None else MANIFEST
    if not manifest_path.is_file():
        raise ImageRefusal(
            f"no image manifest at {manifest_path}. The per-image load geometry lives there; it is "
            "data, not code, so adding a title is a JSON edit."
        )
    try:
        raw = json.loads(manifest_path.read_text())
    except json.JSONDecodeError as error:
        raise ImageRefusal(f"{manifest_path} is not valid JSON: {error}") from error
    images = raw.get("images") if isinstance(raw, dict) else None
    if not isinstance(images, dict) or not images:
        raise ImageRefusal(
            f"{manifest_path} has no non-empty 'images' object, so no image can be resolved. "
            "Refusing rather than reporting every lookup as unknown."
        )
    specs: dict[str, ImageSpec] = {}
    for name, entry in images.items():
        kind = entry.get("kind", RESIDENT)
        specs[name] = ImageSpec(
            name=name,
            title=entry.get("title", name),
            serial=entry.get("serial", ""),
            kind=kind,
            text_load_address=_maybe_int(entry.get("text_load_address")),
            text_file_offset=_maybe_int(entry.get("text_file_offset")),
            text_size=_maybe_int(entry.get("text_size")),
            load_base=_maybe_int(entry.get("load_base")),
            code_first=_maybe_int(entry.get("code_first")),
            code_last=_maybe_int(entry.get("code_last")),
            sha1=(entry.get("sha1") or None),
            note=entry.get("note", ""),
        )
    # VALIDATION IS PER ENTRY, NOT PER MANIFEST. It used to run here, for every entry, as the
    # manifest loaded - so one entry missing a field made the WHOLE manifest unreadable and took every
    # other title's RE with it. That is the wrong blast radius twice over: an incomplete entry is a
    # fact about THAT entry, and reading the manifest is not a request to analyse any of them.
    #
    # It matters here because the honest state of a title's module geometry is routinely "not
    # measured": Tomba! 2's 28 module entries all asserted the same load base as one blanket copy, and
    # the one entry that could be measured proved the copy wrong. Refusing the unmeasured entries is
    # correct - a wrong base imports cleanly and answers about a body 0x8F9C away from the one the
    # documents name - but it must refuse only when that entry is asked for, so the measured entry and
    # every other title keep working.
    return _SpecsByValidatedEntry(specs)


class _SpecsByValidatedEntry(dict):
    """The manifest, validating each entry as it is read rather than as the file is loaded."""

    def __getitem__(self, key):
        spec = super().__getitem__(key)
        spec.validate()
        return spec

    def get(self, key, default=None):
        if key in self:
            return self[key]
        return default


def _maybe_int(value) -> int | None:
    """A manifest integer, or None. Accepts ``"0x80010000"`` and ``"0x800"`` as written in the file."""
    if value is None or value == "":
        return None
    if isinstance(value, int):
        return value
    return int(value, 0)


def select(manifest: dict[str, ImageSpec], name: str | None) -> ImageSpec:
    """One spec by name, or a refusal that NAMES what exists.

    The name list is the denominator: "unknown image" and "this tool knows N images" are different
    facts, and a reader who gets the first has no way to learn the second.
    """
    if name:
        if name in manifest:
            return manifest[name]
        raise ImageRefusal(
            f"no manifest entry named {name!r}. Known images: " + ", ".join(sorted(manifest))
            + f" ({len(manifest)} known). Add the image to tools/decomp/manifest.json; its load "
            "geometry is data, not a code change."
        )
    if len(manifest) == 1:
        return next(iter(manifest.values()))
    raise ImageRefusal(
        f"--image-name is required: the manifest knows {len(manifest)} images "
        f"({', '.join(sorted(manifest))}), so there is no single default to guess.")


class ImageWindow:
    """The admitted image, read whole, with its guest<->file mapping.

    One instance per run. ``data`` is the only payload; every address question is asked of this
    object so there is exactly one offset formula in the process.
    """

    def __init__(self, spec: ImageSpec, path: Path):
        self.spec = spec
        self.path = Path(path)
        self.spec.validate()
        if not self.path.is_file():
            raise ImageRefusal(
                f"no image at {self.path}. This tool needs the admitted image (provision it first); "
                "refusing rather than reporting an empty inventory.")
        self.size = self.path.stat().st_size
        if self.spec.kind == RESIDENT:
            self.header = read_psx_exe_header(self.path)
            cross_check_resident(self.spec, self.header, self.path)
            if self.header.entry == 0:
                raise ImageRefusal(
                    f"{self.path} declares entry point 0. A PS-X EXE with no entry cannot seed "
                    "disassembly, so every function would be unreachable and the inventory would read "
                    "as an empty image rather than a malformed one."
                )
            self.entry = self.header.entry
        else:
            self.header = None
            check_module_geometry(self.spec, self.path, self.size)
            # A module's entry is the first byte of its CODE window, and the code window is measured.
            # Not a function entry -- seeding there is a linear sweep, and the entry points the
            # analysis finds are the ones with a call site or a prologue.
            self.entry = self.spec.load_base + self.spec.code_first
        self.data = self.path.read_bytes()

    @property
    def text_size(self) -> int:
        return self.header.text_size if self.header else (self.spec.code_last - self.spec.code_first)

    @property
    def text_end(self) -> int:
        return self.spec.last_address

    def covers(self, address: int, length: int = 4) -> bool:
        return (self.spec.covers(address, length)
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
                f"0x{address:08X} is outside the window "
                f"0x{self.spec.first_address:08X}..0x{self.text_end:08X}")
        return struct.unpack_from("<I", self.data, self.file_offset(address))[0]
