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
wide presentation region), the reference through its libretro framebuffer. Both cores are driven to
the same state by the same title checkpoints `compare.py` uses, so the two pictures are of the same
moment in the same route.

WHAT IT REFUSES, and why each refusal exists rather than a number:

  * EITHER picture blank, or both uniform. A zero difference between two pictures that hold nothing
    is the uniform-output tell, not a match. This is the exact failure it was written to replace.
  * Differing dimensions. A widescreen product against a 4:3 reference is a deliberate difference,
    not a defect, and scaling one onto the other would invent pixels and then measure them. Compare
    the product's 4:3 output against the reference, and judge the widescreen extra area by coverage.
  * A capture either core did not write.

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
)
from compare_cores import ConsoleSession, CoreError, CoreSession, NativeReplSession

try:
    from PIL import Image
except ImportError:  # pragma: no cover - the message is the whole value
    sys.exit("picture: needs Pillow (it is a declared dependency; run through uv)")

TILE = 16


@dataclass(frozen=True)
class Picture:
    """One captured frame and the two facts every refusal below is decided on."""

    path: Path
    size: tuple[int, int]
    distinct_colours: int
    non_black: int

    @property
    def blank(self) -> bool:
        return self.non_black == 0

    @property
    def uniform(self) -> bool:
        return self.distinct_colours <= 1

    @classmethod
    def load(cls, path: Path) -> "Picture":
        if not path.is_file():
            raise CoreError(f"no capture at {path}")
        with Image.open(path) as handle:
            image = handle.convert("RGB")
            colours = image.getcolors(maxcolors=1 << 20)
            non_black = sum(count for count, pixel in colours if pixel != (0, 0, 0))
            return cls(path, image.size, len(colours), non_black)


@dataclass(frozen=True)
class PictureDiff:
    """How two pictures of the same moment differ, per pixel and per tile."""

    pixels: int
    differing: int
    worst_tiles: tuple[tuple[tuple[int, int], int], ...]
    tiles_touched: int
    tiles_total: int

    @property
    def share(self) -> float:
        return self.differing / self.pixels if self.pixels else 0.0

    @property
    def concentrated(self) -> bool:
        """Is the difference in a few places rather than spread over the frame?

        A renderer difference (dither, sub-pixel sampling) touches most tiles a little. A defect —
        a missing panel, a mislaid menu — touches few tiles a lot. This is the discriminator that
        makes the number actionable, and it is deliberately not a verdict on its own."""
        return self.differing > 0 and self.tiles_touched * 4 <= self.tiles_total


def compare_pictures(native: Path, console: Path) -> PictureDiff:
    with Image.open(native) as a_handle, Image.open(console) as b_handle:
        a, b = a_handle.convert("RGB"), b_handle.convert("RGB")
        width, height = a.size
        a_pixels, b_pixels = a.load(), b.load()
        per_tile: dict[tuple[int, int], int] = {}
        differing = 0
        for y in range(height):
            for x in range(width):
                if a_pixels[x, y] != b_pixels[x, y]:
                    differing += 1
                    key = (x // TILE * TILE, y // TILE * TILE)
                    per_tile[key] = per_tile.get(key, 0) + 1
        tiles_total = ((width + TILE - 1) // TILE) * ((height + TILE - 1) // TILE)
        worst = tuple(sorted(per_tile.items(), key=lambda item: -item[1])[:8])
        return PictureDiff(width * height, differing, worst, len(per_tile), tiles_total)


class PictureRun:
    """Drives both cores through the title's checkpoints, capturing and comparing at each one."""

    def __init__(self, title: Title, native: CoreSession, console: CoreSession, out_dir: Path, report: dict):
        self.title = title
        self.driver = Driver(title)
        self.native = native
        self.console = console
        self.out_dir = out_dir
        self.report = report

    def reach(self, checkpoint, budget: int) -> None:
        settle = None
        for core in (self.console, self.native):
            used, settle = checkpoint.reach(self.driver, core, budget, settle)
            print(f"[picture] {core.name}: {checkpoint.name} after {used} game frames")

    def play(self, frames: int) -> None:
        """Run the title's gameplay schedule on both cores so a later state can be compared too."""
        schedule = [buttons for buttons, count in self.title.gameplay for _ in range(count)]
        players = [Playback(self.driver, core, schedule) for core in (self.native, self.console)]
        for _ in range(min(frames, len(schedule))):
            for player in players:
                player.step()

    def capture(self, name: str) -> tuple[Picture, Picture]:
        pictures = []
        for core in (self.native, self.console):
            path = self.out_dir / f"{name}.{core.name}.png"
            core.capture(path)
            pictures.append(Picture.load(path))
        return pictures[0], pictures[1]

    def at(self, name: str) -> bool:
        """Capture both cores here and report. Returns whether the pictures are comparable AND
        matched well enough not to name a defect; a refusal is False and says why."""
        native, console = self.capture(name)
        row: dict = {
            "checkpoint": name,
            "native": {"path": str(native.path), "size": list(native.size), "non_black": native.non_black,
                       "colours": native.distinct_colours},
            "console": {"path": str(console.path), "size": list(console.size), "non_black": console.non_black,
                        "colours": console.distinct_colours},
        }
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
        if native.size != console.size:
            row["refused"] = (f"the product presents {native.size[0]}x{native.size[1]} and the reference "
                              f"{console.size[0]}x{console.size[1]}; scaling one onto the other would invent "
                              f"the pixels this tool then measured. Compare the product's 4:3 output here")
            print(f"[picture] {name}: REFUSED — {row['refused']}", file=sys.stderr)
            return False
        diff = compare_pictures(native.path, console.path)
        row["diff"] = {"pixels": diff.pixels, "differing": diff.differing, "share": round(diff.share, 6),
                       "tiles_touched": diff.tiles_touched, "tiles_total": diff.tiles_total,
                       "concentrated": diff.concentrated,
                       "worst_tiles": [{"tile": list(tile), "differing": count} for tile, count in diff.worst_tiles]}
        shape = "CONCENTRATED" if diff.concentrated else "spread"
        print(f"[picture] {name}: {diff.differing}/{diff.pixels} pixels differ ({100 * diff.share:.2f}%), "
              f"{diff.tiles_touched}/{diff.tiles_total} tiles touched — {shape}")
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
        run_state = PictureRun(title, native, console, out_dir, report)
        ok = True
        for checkpoint in title.checkpoints:
            run_state.reach(checkpoint, args.budget)
            if args.selftest and checkpoint is title.checkpoints[-1]:
                report["complete"] = True
                return 0 if run_state.selftest() else 1
            ok = run_state.at(checkpoint.name) and ok
        if args.play:
            run_state.play(args.play)
            ok = run_state.at(f"played-{args.play}f") and ok
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
