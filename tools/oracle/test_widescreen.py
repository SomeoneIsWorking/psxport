#!/usr/bin/env python3
"""Hermetic tests for the widescreen extension check (widescreen.py).

Every fake below is a way a port could pass the STATE oracle while shipping a wrong picture, which
is the whole reason this instrument exists: guest state is identical for all of them.
"""

from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import widescreen  # noqa: E402

from PIL import Image  # noqa: E402

NARROW = (64, 48)
MARGIN = 10
WIDE = (NARROW[0] + 2 * MARGIN, NARROW[1])


def scene(x: int, y: int) -> tuple[int, int, int]:
    """Varied in both axes, so a smear or a stretch is detectable."""
    return ((x * 37) % 256, (y * 53) % 256, (x * 11 + y * 7) % 256)


def draw(path: Path, size, colour_of) -> Path:
    image = Image.new("RGB", size)
    image.putdata([colour_of(x, y) for y in range(size[1]) for x in range(size[0])])
    path.parent.mkdir(parents=True, exist_ok=True)
    image.save(path)
    return path


class WidescreenTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self.out = self.root / "out"
        # The 4:3 frame, and the world it is a window onto.
        self.narrow = draw(self.root / "narrow.png", NARROW, scene)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def _wide(self, colour_of) -> Path:
        return draw(self.root / "wide.png", WIDE, colour_of)

    # --- the honest implementation -----------------------------------------------------

    def test_a_true_extension_keeps_the_centre_and_fills_the_margins(self) -> None:
        wide = self._wide(lambda x, y: scene(x - MARGIN, y))
        result = widescreen.analyse(self.narrow, wide, self.out)
        self.assertEqual(result.margin, MARGIN)
        self.assertEqual(result.centre.significant, 0, result.report())
        self.assertTrue(result.centre_survived)
        self.assertTrue(all(m.carries_scene for m in result.margins), result.report())
        self.assertTrue(result.extends)

    # --- the three ways to fake it, each shown to be caught ------------------------------

    def test_a_stretched_frame_is_not_an_extension(self) -> None:
        """The cheapest fake, and the one the project's rules name: the same picture resampled to
        the wider viewport. Guest state is untouched, so only the picture can tell."""
        with Image.open(self.narrow) as handle:
            handle.convert("RGB").resize(WIDE, Image.NEAREST).save(self.root / "wide.png")
        result = widescreen.analyse(self.narrow, self.root / "wide.png", self.out)
        self.assertFalse(result.centre_survived, result.report())
        self.assertFalse(result.extends)

    def test_margins_smeared_from_the_edge_column_are_not_coverage(self) -> None:
        """The centre is perfect here -- this fake passes the centre check and must still fail."""
        wide = self._wide(lambda x, y: scene(min(max(x - MARGIN, 0), NARROW[0] - 1), y))
        result = widescreen.analyse(self.narrow, wide, self.out)
        self.assertTrue(result.centre_survived, "the centre really is intact")
        self.assertEqual([m.repeated_columns for m in result.margins], [MARGIN - 1] * 2)
        self.assertFalse(any(m.carries_scene for m in result.margins))
        self.assertFalse(result.extends)

    def test_black_margins_are_not_coverage(self) -> None:
        """Pillarboxing: the viewport widened and nothing was drawn in the new area."""
        wide = self._wide(lambda x, y: scene(x - MARGIN, y)
                          if MARGIN <= x < MARGIN + NARROW[0] else (0, 0, 0))
        result = widescreen.analyse(self.narrow, wide, self.out)
        self.assertTrue(result.centre_survived)
        self.assertFalse(any(m.carries_scene for m in result.margins), result.report())
        self.assertFalse(result.extends)

    # --- the refusals, rather than a number from an unanswerable pair ---------------------

    def test_a_capture_that_did_not_widen_is_refused(self) -> None:
        """What aspect=3 (AUTO) produces headless: two 4:3 frames, and a 0% that would read as a
        perfect extension."""
        same = draw(self.root / "wide.png", NARROW, scene)
        with self.assertRaises(widescreen.Unanswerable) as caught:
            widescreen.analyse(self.narrow, same, self.out)
        self.assertIn("did not widen", str(caught.exception))

    def test_differing_heights_are_refused(self) -> None:
        taller = draw(self.root / "wide.png", (WIDE[0], WIDE[1] + 8), scene)
        with self.assertRaises(widescreen.Unanswerable):
            widescreen.analyse(self.narrow, taller, self.out)

    def test_an_odd_width_difference_is_refused_rather_than_biased(self) -> None:
        odd = draw(self.root / "wide.png", (NARROW[0] + 2 * MARGIN + 1, NARROW[1]), scene)
        with self.assertRaises(widescreen.Unanswerable) as caught:
            widescreen.analyse(self.narrow, odd, self.out)
        self.assertIn("ODD", str(caught.exception))

    def test_a_missing_capture_is_refused(self) -> None:
        with self.assertRaises(widescreen.Unanswerable):
            widescreen.analyse(self.narrow, self.root / "absent.png", self.out)


if __name__ == "__main__":
    unittest.main()
