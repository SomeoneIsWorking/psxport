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
from test_compare import SCRATCH, FakeConsole, FakeNative, FakeTitle  # noqa: E402

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
# Differs from SCENE in the LOW BIT of one channel: a rounding difference, well inside one 15-bit
# colour step, and invisible to a player.
SCENE_DITHERED = paint(lambda x, y: (x * 3 % 256, y * 5 % 256, ((x + y) % 256) ^ ((x + y) & 1)))
# Differs from SCENE everywhere by several colour steps: a real difference that happens to be spread.
SCENE_RECOLOURED = paint(lambda x, y: ((x * 3 + 40) % 256, y * 5 % 256, (x + y) % 256))
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


class PictureFixture(unittest.TestCase):
    """Fixture only -- no tests. Two test classes share it; inheriting PictureTests instead would
    re-run every one of its cases under the second class's name."""

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

    def _presented(self, report, core: str) -> dict:
        checkpoints = report["presented"]
        return checkpoints[list(checkpoints)[-1]][core]


class PictureTests(PictureFixture):
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
        code, report = self._run(SCENE_RECOLOURED, SCENE)
        self.assertEqual(code, 0, report)
        diff = self._row(report)["diff"]
        self.assertGreater(diff["significant"], 0)
        self.assertFalse(diff["concentrated"], diff)

    # --- rounding is not rendering ---------------------------------------------------------
    #
    # Both answers, because the whole point is a discriminator. Spyro 1's settled_play reported
    # 54.62% of pixels differing while 29.45% of the frame differed by exactly one colour step;
    # read as rendering, that number said the courtyard was half wrong, and it was not.

    def test_a_sub_step_difference_is_counted_but_not_called_a_colour_difference(self) -> None:
        code, report = self._run(SCENE_DITHERED, SCENE)
        self.assertEqual(code, 0, report)
        diff = self._row(report)["diff"]
        self.assertGreater(diff["differing"], 0, "the frames are not bit-identical")
        self.assertEqual(diff["significant"], 0, diff)
        self.assertEqual(diff["tiles_touched"], 0, "a rounding difference must touch no tile")
        self.assertFalse(diff["concentrated"], diff)

    def test_a_real_difference_of_several_steps_is_significant(self) -> None:
        code, report = self._run(SCENE_WITH_A_BLOT, SCENE)
        self.assertEqual(code, 0, report)
        diff = self._row(report)["diff"]
        self.assertGreater(diff["significant"], 0, diff)
        self.assertTrue(diff["concentrated"], diff)

    def test_the_magnitude_buckets_are_monotonic_and_bounded_by_the_bare_count(self) -> None:
        """A bucket that counted the wrong thing would still look plausible alone; the shape of the
        whole distribution is what catches it."""
        code, report = self._run(SCENE_WITH_A_BLOT, SCENE)
        diff = self._row(report)["diff"]
        counts = [entry["pixels"] for entry in diff["beyond"]]
        thresholds = [entry["threshold"] for entry in diff["beyond"]]
        self.assertEqual(thresholds, sorted(thresholds))
        self.assertEqual(counts, sorted(counts, reverse=True))
        self.assertLessEqual(counts[0], diff["differing"])
        self.assertEqual(counts[0], diff["significant"])

    def test_worst_tiles_rank_by_colour_difference_not_by_rounding(self) -> None:
        """The failure this ordering exists to prevent: a frame that rounds differently everywhere
        and has lost an object in one place must name the place, not the rounding."""
        blot_and_dither = paint(lambda x, y: (255, 0, 0) if 8 <= x < 16 and 8 <= y < 16
                                else (x * 3 % 256, y * 5 % 256, ((x + y) % 256) ^ ((x + y) & 1)))
        code, report = self._run(blot_and_dither, SCENE)
        self.assertEqual(code, 0, report)
        diff = self._row(report)["diff"]
        tiles = [tuple(entry["tile"]) for entry in diff["worst_tiles"]]
        self.assertTrue(tiles, diff)
        for tile in tiles:
            self.assertLess(tile[0], 16, f"tile {tile} is outside the blot")
            self.assertLess(tile[1], 16, f"tile {tile} is outside the blot")

    def test_a_decisive_state_divergence_is_refused_rather_than_scored(self) -> None:
        """The refusal that was missing, and what it cost. On Spyro 1 (2026-09-19) the two cores
        reached the same `playing` checkpoint at different moments -- the reference still on the
        "The Adventure Begins..." card, the product already showing the world -- and the tool
        reported 18% of pixels differing, then 58-89% over the 600 frames that followed. Every one
        of those numbers was about WHEN each core was photographed, not about rendering."""
        native, console = PaintingNative("native"), PaintingConsole(1)
        native.paint_with(SCENE_WITH_A_BLOT)
        console.paint_with(SCENE)
        console.memory[0:4] = (999).to_bytes(4, "little")  # a decisive range: the frame counter
        code = picture.run(FakeTitle(console_lookahead=1), self.product,
                           arguments(bios=self.bios), self.out,
                           sessions=lambda product, args, out_dir: (native, console))
        report = json.loads((self.out / "picture.json").read_text())
        self.assertEqual(code, 1, report)
        row = self._row(report)
        self.assertIn("refused", row)
        self.assertIn("not at the same guest state", row["refused"])
        self.assertNotIn("diff", row)  # no percentage that ranks nothing
        self.assertEqual([d["range"] for d in row["state_divergence"]], ["frame"])

    def test_an_informational_range_diverging_does_not_block_the_comparison(self) -> None:
        """The other answer. Spyro's level-tick counter keeps a VSync-phase offset the host clock
        cannot reproduce (issue 0114), so a check that required every declared range to match would
        refuse every gameplay comparison forever and this instrument would print one answer only."""
        native, console = PaintingNative("native"), PaintingConsole(1)
        native.paint_with(SCENE_WITH_A_BLOT)
        console.paint_with(SCENE)
        console.write8(SCRATCH, 0x5A)  # declared, but not decisive
        code = picture.run(FakeTitle(console_lookahead=1), self.product,
                           arguments(bios=self.bios), self.out,
                           sessions=lambda product, args, out_dir: (native, console))
        report = json.loads((self.out / "picture.json").read_text())
        self.assertEqual(code, 0, report)
        row = self._row(report)
        self.assertNotIn("refused", row)
        self.assertEqual(row["diff"]["differing"], 64)

    def test_a_title_can_declare_a_range_decisive_for_the_picture_that_the_ram_gate_ignores(self) -> None:
        """The two questions are not the same question. A title marks its camera informational
        because a small camera difference cannot change whether the game BEHAVES -- and it moves
        every pixel. Measured on Spyro 1: 87% of pixels differed at a dragon-cutscene frame with
        every RAM-decisive range equal, because the product framed the shot ~25px left of the
        reference. Without this the picture tool reports that as a rendering defect."""

        class PictureStrictTitle(FakeTitle):
            picture_decisive = ("scratch",)  # informational for RAM, decisive for the picture

        native, console = PaintingNative("native"), PaintingConsole(1)
        native.paint_with(SCENE_WITH_A_BLOT)
        console.paint_with(SCENE)
        console.write8(SCRATCH, 0x5A)
        code = picture.run(PictureStrictTitle(console_lookahead=1), self.product,
                           arguments(bios=self.bios), self.out,
                           sessions=lambda product, args, out_dir: (native, console))
        report = json.loads((self.out / "picture.json").read_text())
        self.assertEqual(code, 1, report)
        row = self._row(report)
        self.assertIn("not at the same guest state", row.get("refused", ""))
        self.assertNotIn("diff", row)

    def test_every_diverging_range_is_reported_even_when_it_does_not_block(self) -> None:
        """A comparison that passes still says what was not equal underneath it, so a number is
        never read without the state it was taken at."""
        native, console = PaintingNative("native"), PaintingConsole(1)
        native.paint_with(SCENE_WITH_A_BLOT)
        console.paint_with(SCENE)
        console.write8(SCRATCH, 0x5A)
        code = picture.run(FakeTitle(console_lookahead=1), self.product,
                           arguments(bios=self.bios), self.out,
                           sessions=lambda product, args, out_dir: (native, console))
        report = json.loads((self.out / "picture.json").read_text())
        self.assertEqual(code, 0, report)
        row = self._row(report)
        self.assertNotIn("refused", row)
        self.assertEqual([d["range"] for d in row["state_divergence"]], ["scratch"])

    def test_the_selftest_requires_both_answers(self) -> None:
        code, report = self._run(SCENE, SCENE, selftest=True)
        # The fake core paints the same picture whatever the state, so 60 frames change nothing and
        # the selftest must FAIL: it is not satisfied by a comparator that only reports zero.
        self.assertEqual(code, 1, report)
        self.assertFalse(report["selftest"]["detected"])
        self.assertEqual(report["selftest"]["changed_pixels"], 0)

    def test_the_product_is_cropped_to_the_rows_it_reports_as_scanned(self) -> None:
        """The native path presents more rows than a console scans out, and the product says how
        many. Comparing the raw frames instead cost a retracted defect report (Tomba! 2 issue 0012):
        the unscanned rows alone read as a 92% difference and an apparent 7-pixel offset."""
        native, console = PaintingNative("native"), PaintingConsole(1)
        # The product presents 48 rows; the top 32 are the scene, the rest is below the scan window.
        native.paint_with(paint(lambda x, y: (x * 3 % 256, y * 5 % 256, (x + y) % 256)
                                if y < 32 else (255, 255, 0), (64, 48)))
        native.scan_rows = 32
        console.paint_with(paint(lambda x, y: (x * 3 % 256, y * 5 % 256, (x + y) % 256), (64, 32)))
        code = picture.run(FakeTitle(console_lookahead=1), self.product,
                           arguments(bios=self.bios), self.out,
                           sessions=lambda product, args, out_dir: (native, console))
        report = json.loads((self.out / "picture.json").read_text())
        row = report["pictures"][-1]
        self.assertEqual(code, 0, report)
        self.assertNotIn("refused", row)
        self.assertEqual(row["native"]["size"], [64, 32])
        self.assertEqual(row["diff"]["differing"], 0)

    def test_without_a_reported_scan_count_the_frames_are_compared_as_captured(self) -> None:
        """No crop is invented when the product did not state one: a tool that guessed the count
        would be fitting the alignment that made its own number look best."""
        native, console = PaintingNative("native"), PaintingConsole(1)
        native.paint_with(paint(lambda x, y: (x * 3 % 256, y * 5 % 256, (x + y) % 256), (64, 48)))
        native.scan_rows = None
        console.paint_with(paint(lambda x, y: (x * 3 % 256, y * 5 % 256, (x + y) % 256), (64, 32)))
        code = picture.run(FakeTitle(console_lookahead=1), self.product,
                           arguments(bios=self.bios), self.out,
                           sessions=lambda product, args, out_dir: (native, console))
        report = json.loads((self.out / "picture.json").read_text())
        self.assertEqual(code, 1, report)
        self.assertIn("invent", report["pictures"][-1]["refused"])

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

    def test_a_recorded_route_without_play_is_refused_rather_than_ignored(self) -> None:
        """--route only drives the post-checkpoint segment, so --play 0 silently discarded it and the
        run printed the ordinary checkpoint comparison as though the flag had been honoured."""
        native, console = PaintingNative("native"), PaintingConsole(1)
        for core in (native, console):
            core.paint_with(SCENE)
        sessions = lambda product, args, out_dir: (native, console)  # noqa: E731
        route = self.root / "route.pad"
        route.write_bytes(b"\xff\xff" * 4)  # four frames of "nothing held" (a PSX pad mask is active-low)
        self.assertEqual(picture.run(FakeTitle(1), self.product,
                                     arguments(bios=self.bios, route=route, route_from=0, play=0),
                                     self.out, sessions=sessions), 2)
        # ... and the SAME arguments with a play budget are NOT refused, so the refusal is about the
        # missing budget and not about passing a route at all.
        self.assertEqual(picture.run(FakeTitle(1), self.product,
                                     arguments(bios=self.bios, route=route, route_from=0, play=2),
                                     self.out, sessions=sessions), 0)

    def test_frame_step_compares_along_the_route_not_only_at_its_end(self) -> None:
        """A single end-of-route picture answers only for the moment the route stops at. --frame-step
        was accepted and ignored in picture mode, so a run asking for a series silently got one."""
        native, console = PaintingNative("native"), PaintingConsole(1)
        for core in (native, console):
            core.paint_with(SCENE)
        sessions = lambda product, args, out_dir: (native, console)  # noqa: E731
        route = self.root / "route.pad"
        route.write_bytes(b"\xff\xff" * 8)
        self.assertEqual(picture.run(FakeTitle(1), self.product,
                                     arguments(bios=self.bios, route=route, route_from=0, play=8,
                                               frame_step=2),
                                     self.out, sessions=sessions), 0)
        report = json.loads((self.out / "picture.json").read_text())
        names = [picture_report["checkpoint"] for picture_report in report["pictures"]]
        # Three interior samples (f2, f4, f6) plus the end; f8 is the end and must not be compared twice.
        self.assertEqual([name for name in names if "-f" in name],
                         ["route-8f-f2", "route-8f-f4", "route-8f-f6"])
        self.assertEqual(names[-1], "route-8f")


