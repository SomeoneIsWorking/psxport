"""The composition: lock, prepare, invoke, report.

ONE CONCEPT: the order of the steps, and the rule that each one refuses rather than degrading. The
steps themselves live in their own modules; this file only decides which comes first and what a
failure of one does to the others.

THE ORDER IS NOT ARBITRARY:

  1. check the Ghidra install and its PyGhidra bridge -- before the lock, so a machine that cannot
     run the tool at all does not make every other agent queue behind it;
  2. read the image and cross-check the manifest against the image's own header -- before the lock,
     because it costs nothing and a wrong base discovered after a 20-minute analysis is a wasted
     20 minutes;
  3. decide whether this run ANALYZES or REUSES, by hashing the image into a project slot;
  4. take the Ghidra lock -- the one scarce resource;
  5. invoke, with a bounded heap;
  6. release the lock, even on failure;
  7. report and audit, and AUDIT AFTER THE LOCK IS RELEASED, because a reader staring at a report
     holds the machine hostage for as long as they stare.

STEP 3 IS WHY THE SECOND QUESTION IS CHEAP. Analysis depends on the image's bytes and nothing else, so
it is kept under a slot keyed by the image's SHA-256 (:mod:`tools.decomp.cache`) and a later run opens
it with ``-process -noanalysis``. The decision is made from the recorded state, never inferred from a
directory merely existing: a run that died leaves a project with no record, and treating that as an
analysis would answer every later question from a half-built program.
"""

from __future__ import annotations

import json
import time
from dataclasses import dataclass, field
from pathlib import Path

from . import cache, headless, images, lock, queries as queries_module
from . import report as report_module


@dataclass
class DecompRun:
    """Everything one run needs. Values, not paths to look up later, so a refusal names them."""

    image_name: str
    image_path: Path
    output_dir: Path
    targets: list[int]
    # The question half. `--callers`, `--refs`, `--function-at` all land here, and they are answered
    # in the SAME Ghidra launch as any --target, because a launch costs minutes and the questions
    # cost nothing once the program is open.
    queries: list[queries_module.Query] = field(default_factory=list)
    # Where the analyzed project lives. None means the consuming repo's build/ghidra/<name>/<sha256>,
    # resolved against `repo_root`; a Path is the caller's explicit directory.
    project_dir: Path | None = None
    fresh: bool = False
    ghidra_home: Path = headless.DEFAULT_GHIDRA_HOME
    lock_dir: Path | None = None
    lock_wait_seconds: float = 1800.0
    clear_noreturn: str = "all"
    max_heap_mb: int = headless.DEFAULT_MAX_HEAP_MB
    timeout_seconds: int = headless.DEFAULT_TIMEOUT_SECONDS
    inventory_limit: int | None = 40
    runner: headless.Runner = field(default_factory=headless.SubprocessRunner)

    def spec(self, manifest_path: Path | None = None) -> images.ImageSpec:
        return images.select(images.load_manifest(manifest_path), self.image_name)


@dataclass
class RunOutcome:
    """What one run produced, and how it got there.

    ``mode`` is ``analyze`` or ``reuse``, and ``seconds`` is the wall time of the Ghidra launch, so a
    reader can see the difference the cache makes rather than being told it exists.
    """

    mode: str
    seconds: float
    slot: cache.ProjectSlot
    report: report_module.Report | None
    query_report: queries_module.QueryReport | None


def _read_preseed(output_dir: Path) -> dict | None:
    """The pre-script's record for this run, or None when it never wrote one.

    None is NOT ignored downstream: :func:`tools.decomp.cache.mark_analyzed` stores what it is given,
    and a project whose state file records no seed is one whose audit will say the seed is unrecorded
    rather than inventing one.
    """
    path = Path(output_dir) / "preseed.json"
    if not path.is_file():
        return None
    try:
        return json.loads(path.read_text())
    except json.JSONDecodeError:
        return None


def _resolve_repo_root(repo_root: Path | None) -> Path:
    """The CONSUMING repository, which is where build/ and scratch/ belong.

    Not the framework's own root: this file lives in ``external/psxport`` (a link to the framework
    checkout) and every caller is a PORT whose build tree and git-ignore rules are its own. Writing an
    analyzed project under the framework's root would put a title's guest program inside the framework
    repository, which is exactly the mistake the git-ignore check below exists to prevent.
    """
    if repo_root is not None:
        return Path(repo_root).resolve()
    import subprocess  # noqa: PLC0415 - one question, asked once, here

    probe = subprocess.run(["git", "rev-parse", "--show-toplevel"], capture_output=True, text=True,
                           check=False)
    if probe.returncode == 0 and probe.stdout.strip():
        return Path(probe.stdout.strip()).resolve()
    return Path.cwd().resolve()


