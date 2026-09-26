#!/usr/bin/env python3
"""widescreen_pair.py — is the wide picture a PROJECTION WIDENING, or a STRETCH?

GAME-AGNOSTIC, AND THIS IS ITS ONE HOME, like its sibling present_geometry.py. It reuses that
file's image reader rather than carrying a second PNG/PPM decoder.

WHY IT EXISTS. `present_geometry.py` measures ONE picture's shape, which catches a frame presented
at the wrong aspect. It cannot catch the failure this file is for, because that failure produces a
picture of exactly the right shape: a 4:3 render scaled horizontally to fill a 16:9 target looks
correct to every single-image measurement. Aspect, coverage, colour count and per-tile richness are
all INVARIANT UNDER A STRETCH of the whole frame into the wider sink.

Separating the two needs a SECOND picture. Run the same title, same route, same checkpoint, once at
4:3 and once wide, then ask which relationship holds between them:

  TRANSLATION  the wide frame contains the 4:3 frame at its ORIGINAL SCALE, offset by
               (wide_w - narrow_w) // 2, with genuinely new content at the sides.
               This is a real horizontal FOV widening.

  STRETCH      the wide frame is the 4:3 frame resampled to the wider width. No new geometry
               was rendered; the same pixels were spread out.

Winning that comparison says the ORIGINAL picture survived intact and centred. It says nothing
about what is in the new area, and two fakes live exactly there, both of which score a perfect
translation because they leave the centre untouched:

  the margins hold NO SCENE  -- black, one flat colour, or the edge column smeared outward.
  the margins hold SOMEONE ELSE'S SCENE -- drawn by a projection of their own, so the world
               breaks where the margin meets the centre. Measured: Spyro 1's and Tomba! 2's real
               joins differ from the columns beside them by 1.01x, 0.71x, 0.56x and 0.82x; the same
               frames with their margins drawn six rows off read 7.84x, 7.27x, 2.37x and 2.30x.

So the verdict below is WIDENED only when the centre survived, the margins carry scene, and the
joins are continuous. None of the three says the extra geometry is CORRECT -- nothing here can,
short of a 16:9 reference that does not exist for a PSX title. They say it is present, continuous
with the picture it extends, and not the original image spread out.

These are quantitatively different and the difference is large, so the verdict does not rest on a
hairline margin. Measured on Spyro 1's save screen at 512 -> 684: translation scores 2.26 mean
absolute error at the predicted offset, 17.3 one pixel away, and a stretch scores 95.9.

THE NEGATIVE IS DESIGNED FIRST. Every report states the score of the losing hypothesis and of the
WORST offset tried, so "translation won" is always accompanied by what it beat. A verdict with no
spread is reported as INCONCLUSIVE rather than as a pass: two pictures that are nearly uniform, or
identical, make every hypothesis score the same and that must not read as success.

    uv run --frozen python external/psxport/tools/port/widescreen_pair.py --selftest
    uv run --frozen python external/psxport/tools/port/widescreen_pair.py \
        --narrow shot43.png --wide shot169.png

CHOOSE THE CHECKPOINT FOR CONTRAST. The separation this reports depends entirely on how much
horizontal detail the picture has, so pick a UI, menu or text-rich frame over open scenery. Measured
on Spyro 1 at 512 -> 684, the same build on the same run:

    save_picker  best 2.26 at dx=+86, next 17.34, stretch 96.45   -> 42.7x separation
    playing      best 2.45 at dx=+86, next  2.63, stretch  4.15   ->  1.7x separation

Both are correct verdicts, but only the first is a strong one. Smooth, dithered gameplay scenery
scores nearly the same under every hypothesis, so a pass there is weak evidence; a 1.7x margin is
above the threshold but is not the check working well. When a title has no UI checkpoint, say so
rather than quoting the margin as if it were the menu's.
"""

from __future__ import annotations

import argparse
import math
from dataclasses import dataclass
import os
import shutil
import sys

from present_geometry import Unreadable, read_image

# A verdict needs the winner to beat the runner-up by more than this ratio, otherwise the two
# hypotheses are not actually distinguished by this pair of pictures.
MIN_SEPARATION = 1.25

