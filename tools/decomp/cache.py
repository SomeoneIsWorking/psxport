"""Where the ANALYZED Ghidra project for one image lives, and whether it exists yet.

ONE CONCEPT: the cache key. Analysis of a 340 KB PSX text window is the expensive half of every RE
question, it depends on nothing but the image's bytes, and re-deriving it per question is why reading
a game one address at a time costs hours. So the analysis is kept, keyed by the image's SHA-256, and
the second question is asked against it with ``-process -noanalysis``.

THE KEY IS THE IMAGE'S HASH AND NOTHING ELSE, deliberately. Keying by manifest entry name would let a
different region, a different revision, or a re-provisioned file answer as if it were the image every
document names, and the failure is invisible: Ghidra opens a project, analyzes happily, and reports
addresses that are plausible and wrong. The full digest is in the directory name, so two images with
the same name never share a directory and a reader can see which bytes a project holds.

THE DIRECTORY IS UNDER ``<repo>/build/ghidra/``, which is git-ignored build output by policy, and
that is CHECKED rather than assumed -- the same ``git check-ignore`` question the decompile output
directory already asks, for the same reason: an analyzed guest program is derived data and must never
be committed.

WHAT IS AND IS NOT CACHED. The Ghidra project (functions, references, symbols) is. The run's own
artefacts -- ``inventory.json``, the decompiled C, ``queries.json`` -- are not: they are written under
the caller's ``--out`` every run, so a warm run's answers are as inspectable as a cold run's and two
runs cannot read each other's results.
"""

from __future__ import annotations

import hashlib
import json
import shutil
from dataclasses import dataclass, replace
from pathlib import Path

STATE_FILE = "analysis.json"
# The directory NAME pattern this tool is willing to delete. The refusal below is a guard against a
# computed path that stopped being what it was: an image named `..` or a future refactor that changes
# the key must fail loudly rather than remove a tree.
DIGEST_LENGTH = 64


class CacheRefusal(Exception):
    """The cached analysis cannot be trusted, identified, or safely replaced. Never a stale reuse."""


def image_sha256(path: Path) -> str:
    """The image's SHA-256, read in chunks so a 600 KB EXE is not the reason a run is refused."""
    digest = hashlib.sha256()
    with Path(path).open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


@dataclass(frozen=True)
class ProjectSlot:
    """One image's analyzed project: where it is, what it is called, and what it was made from."""

    directory: Path
    project_name: str
    sha256: str
    image_name: str
    image_path: Path
    ghidra_base: int
    text_first: int
    text_last: int
    entry_address: int
    # False when the caller named its own directory with --project-dir. Such a directory is not
    # removed by this tool even on --fresh: it was not this tool's to create, so it is not this
    # tool's to delete.
    managed: bool = True

    @property
    def state_path(self) -> Path:
        return self.directory / STATE_FILE

    def facts(self) -> dict:
        return {
            "image_name": self.image_name,
            "image_sha256": self.sha256,
            "image_path": str(self.image_path),
            "ghidra_base": "0x%08X" % self.ghidra_base,
            "text_first": "0x%08X" % self.text_first,
            "text_last": "0x%08X" % self.text_last,
            "entry_address": "0x%08X" % self.entry_address,
            "project_name": self.project_name,
        }


def project_root(repo_root: Path) -> Path:
    """``<repo>/build/ghidra`` -- build output, git-ignored, owned by the build, not by a scratch run."""
    return Path(repo_root) / "build" / "ghidra"


def slot_for(spec, image_path: Path, repo_root: Path, *, directory: Path | None = None) -> ProjectSlot:
    """The slot for this image, hashing it. A directory override is the caller's explicit choice."""
    image_path = Path(image_path).resolve()
    digest = image_sha256(image_path)
    if directory is not None:
        slot_directory = Path(directory).resolve()
        managed = False
    else:
        slot_directory = (project_root(repo_root) / spec.name / digest).resolve()
        managed = True
    return ProjectSlot(
        directory=slot_directory,
        project_name=spec.name,
        sha256=digest,
        image_name=spec.name,
        image_path=image_path,
        ghidra_base=spec.ghidra_base,
        text_first=spec.first_address,
        text_last=spec.last_address - 1,
        entry_address=0,  # filled in by with_entry(), which is the only thing that knows the window
        managed=managed,
    )


def with_entry(slot: "ProjectSlot", entry_address: int) -> "ProjectSlot":
    """The same slot with the seed address filled in.

    It is not part of :func:`slot_for` because the seed is a property of the IMAGE and its window --
    a PS-X EXE's header entry, or a module's measured code start -- and only the caller holds both.
    """
    return replace(slot, entry_address=entry_address)


