#!/usr/bin/env python3
"""Drivable core sessions for the state-aligned oracle comparison (docs/oracle.md, compare.py).

Two cores, one narrow interface: a Lightrec product driven through the framework REPL over pipes,
and the independent Beetle full-console reference driven through `tools/oracle/console.py`'s JSON
protocol. A title's comparison policy drives either without knowing which it is: `hold(buttons)`,
`step(frames)`, `read(address, size)`.

Both sessions refuse rather than guess: a REPL reply that does not arrive, a button name the REPL
mapped to the wrong bit, a console reply without `ok`, or a read outside the reference's main RAM
each raise `CoreError` with what was seen.
"""

from __future__ import annotations

import json
import os
import queue
import struct
import sys
import shutil
import subprocess
import threading
import time
from pathlib import Path
from typing import Protocol

# The PSX digital pad bit order lives in tools/psx_pad.py, with the .pad replay format that uses the
# same table. It was duplicated here and in a consumer's replay decoder; an incomplete copy of a bit
# table drops input silently, so there is one.
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from psx_pad import PSX_BUTTON_BITS  # noqa: E402


class CoreError(RuntimeError):
    """A core did not do what the driver asked, with the evidence seen."""


class CoreSession(Protocol):
    name: str
    frames: int          # steps taken so far, in this session's own unit
    reference: bool      # True for the independent console reference, False for the product

    @property
    def held(self) -> frozenset[str]: ...
    def hold(self, buttons: frozenset[str]) -> None: ...
    def step(self, frames: int) -> None: ...
    def read(self, address: int, size: int) -> bytes: ...
    # The picture this core PRESENTS, written to `destination`. Not the emulated VRAM: on a native
    # render path the picture is never in VRAM, and comparing VRAM there compares two blank buffers
    # and calls them equal (psxport issue 0121).
    def capture(self, destination: Path) -> None: ...
    def close(self) -> None: ...


def _validate_buttons(buttons: frozenset[str]) -> None:
    unknown = sorted(buttons - PSX_BUTTON_BITS.keys())
    if unknown:
        raise CoreError(f"unknown pad button name(s) {unknown}; allowed: {sorted(PSX_BUTTON_BITS)}")


