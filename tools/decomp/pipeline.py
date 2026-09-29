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
  3. take the Ghidra lock -- the one scarce resource;
  4. invoke, with a bounded heap;
  5. release the lock, even on failure;
  6. report and audit, and AUDIT AFTER THE LOCK IS RELEASED, because a reader staring at a report
     holds the machine hostage for as long as they stare.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path

from . import headless, images, lock, report as report_module


@dataclass
class DecompRun:
    """Everything one run needs. Values, not paths to look up later, so a refusal names them."""

    title: str
    image_path: Path
    output_dir: Path
    targets: list[int]
    ghidra_home: Path = headless.DEFAULT_GHIDRA_HOME
    lock_dir: Path | None = None
    lock_wait_seconds: float = 1800.0
    clear_noreturn: str = "all"
    max_heap_mb: int = headless.DEFAULT_MAX_HEAP_MB
    timeout_seconds: int = headless.DEFAULT_TIMEOUT_SECONDS
    inventory_limit: int | None = 40
    runner: headless.Runner = field(default_factory=headless.SubprocessRunner)

    def spec(self, manifest_path: Path | None = None) -> images.ImageSpec:
        return images.select(images.load_manifest(manifest_path), self.title)


def _assert_output_dir_is_ignored(output_dir: Path, repo_root: Path) -> None:
    """Refuse to write derived C outside a git-ignored path.

    Decompiled guest C is a READING AID, never a build input, and this workspace must never commit
    it -- least of all the AGPL-3.0 MMX4 decomp or any third party's text. The check is mechanical
    rather than a rule people have to remember: ``git check-ignore`` answers whether the path is
    ignored, and the pipeline asks before it writes.
    """
    import subprocess

    probe = subprocess.run(["git", "check-ignore", "-q", str(output_dir)], cwd=str(repo_root),
                           capture_output=True, text=True, check=False)
    if probe.returncode == 0:
        return
    raise images.ImageRefusal(
        f"{output_dir} is NOT git-ignored, and decompiled guest C must never be committed. Put it "
        f"under {repo_root}/scratch/ (which is ignored) or add an ignore rule. Refusing before "
        "writing anything."
    )


def execute(run: DecompRun, manifest_path: Path | None = None,
            repo_root: Path | None = None) -> report_module.Report:
    """Run the pipeline end to end. Raises the first refusal; returns the audited report."""
    # 0. the cheapest refusal first: a run with no targets is a run that establishes nothing, and
    # it must not cost a Ghidra install check, a header read, or a queue behind another agent.
    if not run.targets:
        raise images.ImageRefusal(
            "no target addresses. Name the functions to read (--target 0x80012340 ...). A run with "
            "no targets decompiles nothing and is not a result.")
    # 1. the install, before anything is queued or written.
    interpreter = headless.resolve_interpreter(run.ghidra_home)
    headless.check_installation(run.ghidra_home, interpreter, run.runner)

    # 2. the image and the manifest, cross-checked against the image's own header.
    spec = run.spec(manifest_path)
    window = images.ImageWindow(spec, run.image_path)

    output_dir = Path(run.output_dir)
    if repo_root is not None:
        _assert_output_dir_is_ignored(output_dir, Path(repo_root))
    output_dir.mkdir(parents=True, exist_ok=True)
    targets_file = output_dir / "targets.txt"
    targets_file.write_text(
        "\n".join("0x%08X" % t for t in sorted(set(run.targets))) + "\n", encoding="utf-8")

    lock_dir = run.lock_dir or lock.default_lock_dir()
    handle = lock.GhidraLock(directory=Path(lock_dir), wait_seconds=run.lock_wait_seconds,
                             holder_note=f"{run.title} {run.image_path.name}")
    # 3-5. the scarce resource, held for the invocation only.
    handle.acquire()
    try:
        invocation = headless.GhidraInvocation(
            interpreter=interpreter,
            ghidra_home=run.ghidra_home,
            project_dir=output_dir / "project",
            project_name="decomp",
            import_path=Path(run.image_path),
            base_address=spec.ghidra_base,
            text_first=window.header.load,
            text_last=window.text_end - 1,
            # Disassembly is seeded from the image's OWN declared entry point, not from the start
            # of the text window. Measured on Spyro 1: the first byte of the window is DATA, so
            # seeding there produced 0 instructions while the declared entry 0x8005B8E0 produced 670
            # functions. ImageWindow.open refuses an image whose entry is 0, so this is never zero.
            entry_address=window.header.entry,
            output_dir=output_dir,
            postscript=Path(headless.__file__).resolve().parent / "postscript.py",
            prescript=Path(headless.__file__).resolve().parent / "prescript.py",
            clear_noreturn=run.clear_noreturn,
            max_heap_mb=run.max_heap_mb,
            timeout_seconds=run.timeout_seconds,
        )
        # 4. the run itself.
        invocation.run(run.runner)
    finally:
        # 5. released even when the run refused, so one failure does not strand the machine.
        handle.release()
    # 6. read and audit, with the lock already gone.
    return report_module.load_report(output_dir / "inventory.json")
