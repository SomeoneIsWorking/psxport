#!/usr/bin/env python3
"""Is a widescreen capture a projection widening or a stretch of the 4:3 picture?

Compares a 4:3 and a wide capture of one checkpoint. WIDENED needs the narrow frame to appear at its
original scale centred in the wide one (translation by (wide_w - narrow_w) // 2 beats resampling),
both margins to carry scene (drawn, varied, not one column smeared) and both joins to be continuous
with their neighbourhood (a margin drawn by another projection breaks there). Every report quotes
the losing hypothesis and the worst offset; no separation is INCONCLUSIVE.

    uv run --frozen python external/psxport/tools/port/widescreen_pair.py --narrow 43.png --wide 169.png

Pick a UI or text-rich checkpoint: smooth scenery scores nearly the same under every hypothesis.
A scene's own hard edge exactly on a join reads as a break; re-measure on another state.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import os
import sys

from present_geometry import Unreadable, read_image

# Winner must beat the runner-up by this ratio.
MIN_SEPARATION = 1.25

# Additive floor so a ratio against an exact match (0.00) stays meaningful.
EPSILON = 0.5

# A margin carries scene when mostly drawn, varied, and not one column repeated.
MIN_NON_BLACK_SHARE = 0.5
MIN_MARGIN_COLOURS = 16

# A join is scored against its local neighbourhood; a frame-wide measure hides it behind scene edges.
SEAM_WINDOW = 8
SEAM_RATIO = 2.0

STRIDE = 2


@dataclass(frozen=True)
class Panel:
    """One side margin of the widened frame."""

    side: str
    width: int
    height: int
    non_black: int
    colours: int
    repeated: int  # columns identical to the panel's first; a smear repeats all of them

    @property
    def non_black_share(self) -> float:
        area = self.width * self.height
        return self.non_black / area if area else 0.0

    @property
    def carries_scene(self) -> bool:
        return (self.non_black_share > MIN_NON_BLACK_SHARE
                and self.colours > MIN_MARGIN_COLOURS
                and self.repeated < self.width - 1)


@dataclass(frozen=True)
class Seam:
    """The join between one margin and the centre, against its immediate neighbourhood.

    One-sided: a gap far below its neighbourhood is a duplicated column, which Panel.repeated owns.
    """

    side: str
    x: int  # left column of the straddling pair
    gap: float
    local_max: float

    @property
    def ratio(self) -> float:
        return self.gap / self.local_max if self.local_max else float("inf")

    @property
    def continuous(self) -> bool:
        return self.ratio <= SEAM_RATIO


def _panel(w, h, px, side, x0, width) -> Panel:
    columns = [b"".join(px[(y * w + x0 + dx) * 3:(y * w + x0 + dx) * 3 + 3] for y in range(h))
               for dx in range(width)]
    seen = {column[i:i + 3] for column in columns for i in range(0, len(column), 3)}
    return Panel(side, width, h,
                 sum(1 for column in columns for i in range(0, len(column), 3)
                     if column[i:i + 3] != b"\x00\x00\x00"),
                 len(seen),
                 sum(1 for column in columns[1:] if column == columns[0]))


def column_gaps(w, h, px) -> list[float]:
    """For every adjacent column pair, the mean over rows of the largest per-channel difference."""
    gaps = []
    for x in range(w - 1):
        total = 0
        for y in range(h):
            a = (y * w + x) * 3
            total += max(abs(px[a] - px[a + 3]), abs(px[a + 1] - px[a + 4]),
                         abs(px[a + 2] - px[a + 5]))
        gaps.append(total / h if h else 0.0)
    return gaps


def seams(gaps, w, margin) -> list[Seam] | None:
    """Both joins scored against their neighbourhood, or None when too few or flat neighbours."""
    found = []
    for side, x in (("left", margin - 1), ("right", w - margin - 1)):
        window = range(max(0, x - SEAM_WINDOW), min(len(gaps), x + SEAM_WINDOW + 1))
        local = [gaps[i] for i in window if i != x]
        if len(local) < 2 * SEAM_WINDOW:
            return None
        local_max = max(local)
        if local_max <= 0.0:
            return None
        found.append(Seam(side, x, gaps[x], local_max))
    return found


def _mean_abs_error(pairs) -> float | None:
    """Mean per-pixel absolute RGB error over (narrow_index, wide_index) byte offsets."""
    total = 0
    count = 0
    for (na, npx), (wa, wpx) in pairs:
        total += (abs(npx[na] - wpx[wa]) + abs(npx[na + 1] - wpx[wa + 1])
                  + abs(npx[na + 2] - wpx[wa + 2]))
        count += 1
    return total / count if count else None


def translation_scores(nw, h, npx, ww, wpx, span=8):
    """Mean absolute error for every horizontal offset of the narrow frame within the wide one.

    Returns [(score, dx), ...] best first; the sweep runs past both ends so the worst is quoted.
    """
    scores = []
    for dx in range(-span, (ww - nw) + span + 1):
        def sampled():
            for y in range(0, h, STRIDE):
                for x in range(0, nw, STRIDE):
                    wx = x + dx
                    if 0 <= wx < ww:
                        yield ((y * nw + x) * 3, npx), ((y * ww + wx) * 3, wpx)

        score = _mean_abs_error(sampled())
        if score is not None:
            scores.append((score, dx))
    scores.sort()
    return scores


def stretch_score(nw, h, npx, ww, wpx):
    """Mean absolute error if the wide frame were the narrow one resampled to the wider width.

    Nearest-neighbour: the question is whether the same pixels were spread out.
    """
    def sampled():
        for y in range(0, h, STRIDE):
            for x in range(0, ww, STRIDE):
                yield ((y * nw + (x * nw) // ww) * 3, npx), ((y * ww + x) * 3, wpx)

    return _mean_abs_error(sampled())


def report(narrow_path, wide_path, out=print) -> int:
    missing = [p for p in (narrow_path, wide_path) if not os.path.isfile(p)]
    if missing:
        out(f"REFUSED: no capture at {', '.join(missing)}. The run that was supposed to write it "
            f"either never reached the checkpoint or failed before the shot -- read ITS log, not "
            f"this one. NOTHING WAS COMPARED, and this is not a pass.")
        return 2
    nw, nh, npx = read_image(narrow_path)
    ww, wh, wpx = read_image(wide_path)
    if nh != wh:
        out(f"REFUSED: {narrow_path} is {nw}x{nh} and {wide_path} is {ww}x{wh}; the two captures "
            f"must share a height or they are not the same checkpoint. NOTHING WAS COMPARED.")
        return 2
    if ww <= nw:
        out(f"REFUSED: the 'wide' capture {wide_path} is {ww} wide and the 'narrow' one is {nw}; "
            f"the wide one must be wider. NOTHING WAS COMPARED.")
        return 2

    expected_dx = (ww - nw) // 2
    scores = translation_scores(nw, nh, npx, ww, wpx)
    best, best_dx = scores[0]
    runner_up = next((s for s, dx in scores if dx != best_dx), None)
    worst, worst_dx = scores[-1]
    stretch = stretch_score(nw, nh, npx, ww, wpx)

    out(f"{narrow_path} {nw}x{nh}  vs  {wide_path} {ww}x{wh}")
    out(f"  predicted offset for a pure widening : {expected_dx:+d}")
    out(f"  best translation                     : {best:8.2f} at dx={best_dx:+d}")
    out(f"  next best translation                : {runner_up:8.2f}"
        if runner_up is not None else "  next best translation                :   (only one offset)")
    out(f"  worst of {len(scores)} offsets tried          : {worst:8.2f} at dx={worst_dx:+d}")
    out(f"  stretch hypothesis                   : {stretch:8.2f}")

    panels = [_panel(ww, wh, wpx, "left", 0, expected_dx),
              _panel(ww, wh, wpx, "right", ww - expected_dx, expected_dx)]
    for panel in panels:
        out(f"  {panel.side:5s} margin {panel.width:3d}px wide          : "
            f"{100 * panel.non_black_share:5.1f}% non-black, {panel.colours} colours, "
            f"{panel.repeated}/{panel.width - 1} repeated columns"
            f"{'' if panel.carries_scene else '   <- NOT SCENE'}")
    joins = seams(column_gaps(ww, wh, wpx), ww, expected_dx)
    if joins is None:
        out(f"REFUSED: a {ww}-column capture with a {expected_dx}-column margin leaves too few "
            f"ordinary column pairs beside a join to say what this picture's column-to-column "
            f"variation normally is, so a discontinuity could not be told from the scene's own "
            f"texture. NOTHING WAS COMPARED at the joins.")
        return 2
    for join in joins:
        out(f"  {join.side:5s} join at x={join.x:<4d}             : gap {join.gap:6.2f} against a "
            f"neighbourhood of {join.local_max:6.2f} ({join.ratio:5.2f}x, limit {SEAM_RATIO:.1f}x)"
            f"{'' if join.continuous else '   <- BREAKS'}")

    if stretch + EPSILON < best / MIN_SEPARATION:
        out(f"  VERDICT: STRETCHED — the stretch hypothesis scores {stretch:.2f} against the best "
            f"translation's {best:.2f}. The wide picture is the 4:3 picture resampled, not "
            f"additional rendered geometry.")
        return 1
    if not best + EPSILON < stretch / MIN_SEPARATION:
        out("  VERDICT: INCONCLUSIVE — neither hypothesis beats the other, so these two pictures "
            "cannot distinguish a widening from a stretch. Check they are the same checkpoint, "
            "that neither is blank, and that the wide one really was captured wide.")
        return 3
    if best_dx != expected_dx:
        out(f"  VERDICT: WIDENED BUT OFF-CENTRE — translation wins, but at dx={best_dx:+d} rather "
            f"than the predicted {expected_dx:+d}. The extra area is not symmetric about the "
            f"original framing.")
        return 1
    unscened = [panel.side for panel in panels if not panel.carries_scene]
    if unscened:
        out(f"  VERDICT: WIDENED BUT THE {'/'.join(unscened).upper()} MARGIN IS NOT COVERAGE — the "
            f"original picture survived at its original scale, and the area the widening revealed "
            f"is blank, flat, or one column smeared outward. The viewport widened; the world did "
            f"not.")
        return 1
    broken = [join.side for join in joins if not join.continuous]
    if broken:
        out(f"  VERDICT: WIDENED BUT THE {'/'.join(broken).upper()} MARGIN IS A DIFFERENT "
            f"PROJECTION — the original picture survived and the margins hold scene, but the world "
            f"breaks where margin meets centre ("
            + ", ".join(f"{j.side} {j.ratio:.2f}x its neighbourhood" for j in joins
                        if not j.continuous)
            + "), so the extra area was not drawn by the same widened frustum.")
        return 1

    margin = (f"{stretch / best:.1f}x worse" if best > EPSILON
              else f"worse by {stretch:.2f} against an exact match")
    out(f"  VERDICT: WIDENED — the 4:3 content appears at its original scale, centred, and the "
        f"stretch hypothesis is {margin}. Both margins carry scene and join the centre "
        f"continuously ({', '.join(f'{j.ratio:.2f}x' for j in joins)} their neighbourhoods), so "
        f"additional horizontal geometry was rendered rather than the original image being spread "
        f"out.")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--narrow", help="the 4:3 capture")
    parser.add_argument("--wide", help="the widescreen capture of the SAME checkpoint")
    args = parser.parse_args()
    if not args.narrow or not args.wide:
        parser.error("--narrow and --wide are both required")
    try:
        return report(args.narrow, args.wide)
    except Unreadable as exc:
        print(f"REFUSED: {exc}. NOTHING WAS COMPARED.", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
