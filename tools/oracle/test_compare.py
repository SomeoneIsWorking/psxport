#!/usr/bin/env python3
"""Hermetic tests for the state-aligned comparison driver (compare.py) over fake cores.

The fakes model the one thing the driver exists to get right: a VBlank-stepped console whose
guest reads the pad with a per-title latency, against a product that reads the pad in the frame it
is stepped. The tests show both answers: the matching lookahead yields MATCH on every checkpoint,
a wrong lookahead is reported as a decisive `pad` divergence, and the seeded selftest is detected.
"""

from __future__ import annotations

import argparse
import json
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import compare  # noqa: E402
from compare_cores import PSX_BUTTON_BITS  # noqa: E402

FRAME = 0x80001000    # u32 game frames run
PAD = 0x80001010      # u16 pad mask the last game frame read
SCRATCH = 0x80001020  # u8 informational byte
COUNTER = 0x80001030  # u32 VBlanks since the guest loop last reset it (console only)


def mask(buttons: frozenset[str]) -> int:
    return sum(PSX_BUTTON_BITS[name] for name in buttons)


class FakeCore:
    """Shared memory model: a frame counter, the pad each frame read, and a pad log per frame."""

    def __init__(self, name: str):
        self.name = name
        self.frames = 0
        self.memory = bytearray(0x100)
        self.pad_log: list[int] = []
        self._held: frozenset[str] = frozenset()

    @property
    def held(self) -> frozenset[str]:
        return self._held

    def hold(self, buttons: frozenset[str]) -> None:
        self._held = buttons

    def _offset(self, address: int) -> int:
        return address - 0x80001000

    def read(self, address: int, size: int) -> bytes:
        return bytes(self.memory[self._offset(address):self._offset(address) + size])

    def write8(self, address: int, value: int) -> None:
        self.memory[self._offset(address)] = value & 0xFF

    def _run_game_frame(self, pad: int) -> None:
        frame = int.from_bytes(self.read(FRAME, 4), "little") + 1
        self.memory[0:4] = frame.to_bytes(4, "little")
        self.memory[0x10:0x12] = pad.to_bytes(2, "little")
        self.pad_log.append(pad)

    def close(self) -> None:
        pass


class FakeNative(FakeCore):
    reference = False

    def step(self, frames: int) -> None:
        for _ in range(frames):
            self._run_game_frame(mask(self._held))
            self.frames += 1


class FakeConsole(FakeCore):
    """Two VBlanks per game frame. The pad is sampled at the first VBlank of a frame; with
    `latency` 0 the frame reads that sample, with 1 it reads the sample taken one frame earlier."""

    reference = True

    def __init__(self, latency: int):
        super().__init__("console")
        self.latency = latency
        self._samples: list[int] = [0] * latency
        self._counter = 0

    def step(self, frames: int) -> None:
        for _ in range(frames):
            self.frames += 1
            self._counter += 1
            if self._counter == 1:
                self._samples.append(mask(self._held))
            if self._counter == 2:
                self._run_game_frame(self._samples.pop(0))
                self._counter = 0
            self.memory[0x30:0x34] = self._counter.to_bytes(4, "little")


class FakeTitle:
    name = "fake"
    declared = (
        compare.DeclaredRange("frame", FRAME, 4, True),
        compare.DeclaredRange("pad", PAD, 2, True),
        compare.DeclaredRange("scratch", SCRATCH, 1, False),
    )
    excluded = {"everything else": "not modelled"}
    gameplay = (
        (frozenset(), 2),
        (frozenset({"left"}), 3),
        (frozenset(), 1),
        (frozenset({"cross", "left"}), 2),
    )
    selftest = compare.SelftestSeed(PAD, "pad", 0)

    def __init__(self, console_lookahead: int, seed: compare.SelftestSeed | None = None):
        self.console_lookahead = console_lookahead
        if seed is not None:
            self.selftest = seed
        self.checkpoints = (compare.Checkpoint("started", self.reach_started),)

    def lookahead(self, core) -> int:
        return self.console_lookahead if core.reference else 0

    def advance(self, core, frames: int, strict: bool = False) -> None:
        if not core.reference:
            core.step(frames)
            return
        for _ in range(frames):
            compare.step_until_counter_resets(core, COUNTER)

    def observe(self, core) -> int:
        return compare.u32(core, FRAME)

    def summary(self, core) -> dict:
        return {"frame": self.observe(core)}

    @staticmethod
    def reach_started(driver, core, budget, settle):
        return driver.drive(core, budget, lambda frame: frame >= 3, compare.tap("cross", 2, 1), "started", settle)


