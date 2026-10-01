"""How Ghidra is invoked at all: the launcher, the heap ceiling, and every refusal.

ONE CONCEPT: the boundary between this pipeline and the Ghidra process. Everything about *what* to
analyse lives in :mod:`tools.decomp.images` and :mod:`tools.decomp.pipeline`; everything about *how
to start the JVM and what it is allowed to cost* lives here.

The command runner is INJECTED, for two reasons that are both load-bearing rather than tidiness:
the selftest proves every refusal without a Ghidra install, and the environment is an argument
rather than ambient process state, so a caller can impose its own heap and cache location.

Every refusal here is an exception. A headless analyzer that is not runnable, a run that fails, and a
run that was never started must never produce the same empty inventory.
"""

from __future__ import annotations

import os
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

# The PyGhidra MODULE, not the `pyghidraRun` wrapper. Measured: the wrapper's argument parsing
# consumes one positional fewer than the headless analyzer needs, so a correct command line arrives
# as "Bad argument: <project name>" AND the process still exits 0. An exit-0 import that created no
# project reports a working run that produced nothing.
HEADLESS_CLASS = "ghidra.app.util.headless.AnalyzeHeadless"
LAUNCH_MODULE = "pyghidra.ghidra_launch"

# Ghidra 12.0.4's own PyGhidra virtual environment, which is the one built against this install and
# carries the matching JPype bridge. A system python that merely has `pyghidra` installed produces a
# version mismatch deep inside the launcher.
DEFAULT_GHIDRA_HOME = Path.home() / "dev" / "ghidra_12.0.4_PUBLIC"

# 1400 MB, MEASURED as the smallest ceiling that completed a full auto-analysis of Spyro 1's text
# window without the decompiler dying, on a machine with ~2 GB free and five other agents building.
# Ghidra's own default is 2 GB, which is the setting that OOM-kills a co-tenant. The number is a
# named constant rather than a literal at the call site so the doc and the code cannot disagree.
DEFAULT_MAX_HEAP_MB = 1400
DEFAULT_TIMEOUT_SECONDS = 5400


class GhidraRefusal(Exception):
    """Ghidra is not runnable, or the run failed or produced nothing. Never an empty result."""


class Runner:
    """The injected boundary: run a command and report (returncode, combined output)."""

    def run(self, command: list[str], cwd: Path, timeout: int,
            environment: dict[str, str]) -> tuple[int, str]:
        raise NotImplementedError  # pragma: no cover - interface


@dataclass
class SubprocessRunner(Runner):
    """The real runner. One command, one bounded wait, combined output returned."""

    def run(self, command: list[str], cwd: Path, timeout: int,
            environment: dict[str, str]) -> tuple[int, str]:
        try:
            done = subprocess.run(command, cwd=str(cwd), env=environment, capture_output=True,
                                  text=True, timeout=timeout, check=False)
        except subprocess.TimeoutExpired as error:
            raise GhidraRefusal(
                f"Ghidra did not finish within {timeout} s. Reported as a TIMEOUT, not as an empty "
                "decompile: a run that was cut off has established nothing."
            ) from error
        return done.returncode, (done.stdout or "") + (done.stderr or "")


