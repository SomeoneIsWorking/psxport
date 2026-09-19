#!/usr/bin/env python3
"""Does widescreen EXTEND the picture, or change it?

The state oracle answers "is the simulation unchanged with widescreen on" and it is structurally
blind to this: a port that stretched its 4:3 frame to 16:9, or smeared the edge column into the
margins, writes exactly the same guest state as one that renders the extra world. Measured on Spyro
1 (2026-09-20), 15/15 checkpoints were byte-identical at render_width=684 -- true, and no evidence
at all about the picture.

A console comparison cannot answer it either, because the reference IS 4:3: there is nothing on the
console to compare the extra area against. So the question is asked of the product against ITSELF,
at one state, under two settings. Widescreen is defined as a deterministic horizontal extension
about the same centre, which makes two things checkable without a reference:

  the CENTRE must survive. The central `narrow_width` columns of the wide frame are the same
    geometry, the same projection and the same scale as the 4:3 frame, so they must agree. A stretch
    fails here and fails loudly: measured, a nearest-neighbour stretch of the same frame reads
    53.91% of the centre a different colour against the real extension's 2.17%.
  the MARGINS must contain scene. Additional coverage means additional world, so a margin that is
    black, one flat colour, or a copy of the edge column is not coverage. Measured on the real
    extension: 93.3% non-black, ~580 distinct colours, and 0 of 85 columns identical to the first.

Neither check says the extra geometry is CORRECT -- nothing available here can, short of a wide
reference that does not exist. They say it is present, and that the original picture was not
resampled to make room for it, which is what "widescreen, not stretching" means and what no other
instrument in this project measures.
"""

from __future__ import annotations

import sys
from dataclasses import dataclass
from pathlib import Path

from picture import COLOUR_STEP, Picture, PictureDiff, compare_pictures

try:
    from PIL import Image
except ImportError:  # pragma: no cover - the launcher pins Pillow
    sys.exit("widescreen: needs Pillow (it is a declared dependency; run through uv)")


class Unanswerable(Exception):
    """The two captures cannot be compared, said as a refusal rather than a number."""


@dataclass(frozen=True)
class Margin:
    """One side panel of the widened frame."""

    side: str
    width: int
    height: int
    non_black: int
    distinct_colours: int
    repeated_columns: int  # columns identical to the panel's first; a smear repeats all of them

    @property
    def non_black_share(self) -> float:
        return self.non_black / (self.width * self.height) if self.width and self.height else 0.0

    @property
    def carries_scene(self) -> bool:
        """Present, varied, and not one column smeared across the panel."""
        return (self.non_black_share > 0.5 and self.distinct_colours > 16
                and self.repeated_columns < self.width - 1)


@dataclass(frozen=True)
class Extension:
    """What the wide frame did to the narrow one."""

    narrow_size: tuple[int, int]
    wide_size: tuple[int, int]
    margin: int
    centre: PictureDiff
    margins: tuple[Margin, ...]
    centre_path: Path

    # How much of the centre may differ and still be the same picture. The residual is screen-space
    # dither: the pattern is a function of x, and every pixel of the centre moves by `margin`
    # columns, so its phase changes. Measured on Spyro 1: 2.17% of the centre a different colour,
    # NONE of it within 8 columns of either crop edge -- interior, as a phase shift is, and not the
    # boundary artefact a mis-centred crop would give. A stretch of the same frame reads 53.91%, so
    # this separates the two by a factor of twenty-five and is not a fitted threshold.
    CENTRE_TOLERANCE = 0.10

    @property
    def centre_survived(self) -> bool:
        return self.centre.significant_share <= self.CENTRE_TOLERANCE

    @property
    def extends(self) -> bool:
        return self.centre_survived and all(m.carries_scene for m in self.margins)

    def report(self) -> dict:
        return {
            "narrow_size": list(self.narrow_size), "wide_size": list(self.wide_size),
            "margin": self.margin, "colour_step": COLOUR_STEP,
            "centre": {"path": str(self.centre_path), "pixels": self.centre.pixels,
                       "differing": self.centre.differing, "significant": self.centre.significant,
                       "significant_share": round(self.centre.significant_share, 6),
                       "survived": self.centre_survived,
                       "tolerance": self.CENTRE_TOLERANCE},
            "margins": [{"side": m.side, "size": [m.width, m.height], "non_black": m.non_black,
                         "non_black_share": round(m.non_black_share, 4),
                         "distinct_colours": m.distinct_colours,
                         "repeated_columns": m.repeated_columns,
                         "carries_scene": m.carries_scene} for m in self.margins],
            "extends": self.extends,
        }