# Absolute floor on a score before ratios mean anything. Two pictures that match exactly score 0.00,
# and a ratio against 0 is not a measurement; this keeps the comparison additive at that end.
EPSILON = 0.5

# A margin carries scene when it is mostly drawn, varied, and not one column repeated across the
# panel. Measured on the real extensions: 93.3%/99.2%/100.0% non-black, 139 to 582 distinct colours,
# and 0 repeated columns out of 85 and 53.
MIN_NON_BLACK_SHARE = 0.5
MIN_MARGIN_COLOURS = 16

# How far either side of a join to sample, and how many times its neighbourhood the join may be.
# The comparison is LOCAL on purpose: a discontinuity is a local event, and a frame-wide percentile
# hides it behind whatever the scene's hardest vertical edge happens to be. Measured on Tomba! 2,
# whose widest ordinary column pair is 42.33 -- somewhere in that scene there is a hard edge --
# while its real joins read 7.97 and 9.20 and a deliberately broken margin reads 33.80. Against the
# frame the break hides; against its neighbours it is 2.37x theirs.
SEAM_WINDOW = 8
SEAM_RATIO = 2.0

# Sampling stride. The pictures are small and the signal is global; every second pixel is plenty and
# keeps a full offset sweep cheap.
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

    One-sided on purpose. A gap far BELOW its neighbourhood is not a projection defect, it is a
    duplicated column -- the smear fake, which Panel.repeated already owns.
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
    # px is flat RGB bytes, the format present_geometry.read_image returns, so a pixel here is a
    # 3-byte slice and BLACK is b"\x00\x00\x00". Comparing such a slice against a (0, 0, 0) tuple
    # is never equal, which would report every panel as fully non-black.
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
    """Both joins, each scored against the ordinary column pairs immediately around it.

    None when a join cannot say what this part of the picture's column-to-column variation normally
    is -- a refusal, not a pass. There are TWO ways to be unable to say it, and both were measured:

    - too few neighbours, which the original guard covered; and
    - a neighbourhood whose maximum variation is ZERO, i.e. a perfectly flat region. The ratio is
      gap/local_max, so a flat neighbourhood divides by zero, the ratio becomes inf, and `inf > limit`
      reads as a BREAK. Measured 2026-09-26 on Tekken 3's `NAMCO PRESENTS` card, where the text glyphs
      sit on a flat black field: both joins reported `gap 0.00 against a neighbourhood of 0.00 (infx)`
      and were called broken. Nothing broke; the question was unaskable. "Cannot tell" is the honest
      answer and it is the one the too-few-neighbours case already returns.

    KNOWN FALSE POSITIVE, stated rather than hidden: a scene whose own hard vertical edge falls
    exactly on a join reads as a break, because at that one column the check cannot tell the
    world's edge from the frustum's. It has no way to distinguish them from a single frame. When a
    join reports a break, re-measure on another state before believing it; a real reprojection
    breaks at every state, a coincident scene edge does not.
    """
    found = []
    for side, x in (("left", margin - 1), ("right", w - margin - 1)):
        window = range(max(0, x - SEAM_WINDOW), min(len(gaps), x + SEAM_WINDOW + 1))
        local = [gaps[i] for i in window if i != x]
        if len(local) < 2 * SEAM_WINDOW:
            return None
        local_max = max(local)
        if local_max <= 0.0:
            # A neighbourhood with no variation at all: the ratio would divide by zero and every
            # join would read as an infinite break. Refuse the same way the too-few-neighbours case
            # does, because both mean the same thing -- this picture cannot answer the question here.
            return None
        found.append(Seam(side, x, gaps[x], local_max))
    return found


def _mean_abs_error(pairs) -> float | None:
    """Mean per-pixel absolute RGB error over (narrow_index, wide_index) byte offsets.

    Pixels are flat RGB bytes, the format present_geometry.read_image returns, so this file indexes
    exactly what the shipping reader produces rather than a convenient shape of its own.
    """
    total = 0
    count = 0
    for (na, npx), (wa, wpx) in pairs:
        total += (abs(npx[na] - wpx[wa]) + abs(npx[na + 1] - wpx[wa + 1])
                  + abs(npx[na + 2] - wpx[wa + 2]))
        count += 1
    return total / count if count else None