if __name__ == "__main__":
    unittest.main()


class AdvanceToPresentedTests(PictureFixture):
    """A checkpoint predicate is guest STATE; a title can enter it before it draws anything, and the
    product's host-file I/O gets there long before the reference's emulated disc. Photographing at
    arrival then compares a blank frame against a drawn one, which answers neither question."""

    def _painter(self, blank_probes: int):
        """Paint BLANK for the first `blank_probes` captures, then the scene. Counts its calls so a
        test can prove the loop really probed rather than got the answer for free."""

        state = {"calls": 0}

        def paint(destination: Path) -> None:
            (BLANK if state["calls"] < blank_probes else SCENE)(destination)
            state["calls"] += 1

        return paint, state

    def test_a_core_that_starts_blank_is_advanced_until_it_presents(self) -> None:
        native, state = self._painter(blank_probes=3)
        code, report = self._run(native, SCENE)
        presented = self._presented(report, "native")
        self.assertTrue(presented["presented"])
        self.assertEqual(presented["extra_frames"], 3 * picture.PictureRun.PRESENT_STEP)
        self.assertGreater(state["calls"], 3)  # it really probed; it did not get the answer free
        # Both pictures are now drawn, so the old "blank" refusal is gone. What is left is the
        # HONEST consequence: advancing one core and not the other moves its guest state, and the
        # state guard says so by name. Settling converts a dead end into a named diagnosis; it
        # cannot invent an alignment the run does not have.
        row = self._row(report)
        self.assertNotIn("blank", row.get("refused", ""))
        self.assertIn("not at the same guest state", row["refused"])
        self.assertIn("frame", row["refused"])

    def test_when_both_cores_need_the_same_advance_the_comparison_proceeds(self) -> None:
        native, _ = self._painter(blank_probes=3)
        console, _ = self._painter(blank_probes=3)
        code, report = self._run(native, console)
        row = self._row(report)
        self.assertNotIn("refused", row)
        self.assertEqual(row["diff"]["differing"], 0)
        for core in ("native", "console"):
            self.assertEqual(self._presented(report, core)["extra_frames"],
                             3 * picture.PictureRun.PRESENT_STEP)

    def test_a_core_already_presenting_is_not_advanced_at_all(self) -> None:
        code, report = self._run(SCENE, SCENE)
        for core in ("native", "console"):
            presented = self._presented(report, core)
            self.assertTrue(presented["presented"])
            self.assertEqual(presented["extra_frames"], 0)

    def test_a_core_that_never_presents_is_reported_with_what_was_scanned(self) -> None:
        never = self._painter(blank_probes=10**6)[0]
        code, report = self._run(never, SCENE)
        presented = self._presented(report, "native")
        self.assertFalse(presented["presented"])
        # The negative must carry a denominator: "never presented" is not "never looked".
        self.assertGreater(presented["probes"], 1)
        self.assertEqual(presented["extra_frames"], picture.PictureRun.PRESENT_BUDGET)
        self.assertEqual(code, 1)
        self.assertIn("refused", self._row(report))


