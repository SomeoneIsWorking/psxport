#!/usr/bin/env python3
"""Hermetic tests for the presented-picture comparison (picture.py) over fake cores.

Every refusal here exists because a real instrument returned a number instead. The one that cost a
session is the first: a zero difference between two blank pictures, reported as a match, on every
native-path frame (psxport issue 0121). These prove each refusal FIRES, and that the comparator can
still say "these differ" and "these are identical" — a checker that only ever refuses is as useless
as one that never does.

The fake cores reuse test_compare's driver model and add a paint function per core, so a test says
what each core presents and nothing else.
"""

from __future__ import annotations

import argparse
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import compare  # noqa: E402
import picture  # noqa: E402
from test_compare import FakeConsole, FakeNative, FakeTitle  # noqa: E402

from PIL import Image  # noqa: E402

SIZE = (64, 48)


def paint(colour_of, size=SIZE):
    """A picture whose pixel at (x, y) is colour_of(x, y)."""

    def render(destination: Path) -> None:
        image = Image.new("RGB", size)
        image.putdata([colour_of(x, y) for y in range(size[1]) for x in range(size[0])])
        destination.parent.mkdir(parents=True, exist_ok=True)
        image.save(destination)

    return render


BLANK = paint(lambda x, y: (0, 0, 0))
UNIFORM = paint(lambda x, y: (7, 7, 7))
SCENE = paint(lambda x, y: (x * 3 % 256, y * 5 % 256, (x + y) % 256))
SCENE_WITH_A_BLOT = paint(lambda x, y: (255, 0, 0) if 8 <= x < 16 and 8 <= y < 16
                          else (x * 3 % 256, y * 5 % 256, (x + y) % 256))
SCENE_DITHERED = paint(lambda x, y: (x * 3 % 256, y * 5 % 256, ((x + y) % 256) ^ ((x + y) & 1)))
WIDE = paint(lambda x, y: (x % 256, y % 256, 0), (96, 48))


class Painter:
    """Mixin giving a fake core a capture() that draws whatever the test asked for."""

    def paint_with(self, render) -> None:
        self._render = render

    def capture(self, destination: Path) -> None:
        self._render(destination)


class PaintingNative(Painter, FakeNative):
    pass


class PaintingConsole(Painter, FakeConsole):
    pass


def arguments(**overrides) -> argparse.Namespace:
    values = {"bios": None, "region": "na", "budget": 50, "selftest": False, "frame_step": 0, "play": 0}
    values.update(overrides)
    return argparse.Namespace(**values)


class PictureTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        for name in ("port", "GAME.EXE", "bios.bin", "disc.chd"):
            (self.root / name).write_bytes(b"x")
        self.product = compare.Product(self.root / "port", self.root / "GAME.EXE", {}, self.root,
                                       self.root / "disc.chd")
        self.bios = self.root / "bios.bin"
        self.out = self.root / "out"

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def _run(self, native_paint, console_paint, **overrides):
        native, console = PaintingNative("native"), PaintingConsole(1)
        native.paint_with(native_paint)
        console.paint_with(console_paint)
        code = picture.run(FakeTitle(console_lookahead=1), self.product,
                           arguments(bios=self.bios, **overrides), self.out,
                           sessions=lambda product, args, out_dir: (native, console))
        name = "picture_selftest.json" if overrides.get("selftest") else "picture.json"
        return code, json.loads((self.out / name).read_text())

    def _row(self, report):
        self.assertTrue(report["pictures"], report)
        return report["pictures"][-1]

    # --- the refusals, each shown to fire -------------------------------------------------

    def test_a_blank_product_picture_is_refused_not_scored_as_a_match(self) -> None:
        code, report = self._run(BLANK, SCENE)
        self.assertEqual(code, 1, report)
        self.assertIn("blank", self._row(report)["refused"])
        self.assertNotIn("diff", self._row(report))

    def test_a_blank_reference_picture_is_refused(self) -> None:
        code, report = self._run(SCENE, BLANK)
        self.assertEqual(code, 1, report)
        self.assertIn("blank", self._row(report)["refused"])

    def test_two_blank_pictures_are_refused_although_they_are_identical(self) -> None:
        """The exact failure this tool replaces: a real zero, and not a match."""
        code, report = self._run(BLANK, BLANK)
        self.assertEqual(code, 1, report)
        self.assertIn("blank", self._row(report)["refused"])

    def test_a_single_colour_picture_is_refused(self) -> None:
        code, report = self._run(UNIFORM, SCENE)
        self.assertEqual(code, 1, report)
        self.assertIn("single colour", self._row(report)["refused"])

    def test_differing_dimensions_are_refused_rather_than_scaled(self) -> None:
        code, report = self._run(WIDE, SCENE)
        self.assertEqual(code, 1, report)
        self.assertIn("invent", self._row(report)["refused"])

    # --- and the answers it must still be able to give -------------------------------------

    def test_identical_pictures_report_zero_and_are_not_refused(self) -> None:
        code, report = self._run(SCENE, SCENE)
        self.assertEqual(code, 0, report)
        row = self._row(report)
        self.assertNotIn("refused", row)
        self.assertEqual(row["diff"]["differing"], 0)

    def test_a_localised_defect_is_reported_as_concentrated_with_its_tiles(self) -> None:
        code, report = self._run(SCENE_WITH_A_BLOT, SCENE)
        self.assertEqual(code, 0, report)  # a difference is a measurement, not a failure
        diff = self._row(report)["diff"]
        self.assertEqual(diff["differing"], 64)
        self.assertTrue(diff["concentrated"], diff)
        self.assertEqual(diff["worst_tiles"][0]["tile"], [0, 0])

    def test_an_everywhere_difference_is_reported_as_spread_not_concentrated(self) -> None:
        code, report = self._run(SCENE_DITHERED, SCENE)
        self.assertEqual(code, 0, report)
        diff = self._row(report)["diff"]
        self.assertGreater(diff["differing"], 0)
        self.assertFalse(diff["concentrated"], diff)

    def test_the_selftest_requires_both_answers(self) -> None:
        code, report = self._run(SCENE, SCENE, selftest=True)
        # The fake core paints the same picture whatever the state, so 60 frames change nothing and
        # the selftest must FAIL: it is not satisfied by a comparator that only reports zero.
        self.assertEqual(code, 1, report)
        self.assertFalse(report["selftest"]["detected"])
        self.assertEqual(report["selftest"]["changed_pixels"], 0)

    def test_a_missing_capture_is_refused(self) -> None:
        code, report = self._run(lambda destination: None, SCENE)
        self.assertEqual(code, 1, report)
        self.assertIn("no capture", report["error"])

    def test_refuses_a_title_without_checkpoints_and_a_missing_bios(self) -> None:
        native, console = PaintingNative("native"), PaintingConsole(1)
        for core in (native, console):
            core.paint_with(SCENE)
        sessions = lambda product, args, out_dir: (native, console)  # noqa: E731
        self.assertEqual(picture.run(FakeTitle(1), self.product, arguments(bios=self.root / "nope"),
                                     self.out, sessions=sessions), 2)
        bare = FakeTitle(1)
        bare.checkpoints = ()
        self.assertEqual(picture.run(bare, self.product, arguments(bios=self.bios), self.out,
                                     sessions=sessions), 2)


if __name__ == "__main__":
    unittest.main()