def translation_scores(nw, h, npx, ww, wpx, span=8):
    """Mean absolute error for every horizontal offset of the narrow frame within the wide one.

    Returns [(score, dx), ...] sorted best first. The sweep deliberately runs past both ends of the
    plausible range so the report can quote the WORST offset: a best score means nothing without the
    range it sits in.
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

    Nearest-neighbour on purpose: the question is whether the SAME pixels were spread out, and a
    smoothing resample would flatter the stretch hypothesis by hiding its own resampling error.
    """
    def sampled():
        for y in range(0, h, STRIDE):
            for x in range(0, ww, STRIDE):
                yield ((y * nw + (x * nw) // ww) * 3, npx), ((y * ww + x) * 3, wpx)

    return _mean_abs_error(sampled())


def report(narrow_path, wide_path, out=print) -> int:
    # A capture that is not on disk is a REFUSAL by name, not a traceback. Measured 2026-09-19 on
    # Tomba! 2: the product hit a fatal invariant at frame 2700 and wrote no PNG, and this tool died
    # with `FileNotFoundError: scratch/wspair/f2700_a0.png` -- which reads as a broken tool when the
    # actual news is that the RUN failed. Naming the missing file and saying nothing was compared
    # puts the reader back on the run.
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

    # The verdict compares the two HYPOTHESES against each other. An earlier version instead
    # required the translation offsets to be spread out, which was wrong twice over: a perfect
    # translation scores 0.00 and read as "not separated" when it is the strongest possible pass,
    # and a genuine stretch makes every offset score badly and similarly, which is the expected
    # shape of that answer rather than an absence of one. EPSILON keeps a near-zero winner from
    # dividing the field by chance.
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
    # The centre survived. What is in the new area is a separate question with its own two ways
    # of being wrong, and both leave the translation score untouched.
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

    # best can be exactly 0 on a synthetic or perfectly reproduced pair, so the margin is reported
    # additively there instead of as a ratio against zero.
    margin = (f"{stretch / best:.1f}x worse" if best > EPSILON
              else f"worse by {stretch:.2f} against an exact match")
    out(f"  VERDICT: WIDENED — the 4:3 content appears at its original scale, centred, and the "
        f"stretch hypothesis is {margin}. Both margins carry scene and join the centre "
        f"continuously ({', '.join(f'{j.ratio:.2f}x' for j in joins)} their neighbourhoods), so "
        f"additional horizontal geometry was rendered rather than the original image being spread "
        f"out.")
    return 0


# Block boundaries in ABSOLUTE column space, covering the whole widened span rather than the narrow
# frame's 0..511. Deliberately none at 0 or 512: those are exactly where the margins meet the centre,
# and a fixture whose own hard edge sits on the join would be testing the join check against a case
# it is known not to be able to answer (see the false positive named in `seams`).
_EDGES = [-79, -55, -31, -12, 23, 41, 78, 96, 139, 187, 204, 251, 297, 314, 368, 402, 455, 498,
          521, 546, 570, 593]


def _pattern(w, h, x0=0):
    """A low-frequency, non-repeating picture over ABSOLUTE columns [x0, x0+w): irregular colour
    blocks over a gradient, with a slow ramp inside each block.

    Three earlier fixtures were wrong in instructive ways. Regular bars ALIAS -- with period p, every
    offset differing by p ties, so the true one cannot be identified. A quadratic-phase pattern fixes
    that but goes high-frequency at one end, where nearest-neighbour resampling scrambles it and even
    a true stretch stops matching itself. Blocks at irregular positions, each at least 8px wide, are
    unique under translation and survive resampling well enough that a real stretch scores near zero.

    The ramp is the third fix and it is what the join check needs: without it neighbouring columns
    inside a block are IDENTICAL, so a join would be compared against a neighbourhood of zero and
    every widening would read as a break.
    """
    out = bytearray()
    for y in range(h):
        for x in range(w):
            ax = x0 + x
            index = sum(1 for e in _EDGES if ax >= e)
            lit = index % 2 == 0
            base = 40 + (y * 160) // max(h, 1)
            out += bytes((base if lit else 230 - base, (index * 37) % 256,
                          60 + (ax + 128) // 6))
    return bytes(out)


def _translated(narrow_w, wide_w, h, shift):
    """The widening hypothesis, built directly: the SAME world sampled over a wider span of columns,
    so the narrow picture appears at its original scale, offset by `shift`, and the margins continue
    it rather than holding something else.

    They used to hold arbitrary noise -- "genuinely different content", which sounds right and is
    not: a widened frustum shows MORE OF THE SAME WORLD, and content unrelated to the centre is the
    reprojected-margin fake this file now catches. That fixture failed the moment the join check
    existed, which is the fixture being wrong rather than the check.
    """
    return _pattern(wide_w, h, -shift)


def _stretched(narrow, narrow_w, wide_w, h):
    """The stretch hypothesis, built directly: the same pixels resampled to the wider width."""
    out = bytearray()
    for y in range(h):
        for x in range(wide_w):
            i = (y * narrow_w + (x * narrow_w) // wide_w) * 3
            out += narrow[i:i + 3]
    return bytes(out)


def _smooth(x0, w, h):
    """A CONTINUOUS scene over absolute columns [x0, x0+w): no hard vertical edges anywhere.

    The block pattern above is deliberately full of them, because the translation sweep needs
    unambiguous landmarks. That makes it the wrong fixture for the join checks, where the question
    is whether the frame HAS an ordinary column-to-column variation for a break to stand out from:
    across a block, neighbouring columns are identical and a join would be compared against zero.
    Here every adjacent pair differs by a few units and the two frames sample one shared world, so
    a real widening joins seamlessly and anything else does not.
    """
    out = bytearray()
    for y in range(h):
        for x in range(w):
            ax = x0 + x
            out += bytes((128 + int(100 * math.sin(ax / 13.0)),
                          128 + int(100 * math.sin(ax / 29.0 + y / 41.0)),
                          40 + (y * 150) // max(h, 1)))
    return bytes(out)


def _margins_replaced(wide, wide_w, h, shift, pixel_of):
    """The same wide frame with both margins redrawn, which is how each coverage fake is built:
    the centre is left byte-identical, so the translation sweep still reads a perfect widening and
    only the margin and join checks can object."""
    out = bytearray(wide)
    for y in range(h):
        for x in list(range(shift)) + list(range(wide_w - shift, wide_w)):
            index = (y * wide_w + x) * 3
            out[index:index + 3] = pixel_of(wide, wide_w, h, shift, x, y)
    return bytes(out)


def _black(wide, wide_w, h, shift, x, y):
    """Pillarboxing: the viewport widened and nothing was drawn in the new area."""
    return b"\x00\x00\x00"


def _smeared(wide, wide_w, h, shift, x, y):
    """The edge column repeated outward -- content, varied vertically, and no new world at all."""
    source = shift if x < shift else wide_w - shift - 1
    index = (y * wide_w + source) * 3
    return wide[index:index + 3]


def _sparse(wide, wide_w, h, shift, x, y):
    """Mostly black with a thin scatter: varied, never one column repeated, and still not a drawn
    world. Isolates the non-black threshold, which the fully black margin never reaches because
    every one of its columns is identical."""
    return (bytes((x % 256, y % 256, (x + y) % 256))
            if (x * 7 + y * 13) % 11 == 0 else b"\x00\x00\x00")


def _two_tone(wide, wide_w, h, shift, x, y):
    """A two-colour dither filling the panel: fully drawn, no repeated column, and two colours of
    world. Isolates the colour-count threshold."""
    return bytes((200, 60, 20)) if (x + y) % 2 else bytes((20, 60, 200))


def _reprojected(wide, wide_w, h, shift, x, y):
    """Real, varied, non-black scene drawn six rows off: a projection of its own. Every other
    check passes this; only the join says so."""
    index = (min(y + 6, h - 1) * wide_w + x) * 3
    return wide[index:index + 3]


def _report_on(images, narrow_path, wide_path, lines):
    """Run the SHIPPING report over in-memory fixtures by swapping only the reader.

    Every case goes through `report` itself, so the selftest exercises the verdict logic rather
    than a convenient copy of it.
    """
    global read_image
    original = read_image
    read_image = lambda path: images[path]  # noqa: E731 — swap the reader, keep the report
    try:
        return report(narrow_path, wide_path, out=lines.append)
    finally:
        read_image = original


def selftest(out=print) -> int:
    """Feed the report a pair that MUST read as widened and one that MUST read as stretched.

    A check that only ever confirms the good case is worthless, so the stretched pair is the more
    important of the two: it is the failure this file exists to catch.
    """
    narrow_w, wide_w, h = 512, 684, 240
    expected_dx = (wide_w - narrow_w) // 2
    pixels = _pattern(narrow_w, h)
    narrow = (narrow_w, h, pixels)
    widened = (wide_w, h, _translated(narrow_w, wide_w, h, expected_dx))
    stretched = (wide_w, h, _stretched(pixels, narrow_w, wide_w, h))

    # The fixtures are written to REAL files. They used to exist only in a dict handed to a stubbed
    # reader, which meant the shipping path's "is this file even there?" refusal was never on the
    # selftest's route -- and when that refusal was added, the selftest went 0/2 while the tool was
    # working correctly. A selftest that a correct change breaks is testing its own scaffolding.
    directory = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                             "..", "..", "build", "widescreen-pair-selftest")
    directory = os.path.normpath(directory)
    shutil.rmtree(directory, ignore_errors=True)
    os.makedirs(directory)
    narrow_path = os.path.join(directory, "N.png")
    wide_path = os.path.join(directory, "W.png")
    for path in (narrow_path, wide_path):
        with open(path, "wb") as handle:
            handle.write(b"fixture")

    # The coverage fakes, on a continuous scene: each leaves the centre untouched, so each scores a
    # perfect translation and must be caught by the margin or the join alone.
    # Shorter than the translation fixtures on purpose: the margin and join checks are per-column
    # measurements averaged down the rows, so height buys nothing here and every extra row is a
    # full-resolution scan of a 684-wide frame in pure Python. At 240 rows this selftest took 18s.
    coverage_h = 64
    smooth_wide = _smooth(-expected_dx, wide_w, coverage_h)
    smooth = (narrow_w, coverage_h, _smooth(0, narrow_w, coverage_h))
    # Each fake, and the verdict it must draw. Checking the exact verdict rather than merely "it
    # failed" is what keeps the thresholds honest: a margin fake that slipped past the coverage
    # check would still fail at the join, and a test that only asked for failure would call that
    # a pass while the threshold it was meant to exercise did nothing.
    coverage = [
        ("continuous widening", None, None),
        ("black margins", _black, "NOT COVERAGE"),
        ("smeared margins", _smeared, "NOT COVERAGE"),
        ("sparse margins", _sparse, "NOT COVERAGE"),
        ("two-tone margins", _two_tone, "NOT COVERAGE"),
        ("reprojected margins", _reprojected, "DIFFERENT PROJECTION"),
    ]

    failures = []
    cases = (("widened", widened, 0), ("stretched", stretched, 1))
    try:
        for name, wide, want in cases:
            lines = []
            code = _report_on({narrow_path: narrow, wide_path: wide},
                              narrow_path, wide_path, lines)
            if code != want:
                failures.append(f"{name} pair returned {code}, expected {want}:\n    "
                                + "\n    ".join(lines))

        for name, pixel_of, fragment in coverage:
            pixels = (smooth_wide if pixel_of is None
                      else _margins_replaced(smooth_wide, wide_w, coverage_h, expected_dx,
                                             pixel_of))
            lines = []
            code = _report_on({narrow_path: smooth, wide_path: (wide_w, coverage_h, pixels)},
                              narrow_path, wide_path, lines)
            want = 0 if fragment is None else 1
            if code != want:
                failures.append(f"{name} returned {code}, expected {want}:\n    "
                                + "\n    ".join(lines))
            elif fragment and not any(fragment in line for line in lines):
                failures.append(f"{name} failed, but not for the reason it was built to fail for "
                                f"-- no verdict said {fragment!r}, so the check that was meant to "
                                f"catch it did not:\n    " + "\n    ".join(lines))

        # A join whose neighbourhood has NO variation at all. Measured 2026-09-26 on Tekken 3's
        # NAMCO PRESENTS card, where text glyphs sit on a flat black field: the ratio divided by a
        # local_max of 0, became inf, and both joins printed BREAKS. Nothing was broken; the ratio
        # was unaskable. The tool must REFUSE, exactly as it does for too few neighbours, because
        # "cannot tell" is the one honest answer and reporting a break there is how a real defect
        # gets buried under a fake one.
        flat = bytearray(wide_w * coverage_h * 3)  # a wholly uniform picture: every gap is 0.0
        lines = []
        code = _report_on({narrow_path: smooth, wide_path: (wide_w, coverage_h, bytes(flat))},
                          narrow_path, wide_path, lines)
        joined = "\n    ".join(lines)
        if code != 2:
            failures.append(f"a wholly flat capture returned {code}, expected a refusal (2); a "
                            f"zero-variation neighbourhood must not be scored:\n    {joined}")
        elif "BREAKS" in joined:
            failures.append(f"a flat capture REFUSED but still called a join broken, which is the "
                            f"false positive this case exists to catch:\n    {joined}")
        elif "NOTHING WAS COMPARED at the joins" not in joined:
            failures.append(f"a flat capture refused for the wrong reason:\n    {joined}")

        # The negative the other two cannot give: a capture that is not there at all must REFUSE by
        # name and must not be mistaken for either verdict. This is the case that fired for real on
        # Tomba! 2 f2700, where the product died before writing the shot.
        lines = []
        code = report(os.path.join(directory, "absent.png"), wide_path, out=lines.append)
        if code != 2:
            failures.append(f"a missing capture returned {code}, expected 2 (REFUSED):\n    "
                            + "\n    ".join(lines))
        elif not any("NOTHING WAS COMPARED" in line for line in lines):
            failures.append("a missing capture refused but never said NOTHING WAS COMPARED, so a "
                            "reader could take the refusal for a result:\n    "
                            + "\n    ".join(lines))
    finally:
        shutil.rmtree(directory, ignore_errors=True)

    # The verdict line follows the shape scripts/tool_selftests.py recognises, the same as
    # looks_right.py ("28/28 => PASS") and present_geometry.py ("16/16 checks passed"). A selftest
    # whose output that runner cannot classify is reported as HAVING NO SELFTEST, which is exactly
    # as bad as not writing one -- this file was rejected that way before the wording was fixed.
    total = 9
    for failure in failures:
        out(f"widescreen_pair selftest: FAIL — {failure}")
    if failures:
        out(f"widescreen_pair selftest: {total - len(failures)}/{total} => FAIL")
        return 1
    out(f"widescreen_pair selftest: {total}/{total} => PASS (a pair translated by {expected_dx:+d} "
        f"reads WIDENED; the same picture resampled to {wide_w} reads STRETCHED; a capture that is "
        f"not on disk REFUSES by name; and on a continuous scene, margins that are black, smeared "
        f"from the edge column, sparse, two-tone, or drawn six rows off each draw the verdict they "
        f"were built to draw — the last of them with a byte-identical centre and real varied scene "
        f"in both margins, caught by the join alone)")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--narrow", help="the 4:3 capture")
    parser.add_argument("--wide", help="the widescreen capture of the SAME checkpoint")
    parser.add_argument("--selftest", action="store_true",
                        help="prove the check reports BOTH verdicts, then exit")
    args = parser.parse_args()
    if args.selftest:
        return selftest()
    if not args.narrow or not args.wide:
        parser.error("--narrow and --wide are both required (or use --selftest)")
    try:
        return report(args.narrow, args.wide)
    except Unreadable as exc:
        print(f"REFUSED: {exc}. NOTHING WAS COMPARED.", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