@dataclass
class GhidraInvocation:
    """One headless run: the command, where its artefacts go, and what it was allowed to cost.

    TWO MODES, and the difference is the whole point of the analyzed-project cache:

      ``reuse_project=False``  ``-import`` the image and ANALYZE it. Expensive: tens of minutes.
      ``reuse_project=True``   ``-process`` an already-imported program with ``-noanalysis``, which
                               asks its questions against an analysis a previous run paid for.

    Neither mode deletes the project. Deleting it would make every question cost a full analysis,
    which is the state this tool exists to leave.
    """

    interpreter: Path
    ghidra_home: Path
    project_dir: Path
    project_name: str
    import_path: Path
    base_address: int
    text_first: int
    text_last: int
    entry_address: int
    output_dir: Path
    postscript: Path
    image_name: str = ""
    image_kind: str = ""
    prescript: Path | None = None
    clear_noreturn: str = "all"
    max_heap_mb: int = DEFAULT_MAX_HEAP_MB
    timeout_seconds: int = DEFAULT_TIMEOUT_SECONDS
    extra_language: str = "MIPS:LE:32:default"
    # The query half. `queryscript` is absent on a decompile-only run; `queries_path` is the FILE the
    # post-script reads the questions from, and it is an argument rather than a channel because a
    # diagnostic that cannot answer a question it was not asked is not a diagnostic.
    queryscript: Path | None = None
    queries_path: Path | None = None
    # False on a query-only run: with no targets there is nothing to decompile, and the decompile
    # post-script REFUSES an empty target list, which is correct there and would be a false alarm here.
    decompile_targets: bool = True
    reuse_project: bool = False
    log_lines: list[str] = field(default_factory=list)

    def __post_init__(self) -> None:
        """Every path is made ABSOLUTE once, here.

        Not tidiness. Ghidra is launched with ``cwd`` set to the project directory while the
        ``-import`` path and the ``-Djava.io.tmpdir`` it reads are built from these values, so a
        relative ``--out`` yields a relative ``java.io.tmpdir`` and the launcher dies with
        ``System property "java.io.tmpdir" is not an absolute path`` -- a refusal that names a JVM
        internal rather than the relative path the caller passed. Resolving once, at construction,
        is what stops any caller from reaching that.
        """
        self.interpreter = Path(self.interpreter).resolve()
        self.ghidra_home = Path(self.ghidra_home).resolve()
        self.project_dir = Path(self.project_dir).resolve()
        self.import_path = Path(self.import_path).resolve()
        self.output_dir = Path(self.output_dir).resolve()
        self.postscript = Path(self.postscript).resolve()
        if self.prescript is not None:
            self.prescript = Path(self.prescript).resolve()
        if self.queryscript is not None:
            self.queryscript = Path(self.queryscript).resolve()
        if self.queries_path is not None:
            self.queries_path = Path(self.queries_path).resolve()
        if self.reuse_project and self.queryscript is None:
            raise GhidraRefusal(
                "reuse_project asks Ghidra to open an existing program with -noanalysis, but no query "
                "script was given, so the run would open the project, do nothing and exit 0. A warm "
                "run with nothing to ask is a run that establishes nothing."
            )
        if self.reuse_project and self.prescript is not None:
            # A warm run must NOT seed anything, and the reason is a measured difference rather than
            # tidiness: the program is already imported, so disassembling from the entry again
            # OVERWRITES the analysis run's own record with a different number -- Spyro 2's entry
            # reaches 4,753 instructions before analysis and 11,674 after -- and both print as "the
            # pre-script seed". The record belongs to the run that analyzed; the pipeline replays it
            # with its provenance instead.
            pass
        if not self.reuse_project and self.prescript is None:
            raise GhidraRefusal(
                "an import with no pre-script cannot seed disassembly. Ghidra's Raw Binary loader "
                "defines no entry point, so MEASURED on a real image the first byte of the text "
                "window is DATA and auto-analysis then produces 0 functions while logging success."
            )

    def command(self) -> list[str]:
        words = [
            str(self.interpreter), "-m", LAUNCH_MODULE,
            "--install-dir", str(self.ghidra_home),
            # The heap ceiling. `-X mx<N>m` is the PyGhidra launcher's own flag, and it is the ONLY
            # one that works on this path: MAXMEM is read by Ghidra's launch.sh, which the PyGhidra
            # launcher does not go through, because it starts the JVM through JPype.
            "-X", "mx%dm" % self.max_heap_mb,
            HEADLESS_CLASS,
            str(self.project_dir), self.project_name,
        ]
        if self.reuse_project:
            # `-process` opens the imported program; `-noanalysis` is what makes this a QUERY rather
            # than a second analysis. Without `-noanalysis` Ghidra would re-run the analyzers on an
            # already-analyzed program and cost the same minutes the cache exists to avoid.
            words += ["-process", "-noanalysis"]
        else:
            words += [
                "-import", str(self.import_path),
                "-loader", "BinaryLoader",
                "-loader-baseAddr", "0x%08X" % self.base_address,
                "-processor", self.extra_language,
            ]
        words += ["-scriptPath", str(self.postscript.parent)]
        if self.prescript is not None and not self.reuse_project:
            words += ["-preScript", self.prescript.name, "0x%08X" % self.text_first,
                      "0x%08X" % self.text_last, "0x%08X" % self.entry_address]
        if self.decompile_targets:
            words += ["-postScript", self.postscript.name, str(self.output_dir / "inventory.json"),
                      str(self.output_dir / "c"), self.clear_noreturn,
                      str(self.output_dir / "targets.txt")]
        if self.queryscript is not None:
            if self.queries_path is None:
                raise GhidraRefusal(
                    "a query script was given with no query file, so it would be handed nothing to "
                    "answer. The questions are the input, not the script.")
            words += ["-postScript", self.queryscript.name, str(self.output_dir / "queries.json"),
                      str(self.queries_path)]
        return words

    def environment(self, base: dict[str, str] | None = None) -> dict[str, str]:
        env = dict(base if base is not None else os.environ)
        # Ghidra's cache must live in bounded project scratch, not a host-global tmpdir: a few hundred
        # MB of analysis cache on this machine's tmpfs is how an unrelated build gets OOM-killed.
        cache = self.project_dir / "ghidra-tmp"
        cache.mkdir(parents=True, exist_ok=True)
        env["JAVA_TOOL_OPTIONS"] = f"-Djava.io.tmpdir={cache}"
        # Where the pre-script records what it did, for the post-script to copy into the inventory
        # and the audit to check. Without it a pre-script failure is invisible: Ghidra raises, logs
        # "Post-analysis succeeded", and carries on.
        env["PSXPORT_DECOMP_PRESEED"] = str(self.output_dir / "preseed.json")
        # WHICH manifest entry and which kind, so the post-script can record it and the cross-check
        # can resolve the geometry. A module shares its parent title's SERIAL, so the serial alone
        # cannot tell BATTLE.PRG from SLUS_010.40, and they sit 0x48800 apart.
        if self.image_name:
            env["PSXPORT_DECOMP_IMAGE_NAME"] = self.image_name
        if self.image_kind:
            env["PSXPORT_DECOMP_IMAGE_KIND"] = self.image_kind
        return env

    def prepare(self) -> None:
        """Create the directories Ghidra needs BEFORE launching it.

        The project directory must exist: Ghidra's "Directory not found" abort is otherwise the
        whole of the error, and it is not a message anyone can act on.
        """
        self.project_dir.mkdir(parents=True, exist_ok=True)
        self.output_dir.mkdir(parents=True, exist_ok=True)
        (self.output_dir / "c").mkdir(parents=True, exist_ok=True)
        (self.project_dir / "ghidra-tmp").mkdir(parents=True, exist_ok=True)

    def run(self, runner: Runner) -> str:
        """Invoke Ghidra and REFUSE unless it ran, succeeded, and left its inventory behind.

        The last clause is the one that matters. `analyzeHeadless` exits 0 and logs "Post-analysis
        succeeded" while never running the script at all, so exit code alone is not evidence that
        anything happened. The inventory file is.

        The whole Ghidra output is WRITTEN to ``ghidra.log`` before anything is judged. Measured: a
        failed run reported only its last 14 lines, which named neither the missing ``__main__`` call
        nor the post-script's own error -- a diagnostic that throws away the evidence for its own
        failure is the defect this tool exists to prevent, committed one level down.
        """
        self.prepare()
        code, output = runner.run(self.command(), self.project_dir, self.timeout_seconds,
                                  self.environment())
        self.log_lines.append(output)
        (self.output_dir / "ghidra.log").write_text(output, encoding="utf-8", errors="replace")
        log_path = self.output_dir / "ghidra.log"
        if code != 0:
            raise GhidraRefusal(
                f"Ghidra exited {code}. The full log is at {log_path}. Last lines:\n"
                + "\n".join(output.strip().splitlines()[-14:]))
        # EVERY artefact this run was asked to produce must be on disk. `analyzeHeadless` exits 0 and
        # logs "Post-analysis succeeded" while never running a script at all, so the exit code is not
        # evidence that anything happened -- the files are. Checked per artefact rather than once,
        # because a run can be asked for both, and a decompile that landed while the query script did
        # not run is a run that half-succeeded and looks complete.
        for artefact, description in self.expected_artifacts():
            if not artefact.is_file():
                raise GhidraRefusal(
                    f"Ghidra exited 0 but wrote no {description} at {artefact}, so that part of the "
                    "run did NOTHING. This is measured behaviour when a Python post-script does not "
                    "run under PyGhidra: the analyzer logs 'Post-analysis succeeded' and exits 0. The "
                    f"full log is at {log_path}; look for the script's own line, and for whether the "
                    "script has an `if __name__ == \"__main__\"` entry point at all. Last lines:\\n"
                    + "\\n".join(output.strip().splitlines()[-14:])
                )
        return output

    def expected_artifacts(self) -> list[tuple[Path, str]]:
        """``(path, what it would mean)`` for everything this run promised to write."""
        expected: list[tuple[Path, str]] = []
        if self.decompile_targets:
            expected.append((self.output_dir / "inventory.json", "inventory"))
        if self.queryscript is not None:
            expected.append((self.output_dir / "queries.json", "query answers"))
        if not expected:
            raise GhidraRefusal(
                "this run was asked to write nothing: no --target to decompile and no query to "
                "answer. A Ghidra launch with nothing to produce exits 0 having established nothing, "
                "and the caller would read that as a successful run."
            )
        return expected


