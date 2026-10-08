#!/usr/bin/env python3
"""picture.py — compare the PICTURE the product PRESENTS against the console reference's.

WHY THIS EXISTS, and what it is not a duplicate of. `compare.py` compares guest RAM at title-owned
checkpoints. It is the right instrument for "does the simulation still behave", and it is
structurally incapable of seeing a rendering defect: a menu drawn with the wrong colours, at the
wrong place, or missing a panel writes exactly the same guest state as a correct one. Every "0
divergences" in this repository's oracle reports is a statement about RAM and says nothing about
pixels.

`vram_oracle.py` (a consumer's tool) compares the guest GPU command feed rasterised into emulated
VRAM. That answers "is our rasterizer right", and only on a run whose picture actually comes from
that feed. On a NATIVE render path the picture never enters emulated VRAM, so it compared two blank
buffers and printed "differing 0/524288 (0.00%)" as a pass — measured on Tomba! 2's save-card pages,
2026-09-19, and the reason this tool was written (psxport issue 0121).

WHAT THIS COMPARES. The frame each core PRESENTS, captured through each core's own present path:
the product through the REPL's `shot` (gpu_native_shot, so it follows the active render path and the
wide presentation region), the reference through its libretro framebuffer. Both cores are driven by
the same title checkpoints `compare.py` uses -- but reaching the same checkpoint is NOT the same as
standing at the same moment, so every comparison below CHECKS that before it believes a number.

BEFORE IT PHOTOGRAPHS, IT WAITS FOR EACH CORE TO BE PRESENTING SOMETHING. A checkpoint predicate
is satisfied by guest STATE, and a title can enter that state well before it draws anything. The
product's host-file I/O also completes far sooner than the reference's emulated disc, so the two
reach the same state at very different moments. Measured 2026-09-19 on Tomba! 2: all three
checkpoints refused as blank on one side -- the product had the complete title screen at game frame
27 while the reference was still black at 302 -- so the run produced no comparison at all, in either
direction. Each core is now advanced with no input, bounded, until it presents a non-blank frame,
and the report records the extra frames and the probes taken (including for a core that never
presents, so "never drew" stays distinguishable from "never looked"). This does NOT invent
alignment: when only one core needed advancing, its guest state moves and the state refusal below
fires instead -- which names ranges, where a blank refusal named nothing. When both settle
together, the comparison proceeds; that is what produced Tomba! 2's first picture number.

WHAT IT REFUSES, and why each refusal exists rather than a number:

  * EITHER picture blank, or both uniform. A zero difference between two pictures that hold nothing
    is the uniform-output tell, not a match. This is the exact failure it was written to replace.
  * Differing dimensions. A widescreen product against a 4:3 reference is a deliberate difference,
    not a defect, and scaling one onto the other would invent pixels and then measure them. Compare
    the product's 4:3 output against the reference, and judge the widescreen extra area by coverage.
  * A capture either core did not write.
  * THE TWO CORES ARE NOT AT THE SAME GUEST STATE. Measured 2026-09-19 on Spyro 1: at the `playing`
    checkpoint the reference was showing the "The Adventure Begins..." card while the product had
    already faded the world in, and a 600-frame held-input schedule from there reported 58-89% of
    pixels differing. None of that was a rendering defect -- the two cores were photographed at
    different moments of the same route. A picture difference is only evidence about rendering when
    the simulation underneath it agrees, so this compares the title's declared ranges through
    `compare.py`'s own owner and refuses rather than printing a percentage that ranks nothing.
    Which ranges block is the title's `picture_decisive`, NOT the RAM gate's `decisive`: a title
    marks its camera informational because a camera difference cannot change whether the game
    behaves, and it moves every pixel. Every diverging range is reported either way.

WHAT A DIFFERENCE MEANS, stated so it cannot be overread. The two cores are different renderers:
the reference rasterises the guest's own command stream at native resolution, the product draws
through native producers. A small, evenly spread difference is dithering and sub-pixel sampling. A
difference CONCENTRATED in one region is a defect with a place to look, which is what the per-tile
report below is for. This tool gives you the number and the pictures; looking at them is the rest of
the check.

USAGE (from a consuming game, whose own tool supplies the title module and product environment)
  uv run --frozen python tools/picture_oracle.py --bios ../SCPH1001.BIN
  uv run --frozen python tools/picture_oracle.py --bios ../SCPH1001.BIN --selftest
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from dataclasses import dataclass
from pathlib import Path

from compare import (
    CARD_ENV,
    PSXPORT,
    Driver,
    Playback,
    Product,
    Title,
    build_parser,
    fresh_card,
    match_console_card,
    recorded_route,
    snapshot,
)
from compare import compare as compare_state
from compare_cores import ConsoleSession, CoreError, CoreSession, NativeReplSession

try:
    from PIL import Image
except ImportError:  # pragma: no cover - the message is the whole value
    sys.exit("picture: needs Pillow (it is a declared dependency; run through uv)")

TILE = 16

# The product settings a picture comparison must run under; the file states why. Consumers pass this
# to their launch environment instead of their shipping settings, so the reasoning is written once.
REFERENCE_SETTINGS = Path(__file__).resolve().parent / "reference_settings.ini"

# One representable step of PSX colour, in the 8-bit values these PNGs carry. The console composits
# in 15 bits per pixel, so a channel takes 32 values spaced 255/31 = 8.22 apart. Two renderers that
# round or dither the same 15-bit colour differently therefore differ by EXACTLY one step, in a
# pixel a player cannot tell apart; anything larger is a different colour, not a different rounding.
#
# Measured on Spyro 1's settled_play, 2026-09-20: 54.62% of pixels differed at all, but the
# magnitudes were banded on multiples of 8 and 29.45% of the frame differed by exactly one step.
# Only 8.28% differed by more than four. A single count conflates the two, so "54.62%" read as a
# rendering verdict when most of it was rounding -- and an object drawn in the wrong place, which is
# what docs/issues/0120 is looking for, would have been invisible inside it.
COLOUR_STEP = 8
SIGNIFICANT = COLOUR_STEP  # a difference must EXCEED one step to count as a different colour


@dataclass(frozen=True)
class Picture:
    """One captured frame and the facts every refusal below is decided on."""

    path: Path
    size: tuple[int, int]
    distinct_colours: int
    non_black: int
    modal_share: float  # how much of the frame the single commonest colour covers

    # A frame more than half of which is ONE colour carries almost nothing to compare, exactly as a
    # blank one does, and `uniform` misses it because a fade is not a single colour -- it is one
    # colour plus faint tints. Measured on Spyro 1, 2026-09-20: at f180 of the played route the two
    # cores were mid-WHITEOUT with every decisive range equal, the reference 56.76% one colour and
    # the product 87.76%, and the comparison reported 23.94% "a different COLOUR" -- a number about
    # two flashes, not about rendering. Real Artisans scenes in the same run measured 6.67% to
    # 14.82%, the 6.67% being the letterbox bar, so "over half the frame is one colour" separates
    # them by a factor of three and is a statement rather than a fitted threshold.
    WASHED_OUT = 0.5

    @property
    def blank(self) -> bool:
        return self.non_black == 0

    @property
    def uniform(self) -> bool:
        return self.distinct_colours <= 1

    @property
    def washed_out(self) -> bool:
        return self.modal_share > self.WASHED_OUT

    @classmethod
    def load(cls, path: Path) -> "Picture":
        if not path.is_file():
            raise CoreError(f"no capture at {path}")
        with Image.open(path) as handle:
            image = handle.convert("RGB")
            colours = image.getcolors(maxcolors=1 << 20)
            non_black = sum(count for count, pixel in colours if pixel != (0, 0, 0))
            pixels = image.size[0] * image.size[1]
            # Counted on the console's own 15-bit colour grid: a fade dithers, and on raw 8-bit
            # values its neighbouring tints would each be a separate colour, so a frame that is
            # visibly one colour would not look like one here.
            quantised: dict[tuple[int, ...], int] = {}
            for count, pixel in colours:
                key = tuple(channel // COLOUR_STEP for channel in pixel)
                quantised[key] = quantised.get(key, 0) + count
            modal = max(quantised.values()) / pixels if pixels else 0.0
            return cls(path, image.size, len(colours), non_black, modal)


@dataclass(frozen=True)
class PictureDiff:
    """How two pictures of the same moment differ, per pixel, by magnitude, and per tile.

    `differing` counts any inequality at all and `significant` counts the pixels whose colour really
    differs (see COLOUR_STEP). Both are reported: the first says whether the frames are bit-identical,
    which is the only thing a selftest may key on, and the second is the one to read as rendering."""

    pixels: int
    differing: int
    significant: int
    magnitudes: tuple[tuple[int, int], ...]  # (threshold, pixels differing by MORE than it)
    worst_tiles: tuple[tuple[tuple[int, int], int], ...]
    tiles_touched: int
    tiles_total: int

    THRESHOLDS = (COLOUR_STEP, 2 * COLOUR_STEP, 4 * COLOUR_STEP, 8 * COLOUR_STEP)

    @property
    def share(self) -> float:
        return self.differing / self.pixels if self.pixels else 0.0

    @property
    def significant_share(self) -> float:
        return self.significant / self.pixels if self.pixels else 0.0

    @property
    def concentrated(self) -> bool:
        """Is the SIGNIFICANT difference in a few places rather than spread over the frame?

        A renderer difference (dither, sub-pixel sampling) touches most tiles a little. A defect —
        a missing panel, a mislaid menu — touches few tiles a lot. This is the discriminator that
        makes the number actionable, and it is deliberately not a verdict on its own. It is computed
        over significant pixels only: ranked by bare inequality, every tile that merely rounds
        differently competes with the tile that actually lost an object."""
        return self.significant > 0 and self.tiles_touched * 4 <= self.tiles_total


# How the magnitude map paints each pixel. A percentage says how much of the frame differs; only the
# map says WHERE and HOW BADLY, and that is the difference between "edges and dither everywhere" and
# "one object is in the wrong place", which is the question Spyro's issue 0120 asks of every scene.
MAGNITUDE_BANDS = (
    (SIGNIFICANT, (0, 0, 0)),             # rounding: a step or less, invisible to a player
    (4 * COLOUR_STEP, (0, 0, 160)),       # a shade out
    (12 * COLOUR_STEP, (220, 180, 0)),    # a different colour
)
MAGNITUDE_WORST = (255, 0, 0)


def _band(magnitude: int) -> tuple[int, int, int]:
    for limit, colour in MAGNITUDE_BANDS:
        if magnitude <= limit:
            return colour
    return MAGNITUDE_WORST


def compare_pictures(native: Path, console: Path, magnitude_map: Path | None = None) -> PictureDiff:
    """Compare two captures, and optionally paint where and how badly they differ.

    The map is written for a human to look at, which the printed numbers exist to send them to."""
    with Image.open(native) as a_handle, Image.open(console) as b_handle:
        a, b = a_handle.convert("RGB"), b_handle.convert("RGB")
        width, height = a.size
        a_pixels, b_pixels = a.load(), b.load()
        per_tile: dict[tuple[int, int], int] = {}
        differing = 0
        beyond = {threshold: 0 for threshold in PictureDiff.THRESHOLDS}
        painted = Image.new("RGB", (width, height)) if magnitude_map else None
        paint = painted.load() if painted else None
        for y in range(height):
            for x in range(width):
                here, there = a_pixels[x, y], b_pixels[x, y]
                if here == there:
                    continue
                differing += 1
                magnitude = max(abs(one - other) for one, other in zip(here, there))
                if paint is not None:
                    paint[x, y] = _band(magnitude)
                for threshold in beyond:
                    if magnitude > threshold:
                        beyond[threshold] += 1
                if magnitude > SIGNIFICANT:
                    key = (x // TILE * TILE, y // TILE * TILE)
                    per_tile[key] = per_tile.get(key, 0) + 1
        if painted is not None:
            painted.save(magnitude_map)
        tiles_total = ((width + TILE - 1) // TILE) * ((height + TILE - 1) // TILE)
        worst = tuple(sorted(per_tile.items(), key=lambda item: -item[1])[:8])
        magnitudes = tuple((threshold, beyond[threshold]) for threshold in PictureDiff.THRESHOLDS)
        return PictureDiff(width * height, differing, beyond[SIGNIFICANT], magnitudes, worst,
                           len(per_tile), tiles_total)


class PictureRun:
    """Drives both cores through the title's checkpoints, capturing and comparing at each one."""

    def __init__(self, title: Title, native: CoreSession, console: CoreSession, out_dir: Path, report: dict):
        self.title = title
        self.driver = Driver(title)
        self.native = native
        self.console = console
        self.out_dir = out_dir
        self.report = report

    # How far a core may be advanced, with no input, to reach a frame it is actually PRESENTING.
    #
    # A checkpoint predicate is satisfied by GUEST STATE, and a title can enter that state well
    # before it draws anything. The two cores then arrive at very different moments, because the
    # product's host-file I/O completes far sooner than the reference's emulated disc. Measured
    # 2026-09-19: all three of Tomba! 2's checkpoints refused as blank on one side -- the product
    # had the complete title screen at game frame 27 while the reference was still black at 302 --
    # so the tool produced no comparison at all, on either side of the question. Spyro 1's `playing`
    # photographed the reference on its intro card and the product already in the lit courtyard.
    #
    # A blank refusal is a dead end: it says neither "these match" nor "these differ". Advancing to
    # a presented frame turns it into one of the two answers the run can act on -- a real comparison,
    # or a state divergence named by range.
    PRESENT_BUDGET = 600
    PRESENT_STEP = 10

    def reach(self, checkpoint, budget: int) -> None:
        settle = None
        for core in (self.console, self.native):
            used, settle = checkpoint.reach(self.driver, core, budget, settle)
            print(f"[picture] {core.name}: {checkpoint.name} after {used} game frames")
        if checkpoint.not_picture_comparable:
            # Advancing to a presented frame is a per-core, unequal advance made solely so a photo
            # can be taken. A checkpoint that will not be photographed must not pay it, or the two
            # cores would leave it at different game frames for no reason at all.
            return
        self.report.setdefault("presented", {})[checkpoint.name] = {
            core.name: self.advance_to_presented(checkpoint.name, core)
            for core in (self.console, self.native)
        }

    def advance_to_presented(self, name: str, core: CoreSession) -> dict:
        """Advance `core` with no input until it presents a SCENE, and say what it took.

        A scene is a frame that is neither blank nor mostly one colour. "Non-blank" was the old
        condition and it is the wrong one: the first non-blank frame of a fade-in is the fade, so
        this stopped each core at whatever point of one it happened to reach and handed `at` two
        frames that differ by fade phase. Measured 2026-09-20, all three of Tomba! 2's checkpoints
        refused that way -- game_stage's reference 99.53% one colour, field's product 51.15%,
        free_roam's product 76.41%, the last a recognisable scene still fading up.

        Reports the frames spent and the frames SCANNED even when no scene was ever reached, and
        which condition was still failing, so a core that never draws is distinguishable from one
        that never finished fading and from one that was never looked at. Returns rather than
        raises: `at` owns the refusal, and a core that never presents still has to be reported
        beside the one that did.
        """
        probe = self.out_dir / f"{name}.{core.name}.presented.png"
        scanned = 0
        last = None
        for advanced in range(0, self.PRESENT_BUDGET + 1, self.PRESENT_STEP):
            core.capture(probe)
            scanned += 1
            last = Picture.load(probe)
            if not last.blank and not last.washed_out:
                if advanced:
                    print(f"[picture] {core.name}: {name} presented after {advanced} more game frame(s)")
                return {"presented": True, "extra_frames": advanced, "probes": scanned,
                        "modal_share": round(last.modal_share, 4)}
            core.hold(frozenset())
            self.title.advance(core, self.PRESENT_STEP)
        stuck = "blank" if last is None or last.blank else (
            f"{100 * last.modal_share:.2f}% a single colour")
        print(f"[picture] {core.name}: {name} NEVER presented a scene — {scanned} probe(s) over "
              f"{self.PRESENT_BUDGET} game frames, still {stuck}", file=sys.stderr)
        return {"presented": False, "extra_frames": self.PRESENT_BUDGET, "probes": scanned,
                "still": stuck, "modal_share": round(last.modal_share, 4) if last else None}

    def play(self, frames: int, segments=None, frame_step: int = 0, label: str = "played") -> bool:
        """Run a held-input schedule on both cores and compare the picture along the way. `segments`
        defaults to the title's own scripted route; a recorded replay can stand in for it.

        `frame_step` compares every N frames INSIDE the segment as well as at its end. One picture at
        the end of a long route can only answer for the moment the route happens to stop at, which on
        Spyro's recorded Artisans replay is a cutscene close-up with nothing of the scene in view; a
        defect that appears while the player walks past it is invisible to that single comparison.
        Stepping is what turns one sample into a series, and it is the same knob name the RAM
        comparison uses for the same reason.
        """
        segments = self.title.gameplay if segments is None else segments
        schedule = [buttons for buttons, count in segments for _ in range(count)]
        players = [Playback(self.driver, core, schedule) for core in (self.native, self.console)]
        ok = True
        total = min(frames, len(schedule))
        for done in range(1, total + 1):
            for player in players:
                player.step()
            if frame_step > 0 and done % frame_step == 0 and done != total:
                ok = self.at(f"{label}-f{done}") and ok
        return ok

    def capture(self, name: str) -> tuple[Picture, Picture]:
        pictures = []
        for core in (self.native, self.console):
            path = self.out_dir / f"{name}.{core.name}.png"
            core.capture(path)
            pictures.append(Picture.load(path))
        return self._align(*pictures)

    def _align(self, native: Picture, console: Picture) -> tuple[Picture, Picture]:
        """Crop the product to the rows a console would scan out, which it reported itself.

        The native render path deliberately presents more rows than the console showed — the port
        declares the real count and the framework keeps drawing the rest (psxport gpu_native.cpp,
        USER 2026-08-19: "PC is fine, oracle isn't"). Comparing the raw frames therefore measures a
        difference nobody considers a defect, and on Tomba! 2 that alone accounted for a 92% pixel
        difference and an apparent 7-pixel vertical offset that was not one.

        The crop count comes from the PRODUCT (`guest_scan=` on its shot reply, from the GPU state),
        never from fitting the pictures to each other. The reference is asked for its own active
        area (`crop_overscan=smart`), so both sides state their geometry and neither is inferred.
        """
        rows = getattr(self.native, "scan_rows", None)
        if not rows or rows >= native.size[1]:
            return native, console
        cropped = native.path.with_suffix(".scanned.png")
        with Image.open(native.path) as handle:
            handle.convert("RGB").crop((0, 0, native.size[0], rows)).save(cropped)
        return Picture.load(cropped), console

    def state_divergence(self) -> tuple[list[dict], list[dict], list[dict]]:
        """Every declared range that disagrees between the two cores right now, split into the ones
        that invalidate a PICTURE comparison and the ones merely worth reporting.

        Read through `compare.snapshot`/`compare.compare` -- the same owner the RAM comparison gates
        on -- so there is one definition of what "the same state" means and no second copy to drift.

        WHY THE PICTURE'S SET IS NOT THE RAM COMPARISON'S. `DeclaredRange.decisive` answers "may the
        simulation differ here", and a title marks the camera informational precisely because a small
        camera difference does not change whether the game BEHAVES. It changes every pixel. Measured
        on Spyro 1 (2026-09-19): at a dragon-cutscene frame with gamestate, level, game_tick,
        state_switch and player.position all equal, the product framed the dragon about 25px left and
        20px below the reference and 87% of pixels differed -- one camera difference, wearing the
        costume of a rendering defect. A title therefore declares `picture_decisive` for the ranges
        that must match before a pixel count means anything; without it the RAM set is used, which is
        the old behaviour.

        Returns (blocking, differing, decisive). The third list is EVERY picture-decisive range with
        its value, agreeing ones included, because a report that prints only what differs cannot be
        told apart from one whose ranges are inert. Measured on Tomba! 2 (2026-09-19): four ranges
        were added to its picture set and two of them -- the fade sequencer's outer state and its ramp
        level -- agreed at every checkpoint. Reading the values showed why, and it was not that the
        two cores' fades matched. Without them in the report, "no fade row" and "this range is never
        populated here" are the same silence."""
        native = snapshot(self.title, self.native)
        console = snapshot(self.title, self.console)
        names = getattr(self.title, "picture_decisive", None)
        everything = compare_state(self.title, native, console)
        chosen = (lambda row: row["range"] in names) if names is not None else (lambda row: row["decisive"])
        rows = [row for row in everything if not row["equal"]]
        blocking = [row for row in rows if chosen(row)]
        return blocking, rows, [row for row in everything if chosen(row)]

    def withhold(self, checkpoint) -> None:
        """Record that this checkpoint was reached and deliberately not photographed, and why.

        A skipped checkpoint must still appear in the report: silence here would be
        indistinguishable from a checkpoint the run never got to, and the reason is the finding."""
        print(f"[picture] {checkpoint.name}: not photographed — {checkpoint.not_picture_comparable}")
        self.report["pictures"].append({"checkpoint": checkpoint.name, "photographed": False,
                                        "withheld": checkpoint.not_picture_comparable})

    def at(self, name: str) -> bool:
        """Capture both cores here and report. Returns whether the pictures are comparable AND
        matched well enough not to name a defect; a refusal is False and says why."""
        diverged, all_diverged, decisive_state = self.state_divergence()
        native, console = self.capture(name)
        row: dict = {
            "checkpoint": name,
            "native": {"path": str(native.path), "size": list(native.size), "non_black": native.non_black,
                       "colours": native.distinct_colours},
            "console": {"path": str(console.path), "size": list(console.size), "non_black": console.non_black,
                        "colours": console.distinct_colours},
        }
        # Recorded before any refusal below can return: the values the comparison was gated on are
        # evidence whichever way the checkpoint goes, and a refusal is exactly when they are wanted.
        row["decisive_state"] = decisive_state
        self.report["pictures"].append(row)
        for label, picture in (("product", native), ("reference", console)):
            if picture.blank or picture.uniform:
                row["refused"] = (f"the {label} picture at {name} is "
                                  f"{'blank' if picture.blank else 'a single colour'} "
                                  f"({picture.non_black}/{picture.size[0] * picture.size[1]} non-black, "
                                  f"{picture.distinct_colours} distinct colour(s)), so there is nothing to "
                                  f"compare and a zero difference would not be a match")
                print(f"[picture] {name}: REFUSED — {row['refused']}", file=sys.stderr)
                return False
        for label, picture in (("product", native), ("reference", console)):
            if picture.washed_out:
                row["refused"] = (f"{100 * picture.modal_share:.2f}% of the {label} picture at {name} "
                                  f"is a single colour, so it is a fade or a flash rather than a scene "
                                  f"and a difference here would be about which moment of it each core "
                                  f"is on. The cores can be at the same declared state and different "
                                  f"points of the same whiteout: that phase is in no declared range")
                print(f"[picture] {name}: REFUSED — {row['refused']}", file=sys.stderr)
                return False
        if native.size != console.size:
            row["refused"] = (f"the product presents {native.size[0]}x{native.size[1]} and the reference "
                              f"{console.size[0]}x{console.size[1]}; scaling one onto the other would invent "
                              f"the pixels this tool then measured. Compare the product's 4:3 output here")
            print(f"[picture] {name}: REFUSED — {row['refused']}", file=sys.stderr)
            return False
        if all_diverged:
            row["state_divergence"] = all_diverged
        if diverged:
            names = ", ".join(f"{d['range']} (+{d['first_diff_offset']}: "
                              f"native {d['native_byte']:02X} console {d['console_byte']:02X})"
                              for d in diverged)
            row["refused"] = (f"the two cores are not at the same guest state here, so a picture "
                              f"difference would not be about rendering: {names}. Drive both to a "
                              f"state the RAM comparison calls equal before photographing them")
            print(f"[picture] {name}: REFUSED — {row['refused']}", file=sys.stderr)
            return False
        magnitude_map = self.out_dir / f"{name}.magnitude.png"
        diff = compare_pictures(native.path, console.path, magnitude_map)
        row["magnitude_map"] = str(magnitude_map)
        row["diff"] = {"pixels": diff.pixels, "differing": diff.differing, "share": round(diff.share, 6),
                       "significant": diff.significant,
                       "significant_share": round(diff.significant_share, 6),
                       "colour_step": COLOUR_STEP,
                       "beyond": [{"threshold": threshold, "pixels": count}
                                  for threshold, count in diff.magnitudes],
                       "tiles_touched": diff.tiles_touched, "tiles_total": diff.tiles_total,
                       "concentrated": diff.concentrated,
                       "worst_tiles": [{"tile": list(tile), "differing": count} for tile, count in diff.worst_tiles]}
        shape = ("nothing beyond rounding" if not diff.significant
                 else "CONCENTRATED" if diff.concentrated else "spread")
        print(f"[picture] {name}: {diff.significant}/{diff.pixels} pixels are a different COLOUR "
              f"({100 * diff.significant_share:.2f}%), {diff.tiles_touched}/{diff.tiles_total} tiles "
              f"touched — {shape}")
        # The bare count is kept beside it rather than replaced: it is the difference between "these
        # frames are bit-identical" and "these frames agree to within rounding", and only the first
        # clears a comparator selftest.
        rounding = diff.differing - diff.significant
        print(f"[picture] {name}: {diff.differing} differ at all; {rounding} of those by one "
              f"{COLOUR_STEP}-value colour step or less (rounding/dither, not rendering)")
        if diff.worst_tiles:
            worst = ", ".join(f"({tile[0]},{tile[1]}):{count}" for tile, count in diff.worst_tiles[:4])
            print(f"[picture]   worst tiles {worst}")
        return True

    def selftest(self) -> bool:
        """Show the other answer before any zero is believed.

        Two captures of the SAME core at two different states must differ, and a capture compared
        with itself must not. A comparator that cannot produce both is not measuring the pictures."""
        first, _ = self.capture("selftest-before")
        self.play(60)
        second, _ = self.capture("selftest-after")
        moved = compare_pictures(first.path, second.path)
        same = compare_pictures(first.path, first.path)
        detected = moved.differing > 0 and same.differing == 0
        self.report["selftest"] = {"changed_pixels": moved.differing, "self_compare": same.differing,
                                   "detected": detected}
        print(f"[picture] selftest: 60 frames of the title's own route changed {moved.differing} pixel(s), "
              f"and a picture compared with itself differs in {same.differing} — comparator "
              f"{'DETECTED both answers' if detected else 'FAILED'}")
        return detected