def arguments(**overrides) -> argparse.Namespace:
    values = {"bios": None, "region": "na", "budget": 50, "selftest": False, "frame_step": 0}
    values.update(overrides)
    return argparse.Namespace(**values)


class CompareTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        root = Path(self._tmp.name)
        for name in ("port", "GAME.EXE", "bios.bin", "disc.chd"):
            (root / name).write_bytes(b"x")
        self.product = compare.Product(root / "port", root / "GAME.EXE", {}, root, root / "disc.chd")
        self.bios = root / "bios.bin"
        self.out = root / "out"

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def _run(self, title: FakeTitle, latency: int, **overrides) -> tuple[int, dict, FakeNative, FakeConsole]:
        native, console = FakeNative("native"), FakeConsole(latency)
        code = compare.run(title, self.product, arguments(bios=self.bios, **overrides), self.out,
                           sessions=lambda product, args, out_dir: (native, console))
        name = "selftest.json" if overrides.get("selftest") else "compare.json"
        return code, json.loads((self.out / name).read_text()), native, console

    def test_matching_lookahead_matches_every_checkpoint_and_delivers_the_schedule(self) -> None:
        code, report, native, console = self._run(FakeTitle(console_lookahead=1), latency=1)
        self.assertEqual(code, 0, report)
        self.assertTrue(report["complete"])
        self.assertTrue(all(row["equal"] for entry in report["checkpoints"] for row in entry["ranges"]))
        schedule = [mask(buttons) for buttons, frames in FakeTitle.gameplay for _ in range(frames)]
        self.assertEqual(native.pad_log[-len(schedule):], schedule)
        self.assertEqual(console.pad_log[-len(schedule):], schedule)
        # The console's first game frame reads the pad committed before the driver started (its
        # latency is one frame); every later frame reads what the driver delivered.
        self.assertEqual(native.pad_log[1:], console.pad_log[1:])

    def test_prompt_console_matches_without_lookahead(self) -> None:
        code, report, native, console = self._run(FakeTitle(console_lookahead=0), latency=0)
        self.assertEqual(code, 0, report)
        self.assertEqual(native.pad_log, console.pad_log)

    def test_wrong_lookahead_is_reported_as_a_decisive_pad_divergence(self) -> None:
        code, report, native, console = self._run(FakeTitle(console_lookahead=0), latency=1)
        self.assertEqual(code, 1)
        self.assertTrue(report["complete"])
        diverged = [row for entry in report["checkpoints"] for row in entry["ranges"]
                    if row["decisive"] and not row["equal"]]
        self.assertTrue(diverged)
        self.assertEqual(diverged[0]["range"], "pad")
        self.assertNotEqual(native.pad_log, console.pad_log)

    def test_selftest_detects_the_seeded_byte_and_misses_a_wrong_seed(self) -> None:
        code, report, _, _ = self._run(FakeTitle(console_lookahead=1), latency=1, selftest=True)
        self.assertEqual(code, 0, report)
        self.assertTrue(report["selftest"]["detected"])
        wrong = FakeTitle(console_lookahead=1, seed=compare.SelftestSeed(SCRATCH, "pad", 0))
        code, report, _, _ = self._run(wrong, latency=1, selftest=True)
        self.assertEqual(code, 1)
        self.assertFalse(report["selftest"]["detected"])

    def test_product_gets_a_fresh_card_and_existing_console_saves_are_refused(self) -> None:
        stale = self.out / "card.mcr"
        stale.parent.mkdir(parents=True, exist_ok=True)
        stale.write_bytes(b"old save")
        seen: dict[str, str] = {}

        def sessions(product, args, out_dir):
            seen.update(product.environment)
            console = FakeConsole(latency=1)
            console.manifest = {"existing_save_files": ["SCUS-94228.mcr"]}
            return FakeNative("native"), console

        code = compare.run(FakeTitle(1), self.product, arguments(bios=self.bios), self.out, sessions=sessions)
        self.assertEqual(code, 1)
        self.assertEqual(seen[compare.CARD_ENV], str(stale))
        self.assertFalse(stale.exists())
        report = json.loads((self.out / "compare.json").read_text())
        self.assertIn("existing save files", report["error"])

    def test_refuses_a_missing_bios_and_a_title_without_checkpoints(self) -> None:
        title = FakeTitle(console_lookahead=1)
        code = compare.run(title, self.product, arguments(bios=self.bios / "missing"), self.out)
        self.assertEqual(code, 2)
        title.checkpoints = ()
        code = compare.run(title, self.product, arguments(bios=self.bios), self.out)
        self.assertEqual(code, 2)

    def test_compare_reports_first_differing_offset_and_count(self) -> None:
        title = FakeTitle(console_lookahead=1)
        rows = compare.compare(title, {"frame": b"\x01\x02\x03\x04", "pad": b"\x00\x00", "scratch": b"\x00"},
                               {"frame": b"\x01\x02\x07\x08", "pad": b"\x00\x00", "scratch": b"\x00"})
        frame = rows[0]
        self.assertFalse(frame["equal"])
        self.assertEqual((frame["first_diff_offset"], frame["differing_bytes"]), (2, 2))
        self.assertTrue(rows[1]["equal"] and rows[2]["equal"])

    def test_step_until_counter_resets_counts_the_vblanks(self) -> None:
        console = FakeConsole(latency=0)
        self.assertEqual(compare.step_until_counter_resets(console, COUNTER), 2)
        self.assertEqual(console.frames, 2)

    def test_playback_refuses_a_park_that_does_not_match_the_schedule(self) -> None:
        native = FakeNative("native")
        native.hold(frozenset({"cross"}))
        with self.assertRaises(compare.CoreError):
            compare.Playback(compare.Driver(FakeTitle(1)), native, [frozenset()])