def resolve_interpreter(ghidra_home: Path) -> Path:
    """The python that drives PyGhidra, or REFUSE.

    Ghidra's own venv first: it is built against this exact install. A machine that never built it
    has no working PyGhidra, and saying "PyGhidra is not installed" there is a fact about the
    install rather than about whichever python happens to be running the tool.
    """
    candidates = [
        ghidra_home / "Ghidra" / "Features" / "PyGhidra" / "venv" / "bin" / "python3",
        Path.home() / ".config" / "ghidra" / ghidra_home.name / "venv" / "bin" / "python3",
    ]
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    probe = subprocess.run([sys.executable, "-c", "import pyghidra"], capture_output=True, text=True,
                           check=False)
    if probe.returncode == 0:
        return Path(sys.executable)
    raise GhidraRefusal(
        f"no PyGhidra interpreter found. Looked for "
        + ", ".join(str(c) for c in candidates)
        + f", and {sys.executable} cannot `import pyghidra` either. Ghidra 12 runs a Python "
        "post-script ONLY under PyGhidra; without it a run exits 0 having decompiled nothing."
    )


def check_installation(ghidra_home: Path, interpreter: Path, runner: Runner) -> None:
    """Refuse early, by name, when Ghidra or its PyGhidra bridge is not usable.

    Run before the lock is taken and before anything is written: PyGhidra's own failure for a wrong
    install directory is a stack trace inside a log file that nobody reads.
    """
    if not ghidra_home.is_dir():
        raise GhidraRefusal(
            f"no Ghidra installation at {ghidra_home}. Pass --ghidra-home, or install Ghidra 12. "
            "Refusing rather than launching something that will fail inside a log."
        )
    if not (ghidra_home / "Ghidra").is_dir():
        raise GhidraRefusal(
            f"{ghidra_home} has no Ghidra/ subdirectory, so it is not a Ghidra install root. "
            "Point --ghidra-home at the directory that CONTAINS Ghidra/."
        )
    code, output = runner.run([str(interpreter), "-c", "import pyghidra"], Path.cwd(), 120,
                              dict(os.environ))
    if code != 0:
        raise GhidraRefusal(
            f"{interpreter} cannot `import pyghidra`, so Ghidra 12 cannot run a Python post-script "
            f"there: PyGhidra is what provides Python to Ghidra, and without it a run exits 0 "
            f"having decompiled nothing. Reported from the interpreter that would actually launch "
            f"Ghidra, not from {sys.executable}, because 'absent' and 'absent from here' are "
            f"different facts. Probe said: {output.strip()[:400]}"
        )