def _margin(image: Image.Image, side: str, x0: int, width: int) -> Margin:
    panel = image.crop((x0, 0, x0 + width, image.size[1]))
    colours = panel.getcolors(maxcolors=1 << 20)
    pixels = panel.load()
    height = panel.size[1]
    repeated = sum(1 for dx in range(1, width)
                   if all(pixels[dx, y] == pixels[0, y] for y in range(height)))
    return Margin(side, width, height,
                  sum(count for count, colour in colours if colour != (0, 0, 0)),
                  len(colours), repeated)


def analyse(narrow: Path, wide: Path, out_dir: Path) -> Extension:
    """Compare a 4:3 capture against a 16:9 capture of the SAME state."""
    for path in (narrow, wide):
        if not path.is_file():
            raise Unanswerable(f"no capture at {path}")
    with Image.open(narrow) as a_handle, Image.open(wide) as b_handle:
        a, b = a_handle.convert("RGB"), b_handle.convert("RGB")
        if a.size[1] != b.size[1]:
            raise Unanswerable(
                f"the 4:3 capture is {a.size[0]}x{a.size[1]} and the wide one {b.size[0]}x{b.size[1]}: "
                f"widescreen changes the horizontal projection, so a differing HEIGHT means these are "
                f"not the same picture and cropping one to the other would compare different rows")
        if b.size[0] <= a.size[0]:
            raise Unanswerable(
                f"the wide capture is {b.size[0]} wide against the 4:3 capture's {a.size[0]}, so it "
                f"did not widen. Read the product's `[wide] native picture:` line: aspect=3 is AUTO "
                f"and resolves to 4:3 in a headless run, which is not a widescreen capture")
        if (b.size[0] - a.size[0]) % 2:
            raise Unanswerable(
                f"the wide capture is {b.size[0] - a.size[0]} pixels wider, an ODD number, so there "
                f"is no centred crop and any choice would bias one side by half a pixel")
        margin = (b.size[0] - a.size[0]) // 2
        out_dir.mkdir(parents=True, exist_ok=True)
        centre_path = out_dir / f"{wide.stem}.centre.png"
        b.crop((margin, 0, margin + a.size[0], a.size[1])).save(centre_path)
        margins = (_margin(b, "left", 0, margin),
                   _margin(b, "right", b.size[0] - margin, margin))
        size_a, size_b = a.size, b.size
    centre = compare_pictures(centre_path, narrow, out_dir / f"{wide.stem}.centre.magnitude.png")
    return Extension(size_a, size_b, margin, centre, margins, centre_path)


def announce(extension: Extension) -> None:
    """Print the verdict with every number it rests on, both answers legible."""
    centre = extension.centre
    verdict = "EXTENDS" if extension.extends else "DOES NOT EXTEND"
    print(f"[widescreen] {extension.narrow_size[0]} -> {extension.wide_size[0]} "
          f"(+{extension.margin} per side): {verdict}")
    print(f"[widescreen]   centre: {centre.significant}/{centre.pixels} a different COLOUR "
          f"({100 * centre.significant_share:.2f}%, tolerance "
          f"{100 * extension.CENTRE_TOLERANCE:.0f}%) — "
          f"{'survived' if extension.centre_survived else 'CHANGED, so the picture was resampled'}")
    for panel in extension.margins:
        print(f"[widescreen]   {panel.side} margin: {100 * panel.non_black_share:.1f}% non-black, "
              f"{panel.distinct_colours} colours, {panel.repeated_columns}/{panel.width - 1} "
              f"repeated columns — "
              f"{'scene' if panel.carries_scene else 'NOT SCENE COVERAGE'}")