class WithheldCheckpointTests(PictureFixture):
    """A checkpoint whose predicate holds DURING an animation aligns state but not presentation, so
    photographing it produces a percentage that ranks nothing (Spyro's issue 0126). The title says
    so on the checkpoint; both answers are checked here, because a tool that quietly stopped
    photographing everything would pass a one-sided test."""

    REASON = "the predicate holds mid-intro, so the two cores are at different moments of it"

    def _two_checkpoints(self, withhold_first: bool) -> FakeTitle:
        title = FakeTitle(console_lookahead=1)
        reach = title.checkpoints[0].reach
        title.checkpoints = (
            compare.Checkpoint("arrival", reach,
                               not_picture_comparable=self.REASON if withhold_first else None),
            compare.Checkpoint("settled", reach),
        )
        return title

    def _run_title(self, title: FakeTitle):
        native, console = PaintingNative("native"), PaintingConsole(1)
        native.paint_with(SCENE)
        console.paint_with(SCENE)
        code = picture.run(title, self.product, arguments(bios=self.bios), self.out,
                           sessions=lambda product, args, out_dir: (native, console))
        return code, json.loads((self.out / "picture.json").read_text())

    def _named(self, report, name: str) -> dict:
        rows = [row for row in report["pictures"] if row["checkpoint"] == name]
        self.assertEqual(len(rows), 1, report["pictures"])
        return rows[0]

    def test_a_withheld_checkpoint_is_reported_with_its_reason_and_not_scored(self) -> None:
        code, report = self._run_title(self._two_checkpoints(withhold_first=True))
        self.assertEqual(code, 0, report)
        withheld = self._named(report, "arrival")
        self.assertFalse(withheld["photographed"])
        self.assertEqual(withheld["withheld"], self.REASON)
        # No number at all: the point is that a percentage here would be read as a verdict.
        self.assertNotIn("diff", withheld)
        self.assertNotIn("refused", withheld)
        # Nor the per-core advance made solely so a photo could be taken.
        self.assertNotIn("arrival", report["presented"])

    def test_the_same_checkpoint_is_photographed_when_the_title_does_not_withhold_it(self) -> None:
        """The other answer: without the field, `arrival` is compared like any other checkpoint."""
        code, report = self._run_title(self._two_checkpoints(withhold_first=False))
        self.assertEqual(code, 0, report)
        photographed = self._named(report, "arrival")
        self.assertNotIn("withheld", photographed)
        self.assertIn("diff", photographed)
        self.assertIn("arrival", report["presented"])

    def test_withholding_one_checkpoint_leaves_the_others_compared(self) -> None:
        code, report = self._run_title(self._two_checkpoints(withhold_first=True))
        self.assertEqual(code, 0, report)
        settled = self._named(report, "settled")
        self.assertIn("diff", settled)
        self.assertIn("settled", report["presented"])