def assert_ignored(directory: Path, repo_root: Path) -> None:
    """Refuse to write derived data where git would not ignore it. The ONE place that asks.

    Two callers need this -- the run's own output directory and the analyzed project directory -- and
    they need it for the same reason with different nouns: decompiled guest C and an analyzed guest
    program are both derived data that must never be committed. One implementation, because two
    `git check-ignore` calls that drift would leave one of the two unprotected without anyone noticing.
    """
    import subprocess  # noqa: PLC0415 - only this owner shells out, and only for this question

    probe = subprocess.run(["git", "check-ignore", "-q", str(directory)], cwd=str(repo_root),
                           capture_output=True, text=True, check=False)
    if probe.returncode == 0:
        return
    raise CacheRefusal(
        f"{directory} is NOT git-ignored. Derived guest data (decompiled C, an analyzed project) must "
        f"never be committed. Put it under {repo_root}/build/ or the repo's scratch/ (both ignored). "
        "Refusing before anything is written."
    )


def is_analyzed(slot: ProjectSlot) -> bool:
    """Is there a COMPLETED analysis for exactly this image? A half-run is not one.

    The state file is written only after Ghidra exits successfully, and it records the image's digest,
    the manifest entry and the geometry the analysis was seeded with. A project directory with no
    state file is a run that died, and it is re-analyzed rather than opened -- which is why
    :func:`discard` exists and why a stale directory cannot be mistaken for a warm cache.
    """
    state = read_state(slot)
    if state is None:
        return False
    if state.get("image_sha256") != slot.sha256:
        return False
    if state.get("image_name") != slot.image_name:
        return False
    return bool(state.get("analyzed"))


def read_state(slot: ProjectSlot) -> dict | None:
    path = slot.state_path
    if not path.is_file():
        return None
    try:
        return json.loads(path.read_text())
    except json.JSONDecodeError as error:
        raise CacheRefusal(
            f"{path} is not valid JSON ({error}). Refusing rather than re-analyzing over an "
            "unreadable cache: the file that records what a project was made from has to be readable."
        ) from error


def mark_analyzed(slot: ProjectSlot, *, ghidra_version: str = "",
                  preseed: dict | None = None) -> None:
    """Record a COMPLETED analysis. Called only after the run returned successfully.

    ``preseed`` is the pre-script's own record of how disassembly was seeded, kept so a LATER run
    opening this project can answer the audit's "was the program seeded from a real entry" question
    without running an analysis. It is stored WITH its provenance, because a pre-script record read
    out of a cache is not the same claim as one this run produced, and a reader must be able to tell
    them apart.
    """
    slot.directory.mkdir(parents=True, exist_ok=True)
    document = slot.facts()
    document["analyzed"] = True
    document["ghidra_version"] = ghidra_version
    if preseed:
        record = dict(preseed)
        record["recorded_from"] = "the analysis run that created this project"
        record["recorded_sha256"] = slot.sha256
        document["preseed"] = record
    slot.state_path.write_text(json.dumps(document, indent=2, sort_keys=True) + "\n",
                               encoding="utf-8")


def recorded_preseed(slot: ProjectSlot) -> dict | None:
    """The pre-script record of the run that analyzed this image, or None.

    Replayed into a warm run's output directory so the decompile audit can still ask whether the
    program was seeded from a real entry. Without it a warm run would report "no pre-script record"
    for a program that demonstrably has one, and the cheapest correct response to that would be to
    weaken the audit.
    """
    state = read_state(slot)
    if state is None:
        return None
    preseed = state.get("preseed")
    if not isinstance(preseed, dict) or preseed.get("instructions_from_entry", 0) <= 0:
        return None
    return preseed


def discard(slot: ProjectSlot) -> None:
    """Remove this slot's directory, for a forced re-analysis. Scoped, and guarded.

    Only a directory whose own name is a 64-hex digest under a ``build/ghidra`` tree is removed. That
    is deliberately narrow: a computed path that has become something else -- a symlink, a
    mis-keyed join, a future change to the layout -- must raise rather than delete.
    """
    directory = slot.directory
    if not slot.managed:
        raise CacheRefusal(
            f"{directory} was named by --project-dir, so this tool did not create it and will not "
            "remove it. Delete it yourself, or drop --project-dir to use the keyed cache."
        )
    if directory.is_symlink():
        raise CacheRefusal(f"{directory} is a symlink, so this tool will not remove what it names.")
    if len(directory.name) != DIGEST_LENGTH:
        raise CacheRefusal(
            f"{directory} is not named as an image digest ({DIGEST_LENGTH} hex characters), so it is "
            "not a cache slot this tool created and it will not be removed."
        )
    if len(directory.parts) < 5 or directory.parts[-3] != "ghidra" or directory.parts[-4] != "build":
        raise CacheRefusal(
            f"{directory} is not a <repo>/build/ghidra/<image>/<digest> cache slot, so it was not "
            "created by this tool and it will not be removed. A --project-dir outside that shape is "
            "the caller's to delete."
        )
    if directory.exists():
        shutil.rmtree(directory)