def execute(run: DecompRun, manifest_path: Path | None = None,
            repo_root: Path | None = None) -> RunOutcome:
    """Run the pipeline end to end. Raises the first refusal; returns the audited outcome."""
    # 0. the cheapest refusal first: a run with no targets and no questions is a run that establishes
    # nothing, and it must not cost a Ghidra install check, a header read, or a queue behind another
    # agent.
    if not run.targets and not run.queries:
        raise images.ImageRefusal(
            "nothing was asked. Name functions to read (--target 0x80012340 ...) or addresses to ask "
            "about (--callers / --refs / --function-at 0x...). A run with neither decompiles nothing, "
            "answers nothing, and is not a result.")
    # 1. the install, before anything is queued or written.
    interpreter = headless.resolve_interpreter(run.ghidra_home)
    headless.check_installation(run.ghidra_home, interpreter, run.runner)

    # 2. the image and the manifest. A resident EXE is cross-checked against its OWN header; a module
    #    has no header, so its measured base and window are trusted as data and the file is gated by
    #    the SHA-1 instead. Both go through one reader -- two implementations would drift exactly where
    #    a re-port drifts.
    spec = run.spec(manifest_path)
    window = images.ImageWindow(spec, run.image_path)

    consuming_root = _resolve_repo_root(repo_root)
    output_dir = Path(run.output_dir)
    cache.assert_ignored(output_dir, consuming_root)
    output_dir.mkdir(parents=True, exist_ok=True)

    # 3. the project slot: hashed from the image, and the mode decided from a RECORDED analysis.
    slot = cache.with_entry(
        cache.slot_for(spec, run.image_path, consuming_root, directory=run.project_dir),
        window.entry)
    cache.assert_ignored(slot.directory, consuming_root)
    if run.fresh and slot.managed:
        cache.discard(slot)
    analyzed = cache.is_analyzed(slot)
    if not analyzed and slot.directory.exists() and slot.managed:
        # A directory with NO state file is a run that died, not an analysis. Ghidra refuses to
        # import into an existing project, so leaving it would make every later run fail on a
        # directory this tool created. Removing it is scoped to this exact slot.
        cache.discard(slot)

    if run.targets:
        (output_dir / "targets.txt").write_text(
            "\n".join("0x%08X" % t for t in sorted(set(run.targets))) + "\n", encoding="utf-8")
    query_path = None
    if run.queries:
        query_path = queries_module.write_query_file(output_dir / "queries.txt", run.queries)
    # A WARM run runs no pre-script, so there is nothing new to record. The record of the run that
    # DID analyze this program is replayed here instead, because the decompile audit asks whether the
    # program was seeded from a real entry and the honest answer to that on a warm run is the earlier
    # run's own record, cited as such.
    if analyzed:
        preseed = cache.recorded_preseed(slot)
        if preseed is not None:
            (output_dir / "preseed.json").write_text(
                json.dumps(preseed, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    lock_dir = run.lock_dir or lock.default_lock_dir()
    handle = lock.GhidraLock(directory=Path(lock_dir), wait_seconds=run.lock_wait_seconds,
                             holder_note=f"{run.image_name} {run.image_path.name}")
    # 4-5. the scarce resource, held for the invocation only.
    handle.acquire()
    started = time.monotonic()
    try:
        invocation = headless.GhidraInvocation(
            interpreter=interpreter,
            ghidra_home=run.ghidra_home,
            project_dir=slot.directory,
            project_name=slot.project_name,
            import_path=Path(run.image_path),
            base_address=spec.ghidra_base,
            text_first=spec.first_address,
            text_last=spec.last_address - 1,
            # Disassembly is seeded from the image's OWN declared entry point, not from the start of
            # the text window. Measured on Spyro 1: the first byte of the window is DATA, so seeding
            # there produced 0 instructions while the declared entry 0x8005B8E0 produced 670
            # functions. A resident's entry is its header's; a MODULE has no header and no entry, so
            # ImageWindow derives one from the measured code window's first byte. The report prints
            # which of the two it used, because "seeded at the window start" and "seeded at the
            # entry point" are different claims about the same address.
            entry_address=window.entry,
            image_name=spec.name,
            image_kind=spec.kind,
            output_dir=output_dir,
            postscript=Path(headless.__file__).resolve().parent / "postscript.py",
            prescript=Path(headless.__file__).resolve().parent / "prescript.py",
            clear_noreturn=run.clear_noreturn,
            max_heap_mb=run.max_heap_mb,
            timeout_seconds=run.timeout_seconds,
            queryscript=(Path(headless.__file__).resolve().parent / "queryscript.py"
                         if run.queries else None),
            queries_path=query_path,
            decompile_targets=bool(run.targets),
            reuse_project=analyzed,
        )
        # 4. the run itself.
        invocation.run(run.runner)
        seconds = time.monotonic() - started
        if not analyzed:
            # 5b. The analysis is recorded only now, after a run that returned. Recording it on
            #     START is how a crashed run becomes a cache entry that answers every later question
            #     from a program that was never analyzed.
            cache.mark_analyzed(slot, preseed=_read_preseed(output_dir))
    finally:
        # 6. released even when the run refused, so one failure does not strand the machine.
        handle.release()
    # 7. read and audit, with the lock already gone.
    report = report_module.load_report(output_dir / "inventory.json") if run.targets else None
    query_report = None
    if run.queries:
        query_report = queries_module.load_query_report(output_dir / "queries.json",
                                                        asked=list(run.queries))
    return RunOutcome(mode="reuse" if analyzed else "analyze", seconds=seconds, slot=slot,
                      report=report, query_report=query_report)