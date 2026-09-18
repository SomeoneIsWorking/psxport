#!/usr/bin/env python3
"""State-aligned comparison of a Lightrec product against the Beetle full-console reference
(docs/oracle.md "State-aligned comparison").

The framework owns the mechanism: both cores boot the same disc; each is driven toward the next
title-declared checkpoint by the title's own input policy; the first arrival parks; once both have
arrived the title's declared main-RAM ranges are compared and every excluded field is named. After
the last checkpoint both cores receive the title's held-input schedule for identical game-frame
counts and are compared after every segment (or every N frames). The report names both cores' frame
counts, every range with its byte count, the first differing byte, and the identities of the binary,
disc, firmware and reference build.

The title owns what a game frame is (`Title.advance` steps the VBlank-driven console until its guest
main loop has run exactly one frame), which state is compared, why the rest is excluded, how each
checkpoint is reached, and the representative gameplay. A title module satisfies `Title`; its thin
`tools/oracle_compare.py` builds a `Product` and calls `run`.

`--selftest` flips one product byte at the first checkpoint and requires the comparator to report
exactly that divergence; a comparator that has never shown a difference is not trusted.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable, Optional, Protocol, Sequence

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from compare_cores import ConsoleSession, CoreError, CoreSession, NativeReplSession  # noqa: E402

PSXPORT = Path(__file__).resolve().parents[2]


@dataclass(frozen=True)
class DeclaredRange:
    name: str
    address: int
    size: int
    decisive: bool  # a decisive mismatch is a divergence; an informational one is reported only

    @property
    def end(self) -> int:
        return self.address + self.size


@dataclass(frozen=True)
class SelftestSeed:
    """One product byte the selftest flips, and the declared range/offset that must report it."""
    address: int
    range: str
    offset: int


Settle = Optional[frozenset[str]]
Pattern = Callable[[int, Any], frozenset[str]]  # (frame index, title observation) -> held pad
Arrived = Callable[[Any], bool]
Reach = Callable[["Driver", CoreSession, int, Settle], tuple[int, frozenset[str]]]


@dataclass(frozen=True)
class Checkpoint:
    name: str
    reach: Reach  # drive one core to this checkpoint: (driver, core, budget, settle) -> (used, settle)


class Title(Protocol):
    """What a title module provides. Predicates read main RAM only: the console reference cannot
    read the scratchpad."""
    name: str
    declared: Sequence[DeclaredRange]
    excluded: dict[str, str]
    checkpoints: Sequence[Checkpoint]
    gameplay: Sequence[tuple[frozenset[str], int]]  # held buttons and game-frame counts
    selftest: SelftestSeed

    def advance(self, core: CoreSession, frames: int, strict: bool = False) -> None: ...
    # Game frames between a hold committed at this core's park point and the frame whose input
    # read sees it: 0 when the guest samples the pad after the park, 1 when it sampled it before.
    def lookahead(self, core: CoreSession) -> int: ...
    def observe(self, core: CoreSession) -> Any: ...
    def summary(self, core: CoreSession) -> dict: ...


@dataclass(frozen=True)
class Product:
    """The built product and how to launch it headless under the REPL."""
    binary: Path
    executable: Path
    environment: dict[str, str]
    cwd: Path
    disc: Path


CARD_ENV = "PSXPORT_CARD"  # the framework memory-card path key every consumer honours (memcard.cpp)


def fresh_card(product: Product, out_dir: Path) -> Product:
    """Point the product at a card image that does not exist yet, so it formats a blank card the way
    the console reference starts with one. A persistent card with a save changes the title's menu
    route (measured: Spyro's picker defaults to LOAD GAME over a saved slot), which is an
    environment difference, not a product divergence. A title-specific card key in the product's
    environment or .env still takes precedence in memcard.cpp; a title tool that has one passes it
    through --product-env at this same path."""
    card = out_dir / "card.mcr"
    card.unlink(missing_ok=True)
    environment = dict(product.environment)
    environment[CARD_ENV] = str(card)
    return Product(product.binary, product.executable, environment, product.cwd, product.disc)


class Unreached(CoreError):
    """A checkpoint predicate never held within its frame budget."""


def u32(core: CoreSession, address: int) -> int:
    return int.from_bytes(core.read(address, 4), "little")


def step_until_counter_resets(core: CoreSession, address: int) -> int:
    """Step a field-driven core one step at a time until the guest's per-field counter at
    `address` decreases, i.e. the guest main loop passed the point that resets it. Returns the
    number of steps taken. A title's `advance` builds its game-frame barrier on this for the
    console and for a product whose REPL step is one field."""
    previous = u32(core, address)
    stepped = 0
    while True:
        core.step(1)
        stepped += 1
        current = u32(core, address)
        if current < previous:
            return stepped
        previous = current


def tap(button: str, period: int, width: int = 6) -> Pattern:
    """Hold `button` for the first `width` frames of every `period` frames."""
    return lambda frame, seen: frozenset({button}) if frame % period < width else frozenset()


def released(frame: int, seen: Any) -> frozenset[str]:
    return frozenset()


class Driver:
    """Per-core input delivery for one title: the same pattern reaches the same game frame's
    input read on the product and on the console."""

    def __init__(self, title: Title):
        self.title = title

    def lookahead(self, core: CoreSession) -> int:
        return self.title.lookahead(core)

    def drive(self, core: CoreSession, budget: int, arrived: Arrived, pattern: Pattern, goal: str,
              settle: Settle) -> tuple[int, frozenset[str]]:
        """Advance one game frame at a time, holding `pattern(frame, observation)` for each frame's
        input read, until `arrived(observation)` holds; then run one settle frame reading the
        settle pad, release, and park.

        The settle frame reads the pattern for the arrival frame index on both cores. A core
        with lookahead committed that pad one frame earlier, so at arrival it commits the released
        pad the first post-park frame will read; a core without lookahead holds the settle pad for
        the settle frame and releases after it. The reference reports the pad its settle frame
        reads; the product, driven after it, is given that pad so both park one frame past the
        transition in the same state. Returns (frames used, settle pad). A pattern that depends on
        the observation sees it `lookahead` frames stale; the decisive pad ranges catch a resulting
        delivery difference."""
        lookahead = self.lookahead(core)
        for frame in range(budget):
            seen = self.title.observe(core)
            if arrived(seen):
                if core.reference:
                    settle = core.held if lookahead else pattern(frame, seen)
                elif settle is None:
                    raise ValueError(f"{core.name}: drive the console first; its arrival pad is the settle pad")
                core.hold(frozenset() if lookahead else settle)
                self.title.advance(core, 1)
                core.hold(frozenset())
                return frame + 1, settle
            core.hold(pattern(frame + lookahead, seen))
            self.title.advance(core, 1)
        seen = self.title.observe(core)
        raise Unreached(f"{core.name}: {goal} not reached within {budget} game frames; last {seen}")


class Playback:
    """Feeds one core a known per-frame button schedule so that game frame K's input read sees
    schedule[K] on both core kinds. The core must be parked with schedule[0] already held
    (checkpoint arrival releases the pad)."""

    def __init__(self, driver: Driver, core: CoreSession, schedule: Sequence[frozenset[str]]):
        if not schedule:
            raise ValueError("empty schedule")
        if core.held != schedule[0]:
            raise CoreError(f"{core.name}: parked holding {sorted(core.held)} but the schedule starts "
                            f"with {sorted(schedule[0])}; the first scheduled frame must repeat the parked pad")
        self.driver = driver
        self.core = core
        self.schedule = schedule
        self.frame = 0

    def step(self) -> None:
        """Run the next scheduled game frame."""
        if self.frame >= len(self.schedule):
            raise IndexError("schedule exhausted")
        index = min(self.frame + self.driver.lookahead(self.core), len(self.schedule) - 1)
        self.core.hold(self.schedule[index])
        self.driver.title.advance(self.core, 1, strict=True)
        self.frame += 1


def snapshot(title: Title, core: CoreSession) -> dict[str, bytes]:
    return {declared.name: core.read(declared.address, declared.size) for declared in title.declared}


def compare(title: Title, native: dict[str, bytes], console: dict[str, bytes]) -> list[dict]:
    """One row per declared range: byte count and the first differing offset, if any."""
    rows = []
    for declared in title.declared:
        a, b = native[declared.name], console[declared.name]
        first = next((i for i in range(declared.size) if a[i] != b[i]), None)
        row = {"range": declared.name, "address": f"0x{declared.address:08X}", "bytes": declared.size,
               "decisive": declared.decisive, "equal": first is None,
               "native_hex": a.hex(), "console_hex": b.hex()}
        if first is not None:
            row.update({"first_diff_offset": first, "native_byte": a[first], "console_byte": b[first],
                        "differing_bytes": sum(1 for i in range(declared.size) if a[i] != b[i])})
        rows.append(row)
    return rows


def binary_identity(binary: Path) -> dict:
    """md5 + mtime of the binary about to run: mtime alone cannot tell a rebuild of the same
    sources from a rebuild of different ones."""
    digest = hashlib.md5()
    try:
        with open(binary, "rb") as handle:
            for chunk in iter(lambda: handle.read(1 << 20), b""):
                digest.update(chunk)
        return {"md5": digest.hexdigest(),
                "mtime": time.strftime("%Y-%m-%dT%H:%M:%S", time.localtime(os.path.getmtime(binary)))}
    except OSError as error:
        return {"md5": f"UNREADABLE({error.__class__.__name__})", "mtime": ""}


class Comparison:
    """One comparison run: two parked cores, a title, and the report being built."""

    def __init__(self, title: Title, native: CoreSession, console: CoreSession, report: dict):
        self.title = title
        self.driver = Driver(title)
        self.native = native
        self.console = console
        self.report = report

    def checkpoint(self, name: str) -> bool:
        """Snapshot both parked cores, compare, record; return whether every decisive range matched."""
        rows = compare(self.title, snapshot(self.title, self.native), snapshot(self.title, self.console))
        native_summary = self.title.summary(self.native)
        console_summary = self.title.summary(self.console)
        self.report["checkpoints"].append({
            "checkpoint": name, "native_frames": self.native.frames, "console_vblanks": self.console.frames,
            "native": native_summary, "console": console_summary, "ranges": rows})
        ok = all(row["equal"] for row in rows if row["decisive"])
        print(f"[oracle] {name}: {'MATCH' if ok else 'DIVERGE'} native f{self.native.frames} {native_summary} | "
              f"console vb{self.console.frames} {console_summary}")
        for row in rows:
            if not row["equal"]:
                kind = "decisive" if row["decisive"] else "informational"
                print(f"[oracle]   {row['range']} ({kind}): first diff at +{row['first_diff_offset']} "
                      f"native {row['native_byte']:02X} console {row['console_byte']:02X}, "
                      f"{row['differing_bytes']}/{row['bytes']} bytes differ"
                      + (f" [native {row['native_hex']} console {row['console_hex']}]" if row["decisive"] else ""))
        return ok

    def reach(self, checkpoint: Checkpoint, budget: int) -> None:
        """Drive the console first at every checkpoint: its arrival frame fixes the settle pad the
        product's arrival frame must read (Driver.drive)."""
        settle: Settle = None
        for core in (self.console, self.native):
            used, settle = checkpoint.reach(self.driver, core, budget, settle)
            print(f"[oracle] {core.name}: {checkpoint.name} after {used} game frames")

    def selftest(self) -> bool:
        """Seed the title's selftest byte on the product and require exactly that divergence."""
        seed = self.title.selftest
        original = self.native.read(seed.address, 1)[0]
        self.native.write8(seed.address, original ^ 0x5A)
        rows = compare(self.title, snapshot(self.title, self.native), snapshot(self.title, self.console))
        row = {r["range"]: r for r in rows}[seed.range]
        hit = (not row["equal"]) and row["first_diff_offset"] == seed.offset
        self.report["selftest"] = {"seeded_address": f"0x{seed.address:08X}", "detected": hit, "ranges": rows}
        print(f"[oracle] selftest: seeded byte at 0x{seed.address:08X}; comparator "
              f"{'DETECTED it' if hit else 'MISSED it'}")
        return hit

    def gameplay(self, frame_step: int) -> bool:
        """Feed both cores the title's held-input segments frame by frame and compare after every
        `frame_step` frames of a segment (once per segment when 0); stop at the first decisive
        divergence."""
        schedule = [buttons for buttons, frames in self.title.gameplay for _ in range(frames)]
        players = [Playback(self.driver, core, schedule) for core in (self.native, self.console)]
        for index, (buttons, frames) in enumerate(self.title.gameplay):
            for done in range(1, frames + 1):
                for player in players:
                    player.step()
                if done == frames or (frame_step > 0 and done % frame_step == 0):
                    label = f"gameplay[{index}] hold {sorted(buttons) or 'nothing'} {done}/{frames}f"
                    if not self.checkpoint(label):
                        return False
        return True


SessionFactory = Callable[[Product, argparse.Namespace, Path], tuple[CoreSession, CoreSession]]


def launch_sessions(product: Product, args: argparse.Namespace, out_dir: Path) -> tuple[CoreSession, CoreSession]:
    native = NativeReplSession(str(product.binary), str(product.executable), product.environment,
                               str(product.cwd), out_dir / "native.log")
    console = ConsoleSession(PSXPORT, product.disc, args.bios, args.region, out_dir / "console.log")
    return native, console


def run(title: Title, product: Product, args: argparse.Namespace, out_dir: Path,
        sessions: SessionFactory = launch_sessions) -> int:
    out_dir.mkdir(parents=True, exist_ok=True)
    if not args.bios.is_file():
        print(f"REFUSED: BIOS image {args.bios} does not exist; pass --bios <SCPH1001.BIN>", file=sys.stderr)
        return 2
    if not product.binary.is_file() or not product.executable.is_file():
        print(f"REFUSED: {product.binary} or {product.executable} is missing; build the product first",
              file=sys.stderr)
        return 2
    if not title.checkpoints:
        print(f"REFUSED: {title.name} declares no checkpoints; a comparison needs at least one", file=sys.stderr)
        return 2
    report = {"title": title.name, "binary": binary_identity(product.binary), "disc": str(product.disc),
              "bios": str(args.bios), "product_env": dict(getattr(args, "product_env_pairs", {})),
              "lookahead": {},
              "declared": [{"range": d.name, "address": f"0x{d.address:08X}", "bytes": d.size,
                            "decisive": d.decisive} for d in title.declared],
              "excluded": dict(title.excluded), "checkpoints": [], "complete": False}
    started = time.monotonic()
    native = console = None
    exit_code = 1
    try:
        product = fresh_card(product, out_dir)
        report["product_card"] = product.environment[CARD_ENV]
        native, console = sessions(product, args, out_dir)
        report["console_manifest"] = getattr(console, "manifest", None)
        existing = (report["console_manifest"] or {}).get("existing_save_files", [])
        if existing:
            raise CoreError(f"the console reference starts with existing save files {existing}; the product "
                            f"starts with a blank card, so the title's menu route would differ. Remove them "
                            f"from the reference's saves directory (see its manifest) and rerun")
        report["lookahead"] = {core.name: title.lookahead(core) for core in (native, console)}
        comparison = Comparison(title, native, console, report)
        first, *rest = title.checkpoints
        comparison.reach(first, args.budget)
        ok = comparison.checkpoint(first.name)
        if args.selftest:
            exit_code = 0 if comparison.selftest() else 1
            report["complete"] = True
            return exit_code
        for checkpoint in rest:
            comparison.reach(checkpoint, args.budget)
            ok = comparison.checkpoint(checkpoint.name) and ok
        ok = comparison.gameplay(args.frame_step) and ok
        report["complete"] = True
        exit_code = 0 if ok else 1
        return exit_code
    except CoreError as error:
        report["error"] = str(error)
        print(f"[oracle] FAILED: {error}", file=sys.stderr)
        return 1
    finally:
        for core in (native, console):
            if core is not None:
                core.close()
        report["seconds"] = round(time.monotonic() - started, 1)
        path = out_dir / ("selftest.json" if args.selftest else "compare.json")
        path.write_text(json.dumps(report, indent=2))
        print(f"[oracle] report: {path} ({report['seconds']}s)")


def build_parser(description: str, default_bios: Path) -> argparse.ArgumentParser:
    """The comparison's common arguments; a title tool adds its own before parsing."""
    parser = argparse.ArgumentParser(description=description, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--bios", type=Path, default=default_bios,
                        help=f"authentic NTSC-U BIOS image (default {default_bios})")
    parser.add_argument("--region", default="na")
    parser.add_argument("--budget", type=int, default=6000, help="frame budget per checkpoint per core")
    parser.add_argument("--product-env", action="append", default=[], metavar="K=V",
                        help="extra environment for the product only, e.g. PSXPORT_FPS60=1 or a "
                             "PSXPORT_SETTINGS file with aspect=1; presentation must not change guest RAM")
    parser.add_argument("--frame-step", type=int, default=0,
                        help="compare every N frames inside a gameplay segment (0 = once per segment)")
    parser.add_argument("--selftest", action="store_true", help="validate the comparator with a seeded divergence")
    return parser


def product_env(args: argparse.Namespace) -> dict[str, str]:
    """The --product-env pairs as a dict, also recorded in the report."""
    pairs = dict(pair.split("=", 1) for pair in args.product_env)
    args.product_env_pairs = pairs
    return pairs