def launch_picture_sessions(product: Product, args: argparse.Namespace,
                            out_dir: Path) -> tuple[CoreSession, CoreSession]:
    """Like compare.launch_sessions, but the reference publishes its active display area so both
    cores present the same rect. Nothing else differs, which is why this is the only thing here."""
    native = NativeReplSession(str(product.binary), str(product.executable), product.environment,
                               str(product.cwd), out_dir / "native.log")
    console = ConsoleSession(PSXPORT, product.disc, args.bios, args.region, out_dir / "console.log",
                             crop_overscan=True)
    return native, console


def run(title: Title, product: Product, args: argparse.Namespace, out_dir: Path,
        sessions=launch_picture_sessions) -> int:
    out_dir.mkdir(parents=True, exist_ok=True)
    if not args.bios.is_file():
        print(f"REFUSED: BIOS image {args.bios} does not exist; pass --bios <SCPH1001.BIN>", file=sys.stderr)
        return 2
    if not product.binary.is_file() or not product.executable.is_file():
        print(f"REFUSED: {product.binary} or {product.executable} is missing; build the product first",
              file=sys.stderr)
        return 2
    if not title.checkpoints:
        print(f"REFUSED: {title.name} declares no checkpoints; a picture comparison needs a state to "
              f"compare at", file=sys.stderr)
        return 2
    report = {"title": title.name, "binary": str(product.binary), "disc": str(product.disc),
              "bios": str(args.bios), "product_env": dict(getattr(args, "product_env_pairs", {})),
              "pictures": [], "complete": False}
    started = time.monotonic()
    native = console = None
    try:
        product = fresh_card(product, out_dir)
        report["product_card"] = product.environment[CARD_ENV]
        native, console = sessions(product, args, out_dir)
        report["console_card"] = match_console_card(console, getattr(args, "console_card", None))
        run_state = PictureRun(title, native, console, out_dir, report)
        ok = True
        for checkpoint in title.checkpoints:
            run_state.reach(checkpoint, args.budget)
            if args.selftest and checkpoint is title.checkpoints[-1]:
                report["complete"] = True
                return 0 if run_state.selftest() else 1
            if checkpoint.not_picture_comparable:
                run_state.withhold(checkpoint)
                continue
            ok = run_state.at(checkpoint.name) and ok
        route = getattr(args, "route", None)
        if route and not args.play:
            # A SILENTLY IGNORED INPUT IS A FAILURE, NOT A FILTER. --route only feeds the post-checkpoint
            # gameplay segment, so without --play it changed nothing and the run printed the ordinary
            # checkpoint comparison as if it had honoured the flag — measured 2026-09-19, where a
            # 113-second run over a recorded Artisans replay reported the same two checkpoint numbers as
            # a run with no route at all, and only the identical figures gave it away.
            print(f"REFUSED: --route {route} needs --play N; the recorded route drives the gameplay "
                  f"segment AFTER the last checkpoint, and with --play 0 it would have been ignored",
                  file=sys.stderr)
            return 2
        segments = recorded_route(route, getattr(args, "route_from", 0)) if route else None
        if args.play:
            label = f"{route.stem}-{args.play}f" if route else f"played-{args.play}f"
            ok = run_state.play(args.play, segments, args.frame_step, label) and ok
            ok = run_state.at(label) and ok
        report["complete"] = True
        return 0 if ok else 1
    except CoreError as error:
        report["error"] = str(error)
        print(f"[picture] FAILED: {error}", file=sys.stderr)
        return 1
    finally:
        for core in (native, console):
            if core is not None:
                core.close()
        report["seconds"] = round(time.monotonic() - started, 1)
        path = out_dir / ("picture_selftest.json" if args.selftest else "picture.json")
        path.write_text(json.dumps(report, indent=2))
        print(f"[picture] report: {path} ({report['seconds']}s)")
        print(f"[picture] PNGs for a human to judge: {out_dir}/*.png")
        print("[picture] LOOKING AT THEM IS THE REST OF THE CHECK — a pixel count is not a verdict.")


def picture_parser(description: str, default_bios: Path) -> argparse.ArgumentParser:
    """The RAM comparison's arguments plus this tool's own, so a consumer wires one parser."""
    parser = build_parser(description, default_bios)
    parser.add_argument("--play", type=int, default=0,
                        help="after the last checkpoint, run N frames of the title's gameplay "
                             "schedule and compare the picture there too")
    return parser
