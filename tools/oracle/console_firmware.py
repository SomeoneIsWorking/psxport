"""Explicit firmware admission; the core's permissive fallback is never an identity check."""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
from pathlib import Path
import re

# Search names belong to the core; admitted identities describe the actual firmware bytes.
FIRMWARE = {"na": "scph5501.bin", "jp": "scph5500.bin", "eu": "scph5502.bin"}
IDENTITIES = {
    "0555c6fae8906f3f09baf5988f00e55f88e9f30b": ("na", "SCPH-5501", "v3.0"),
    "b05def971d8ec59f346f2d9ac21fb742e3eb6917": ("jp", "SCPH-5500", "v3.0"),
    "f6bc2d1f5eb6593de7d089c425ac681d6fffd3f0": ("eu", "SCPH-5502", "v3.0"),
    "10155d8d6e6e832d6ea66db9bc098321fb5e8ebf":
        ("na", "SCPH-1001/5003/DTL-H1201/H3001", "v2.2 12-04-95 A"),
}


@dataclass(frozen=True)
class FirmwareSelection:
    region: str
    path: Path | None
    sha1: str | None
    model: str | None = None
    revision: str | None = None

    @property
    def authentic(self) -> bool:
        return self.path is not None

    def describe(self) -> dict:
        return {"region": self.region, "kind": "authentic" if self.authentic else "OpenBIOS",
                "sha1": self.sha1, "model": self.model, "revision": self.revision,
                "core_search_filename": FIRMWARE[self.region] if self.authentic else None,
                "limitation": None if self.authentic else
                "Built-in OpenBIOS; no authentic-BIOS boot/service/timing parity claim."}


def select_firmware(region: str, bios: Path | None, openbios: bool) -> FirmwareSelection:
    if region not in FIRMWARE:
        raise ValueError(f"unsupported firmware region {region!r}")
    if (bios is not None) == openbios:
        raise ValueError("choose exactly one explicit --bios or --openbios")
    if openbios:
        return FirmwareSelection(region, None, None)
    if bios is None:
        raise ValueError("authentic BIOS path is required")
    path = bios.resolve(strict=True)
    if path.stat().st_size != 512 * 1024:
        raise ValueError("authentic BIOS must be exactly 524288 bytes")
    digest = hashlib.sha1(path.read_bytes()).hexdigest()
    identity = IDENTITIES.get(digest)
    if identity is None or identity[0] != region:
        raise ValueError(f"BIOS identity mismatch: no admitted {region} firmware for SHA1 {digest}")
    return FirmwareSelection(region, path, digest, identity[1], identity[2])


def verify_loaded_firmware(selection: FirmwareSelection, core_log: str) -> dict:
    """Check the pinned core's actual selection before publishing a ready authentic session."""
    hashes = [digest.lower() for digest in re.findall(
        r"(?:Firmware|Obtained) SHA1: ([0-9a-fA-F]{40})(?![0-9A-Za-z])", core_log)]
    if "BIOS file short read" in core_log or "BIOS file size" in core_log:
        raise ValueError("core did not read a complete admitted BIOS")
    if selection.authentic:
        if hashes != [selection.sha1]:
            raise ValueError(f"core firmware selection differs from admitted identity: observed {hashes}")
    elif hashes:
        raise ValueError("explicit OpenBIOS session unexpectedly selected external firmware")
    return {"observed_sha1": hashes[0] if hashes else None,
            "core_revision_warning": "Firmware found but has invalid SHA1:" in core_log}



def prepare_system_directory(root: Path, selection: FirmwareSelection) -> Path:
    directory = root / "system"
    directory.mkdir(parents=True, exist_ok=True)
    # This directory is the sole firmware search surface. Foreign files could make the core's
    # permissive name/search fallback boot different firmware than the manifest declares.
    names = set(FIRMWARE.values())
    entries = list(directory.iterdir())
    for entry in entries:
        if entry.name not in names or not entry.is_symlink():
            raise ValueError(f"foreign firmware input in controlled system directory: {entry}")
    for entry in entries:
        entry.unlink()
    if selection.path is not None:
        (directory / FIRMWARE[selection.region]).symlink_to(selection.path)
    return directory
