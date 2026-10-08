"""The one-Ghidra-at-a-time lock.

ONE CONCEPT: this machine cannot afford two Ghidra auto-analyses at once. A full analysis of a PSX
RAM dump costs 1-2 GB, this machine has roughly 2 GB free, and several agents build here
concurrently. A build OOM-killed by someone else's analysis is reported to its owner as a FALSE RED,
so the cost of failing to coordinate is paid by somebody who is not even running this tool.

The lock is ``mkdir``, because ``mkdir`` is atomic on POSIX. There is no daemon, no lease, no race:
exactly one holder can create the directory, and the loser waits and retries. That is the same lock
the workspace's area claims use, applied to a scarce runtime rather than to a file.

It is released in a ``finally``, including on an exception, so a refusal never strands the machine
until someone notices by hand.
"""

from __future__ import annotations

import errno
import os
import time
from dataclasses import dataclass
from pathlib import Path

DEFAULT_DIR_NAME = Path("coord") / "locks" / "ghidra"


class LockRefusal(Exception):
    """The lock could not be taken in the time allowed. Raised, never ignored."""


@dataclass
class GhidraLock:
    """A held, exclusive claim on the Ghidra slot.

    Use as a context manager. ``wait_seconds`` bounds the wait so a stranded lock reports itself
    instead of hanging a caller until it is killed.
    """

    directory: Path
    wait_seconds: float = 1800.0
    poll_seconds: float = 5.0
    holder_note: str = ""
    attempts: int = 0
    waited_seconds: float = 0.0
    acquired: bool = False

    def acquire(self) -> "GhidraLock":
        deadline = time.monotonic() + self.wait_seconds
        self.directory.parent.mkdir(parents=True, exist_ok=True)
        while True:
            self.attempts += 1
            try:
                self.directory.mkdir()
                self.acquired = True
                self._stamp()
                return self
            except OSError as error:
                # EEXIST is the contended case and falls through to the wait below. Every other
                # errno is a real filesystem failure and is refused rather than retried, because
                # retrying a permission error until the deadline reports a machine fault as a busy
                # lock. NOTE: there is deliberately no separate `except FileExistsError` clause --
                # it would be dead, since this branch already treats EEXIST as "contended", and a
                # seed that removed the dead clause used to leave the selftest GREEN, which is how
                # that redundancy was found.
                if error.errno != errno.EEXIST:
                    raise LockRefusal(
                        f"cannot create the Ghidra lock at {self.directory}: {error}. Refusing "
                        "rather than starting an analysis with no lock, because that is how a "
                        "co-tenant's build gets OOM-killed."
                    ) from error
            if time.monotonic() >= deadline:
                holder = self._read_holder()
                raise LockRefusal(
                    f"another Ghidra run has held {self.directory} for longer than "
                    f"{self.wait_seconds:.0f} s ({self.attempts} attempts). Holder: {holder}. This "
                    "machine cannot run two analyses at once, so waiting is correct and starting "
                    "one anyway is not. Re-run later, or raise --lock-wait."
                )
            time.sleep(self.poll_seconds)
            self.waited_seconds += self.poll_seconds

    def release(self) -> None:
        if not self.acquired:
            return
        note = self.directory / "holder.txt"
        if note.exists():
            note.unlink()
        try:
            self.directory.rmdir()
        except OSError as error:  # pragma: no cover - a foreign file in the lock directory
            raise LockRefusal(
                f"released the Ghidra lock at {self.directory} but could not remove it: {error}. The "
                "next run will wait on a lock nobody holds; remove that directory by hand."
            ) from error
        self.acquired = False

    def __enter__(self) -> "GhidraLock":
        return self.acquire()

    def __exit__(self, exception_type, exception, traceback) -> bool:
        # Released on the exception path too. A pipeline that refuses half way through must not
        # strand the machine's only Ghidra slot until somebody notices by hand.
        self.release()
        return False

    def _stamp(self) -> None:
        """Name the holder, so a stranded lock says WHO and WHEN rather than only existing."""
        (self.directory / "holder.txt").write_text(
            "pid=%d\nsince=%s\nnote=%s\n" % (os.getpid(), time.strftime("%Y-%m-%dT%H:%M:%S"),
                                            self.holder_note or "-")
        )

    def _read_holder(self) -> str:
        note = self.directory / "holder.txt"
        try:
            return note.read_text().strip().replace("\n", " ") or "no holder note"
        except OSError:
            return "no holder note"


def default_lock_dir(workspace: Path | None = None) -> Path:
    """Where the lock lives when the caller does not say.

    The workspace convention is ``<workspace>/coord/locks/ghidra``: ``coord/`` is untracked and
    machine-local, which is exactly right for a lock that coordinates agents on THIS machine, and it
    is the SAME directory the area claims use, so one `ls coord/locks` shows every scarce resource
    currently held. A lock inside a repository would not coordinate anything, because every title
    has its own repository and the whole point is to serialise ACROSS them.

    MEASURED: an earlier revision of this dropped the ``coord/`` component and created
    ``~/repo/psx/locks/ghidra``. That is a lock which coordinates nobody -- the operator's stated
    protocol directory stayed empty while this one was held -- so the path is asserted by the
    selftest rather than left to be re-derived.
    """
    root = Path(workspace) if workspace else Path.home() / "repo" / "psx"
    return Path(root) / DEFAULT_DIR_NAME