if __name__ == "__main__":
    unittest.main()


class MatchConsoleCardTest(unittest.TestCase):
    """The reference's memory card is an INPUT to the comparison, so the harness must be able to
    make it equal to the product's -- and must say so, both when it did and when it did not."""

    def setUp(self) -> None:
        self.directory = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.directory, ignore_errors=True)
        self.card = self.directory / "card.mcr"
        self.image = b"MC" + bytes(128 * 1024 - 2)

    def console(self):
        class Console:
            def __init__(self):
                self.received = None

            def insert_card(self, card) -> dict:
                self.received = card
                image = card.read_bytes()
                return {"card_bytes": len(image), "card_sha256": "abc", "magic": "MC"}

        return Console()

    def test_off_by_default_and_says_the_reference_kept_its_own_card(self) -> None:
        console = self.console()
        self.card.write_bytes(self.image)
        result = compare.match_console_card(console, None)
        self.assertFalse(result["matched"])
        self.assertIn("--console-card", result["reason"])
        self.assertIsNone(console.received)  # the NEGATIVE really did nothing

    def test_on_hands_the_reference_the_product_bytes_verbatim(self) -> None:
        console = self.console()
        self.card.write_bytes(self.image)
        result = compare.match_console_card(console, self.card)
        self.assertTrue(result["matched"])
        self.assertEqual(console.received, self.card)
        self.assertEqual(result["source"], str(self.card))

    def test_refuses_a_missing_card_instead_of_comparing_different_cards(self) -> None:
        console = self.console()
        with self.assertRaises(compare.CoreError) as raised:
            compare.match_console_card(console, self.card)
        self.assertIn("does not exist", str(raised.exception))
        self.assertIsNone(console.received)

    def test_refuses_a_reference_that_cannot_take_a_card(self) -> None:
        self.card.write_bytes(self.image)
        with self.assertRaises(compare.CoreError):
            compare.match_console_card(object(), self.card)