class NativeReplSession:
    """A built product binary, driven through PSXPORT_REPL=1 over stdin with its lucent log on
    stderr. Each REPL command is followed by a read whose echo is the barrier, so `step` returns
    only once the requested frames have run."""

    name = "native"
    reference = False  # the title's frame driver decides what one REPL step runs (a frame or a field)
    _REPLY_TIMEOUT = 180.0
    _WORDS_PER_READ = 64  # the REPL's `rw` prints at most 64 words
    _BARRIER_ADDRESS = 0x80000000  # any readable main-RAM word serves as the post-step reply barrier

    def __init__(self, binary: str, executable: str, environment: dict, cwd: str, log_path: Path):
        self.frames = 0
        self._held: frozenset[str] = frozenset()
        self.scan_rows: int | None = None  # set by capture(): rows a console would scan out
        self._log = open(log_path, "w")
        self._lines: queue.Queue[str | None] = queue.Queue()
        self._process = subprocess.Popen(
            [binary, executable], stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE, env=environment, cwd=cwd, text=True, bufsize=1)
        self._reader = threading.Thread(target=self._pump, daemon=True)
        self._reader.start()

    def _pump(self) -> None:
        assert self._process.stderr is not None
        for line in self._process.stderr:
            self._log.write(line)
            self._lines.put(line.rstrip("\n"))
        self._lines.put(None)

    def _send(self, command: str) -> None:
        assert self._process.stdin is not None
        self._process.stdin.write(command + "\n")
        self._process.stdin.flush()

    def _expect(self, marker: str) -> str:
        deadline = time.monotonic() + self._REPLY_TIMEOUT
        seen: list[str] = []
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                tail = "\n".join(seen[-5:])
                raise CoreError(f"native REPL: no line containing {marker!r} within "
                                f"{self._REPLY_TIMEOUT:.0f}s ({len(seen)} lines seen); last lines:\n{tail}")
            try:
                line = self._lines.get(timeout=remaining)
            except queue.Empty:
                continue
            if line is None:
                raise CoreError(f"native REPL exited (code {self._process.poll()}) while waiting for "
                                f"{marker!r}; see the native log")
            seen.append(line)
            if marker in line:
                return line

    @property
    def held(self) -> frozenset[str]:
        return self._held

    def hold(self, buttons: frozenset[str]) -> None:
        _validate_buttons(buttons)
        echo = ""
        for name in sorted(self._held - buttons):
            self._send(f"release {name}")
            echo = self._expect("held=")
        for name in sorted(buttons - self._held):
            self._send(f"press {name}")
            echo = self._expect("held=")
        self._held = buttons
        if echo:
            expected = 0xFFFF & ~sum(PSX_BUTTON_BITS[name] for name in buttons)
            held = int(echo.rsplit("held=", 1)[1][:4], 16)
            if held != expected:
                raise CoreError(f"native REPL held mask {held:04X} after holding {sorted(buttons)}, "
                                f"expected {expected:04X}: a button name mapped to the wrong bit")

    def step(self, frames: int) -> None:
        if frames <= 0:
            raise CoreError(f"step needs a positive frame count, got {frames}")
        self._send(f"run {frames}")
        self.read(self._BARRIER_ADDRESS, 4)  # the reply is the barrier: it prints only after the frames ran
        self.frames += frames

    def read(self, address: int, size: int) -> bytes:
        if size <= 0:
            raise CoreError(f"read needs a positive size, got {size}")
        out = bytearray()
        cursor = address & ~3
        end = address + size
        while cursor < end:
            words = min(self._WORDS_PER_READ, (end - cursor + 3) // 4)
            self._send(f"rw {cursor:08X} {words}")
            line = self._expect(f"{cursor:08X}:")
            values = line.rsplit(f"{cursor:08X}:", 1)[1].split()
            if len(values) != words:
                raise CoreError(f"native REPL printed {len(values)} words for rw {cursor:08X} {words}")
            out += b"".join(struct.pack("<I", int(value, 16)) for value in values)
            cursor += words * 4
        skip = address & 3
        return bytes(out[skip:skip + size])

    def write8(self, address: int, value: int) -> None:
        self._send(f"w8 {address:08X} {value & 0xFF:02X}")
        self._expect("[repl] ok")

    def capture(self, destination: Path) -> None:
        """The REPL's `shot`, which routes through gpu_native_shot and so follows whichever render
        path is active and the wide presentation region when widescreen is on. A shot the product
        reported but did not write is refused here rather than compared as an old file.

        The reply carries `guest_scan=<rows>`: how many of the captured rows a console would scan
        out, which the native path may exceed deliberately. It is recorded in `scan_rows` for a
        picture comparison to crop by, so that alignment comes from the GPU state rather than from
        fitting an offset to the pixels being compared."""
        destination.parent.mkdir(parents=True, exist_ok=True)
        if destination.exists():
            destination.unlink()
        self._send(f"shot {destination}")
        reply = self._expect("shot (")
        if not destination.is_file():
            raise CoreError(f"native REPL reported a shot but {destination} was not written")
        marker = "guest_scan="
        if marker not in reply:
            raise CoreError(f"native REPL's shot reply did not state guest_scan=; this build predates "
                            f"the scanned-row report, so a picture comparison cannot align: {reply.strip()}")
        self.scan_rows = int(reply.rsplit(marker, 1)[1].split()[0])

    def close(self) -> None:
        if self._process.poll() is None:
            try:
                self._send("quit")
                self._process.wait(timeout=60)
            except (OSError, subprocess.TimeoutExpired):
                self._process.kill()
        self._log.close()


class ConsoleSession:
    """psxport's Beetle full-console reference (`tools/oracle/console.py run`), one JSON command per
    line on stdin and one JSON reply per line on stdout."""

    name = "console"
    reference = True  # Beetle steps by VBlank; the title decides what a game frame is
    _MAX_STEP = 3600
    _MAX_READ = 256

    def __init__(self, psxport_dir: Path, disc: Path, bios: Path, region: str, log_path: Path,
                 crop_overscan: bool = False):
        self.frames = 0
        self._log = open(log_path, "w")
        command = ["uv", "run", "--frozen", "python", "tools/oracle/console.py", "run",
                   "--disc", str(disc), "--region", region, "--bios", str(bios)]
        if crop_overscan:
            # Picture comparison only: the reference then publishes its active display area rather
            # than the padded scanline, so both cores present the same rect. RAM is unaffected, and
            # the RAM comparison keeps the pinned contract by not passing this.
            command.append("--crop-overscan")
        environment = {key: value for key, value in os.environ.items() if key != "VIRTUAL_ENV"}
        self._held: frozenset[str] = frozenset()
        self._process = subprocess.Popen(command, cwd=psxport_dir, stdin=subprocess.PIPE, env=environment,
                                         stdout=subprocess.PIPE, stderr=self._log, text=True, bufsize=1)
        ready = self._receive()
        if not ready.get("ready"):
            raise CoreError(f"console reference did not report ready: {ready}")
        self.manifest = ready["manifest"]

    def _receive(self) -> dict:
        assert self._process.stdout is not None
        line = self._process.stdout.readline()
        if not line:
            raise CoreError(f"console reference exited (code {self._process.poll()}); see its log")
        reply = json.loads(line)
        if "error" in reply:
            raise CoreError(f"console reference refused: {reply['error']}")
        return reply

    def _call(self, message: dict) -> dict:
        assert self._process.stdin is not None
        self._process.stdin.write(json.dumps(message) + "\n")
        self._process.stdin.flush()
        reply = self._receive()
        if not reply.get("ok"):
            raise CoreError(f"console reference replied without ok to {message}: {reply}")
        return reply["result"]

    @property
    def held(self) -> frozenset[str]:
        return self._held

    def hold(self, buttons: frozenset[str]) -> None:
        _validate_buttons(buttons)
        if buttons != self._held:
            self._call({"command": "buttons", "buttons": sorted(buttons)})
            self._held = buttons

    def insert_card(self, card: Path) -> dict:
        """Start this reference from the memory-card image at `card`. See ConsoleSession.insert_card
        in tools/oracle/console_session.py for why card state is an input that must be equal. The
        path is sent, not the bytes: the protocol bounds a command line well below a 128 KiB card."""
        return self._call({"command": "insert_card", "card": str(card.resolve())})

    def step(self, frames: int) -> None:
        if frames <= 0:
            raise CoreError(f"step needs a positive frame count, got {frames}")
        remaining = frames
        while remaining:
            chunk = min(self._MAX_STEP, remaining)
            status = self._call({"command": "step", "frames": chunk})
            remaining -= chunk
        self.frames = status["frames"]

    def read(self, address: int, size: int) -> bytes:
        out = bytearray()
        cursor = address
        while cursor < address + size:
            chunk = min(self._MAX_READ, address + size - cursor)
            result = self._call({"command": "read", "address": f"0x{cursor:08X}", "bytes": chunk})
            out += bytes.fromhex(result["bytes"])
            cursor += chunk
        return bytes(out)

    def capture(self, destination: Path) -> None:
        """The reference's own framebuffer. console.py writes it to one fixed path and overwrites it
        every time, so it is copied out immediately."""
        destination.parent.mkdir(parents=True, exist_ok=True)
        result = self._call({"command": "capture"})
        shutil.copyfile(result["capture"], destination)

    def close(self) -> None:
        if self._process.poll() is None:
            try:
                self._call({"command": "quit"})
                self._process.wait(timeout=60)
            except (CoreError, OSError, subprocess.TimeoutExpired):
                self._process.kill()
        self._log.close()
