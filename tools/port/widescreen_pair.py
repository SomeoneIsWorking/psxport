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

# Sampling stride. The pictures are small and the signal is global; every second pixel is plenty and
# keeps a full offset sweep cheap.
STRIDE = 2


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
    # best can be exactly 0 on a synthetic or perfectly reproduced pair, so the margin is reported
    # additively there instead of as a ratio against zero.
    margin = (f"{stretch / best:.1f}x worse" if best > EPSILON
              else f"worse by {stretch:.2f} against an exact match")
    out(f"  VERDICT: WIDENED — the 4:3 content appears at its original scale, centred, and the "
        f"stretch hypothesis is {margin}. Additional horizontal geometry was rendered rather than "
        f"the original image being spread out.")
    return 0


def _pattern(w, h):
    """A low-frequency, non-repeating picture: irregular colour blocks over a gradient.

    Two earlier fixtures were wrong in instructive ways. Regular bars ALIAS -- with period p, every
    offset differing by p ties, so the true one cannot be identified. A quadratic-phase pattern fixes
    that but goes high-frequency at one end, where nearest-neighbour resampling scrambles it and even
    a true stretch stops matching itself. Blocks at irregular positions, each at least 8px wide, are
    unique under translation and survive resampling well enough that a real stretch scores near zero.
    """
    edges = [0, 23, 41, 78, 96, 139, 187, 204, 251, 297, 314, 368, 402, 455, 498]
    out = bytearray()
    for y in range(h):
        for x in range(w):
            index = sum(1 for e in edges if (x * 512) // max(w, 1) >= e)
            lit = index % 2 == 0
            base = 40 + (y * 160) // max(h, 1)
            out += bytes((base if lit else 230 - base, (index * 37) % 256, 200 if lit else 60))
    return bytes(out)


def _translated(narrow, narrow_w, wide_w, h, shift):
    """The widening hypothesis, built directly: the narrow picture at its ORIGINAL scale, offset,
    with genuinely different content in the newly revealed margins."""
    out = bytearray()
    for y in range(h):
        for x in range(wide_w):
            nx = x - shift
            if 0 <= nx < narrow_w:
                i = (y * narrow_w + nx) * 3
                out += narrow[i:i + 3]
            else:
                # New geometry at the sides, deliberately unlike anything in the narrow frame.
                out += bytes(((x * 5) % 256, 255 - (y * 3) % 256, (x + y) % 256))
    return bytes(out)


def _stretched(narrow, narrow_w, wide_w, h):
    """The stretch hypothesis, built directly: the same pixels resampled to the wider width."""
    out = bytearray()
    for y in range(h):
        for x in range(wide_w):
            i = (y * narrow_w + (x * narrow_w) // wide_w) * 3
            out += narrow[i:i + 3]
    return bytes(out)


def selftest(out=print) -> int:
    """Feed the report a pair that MUST read as widened and one that MUST read as stretched.

    A check that only ever confirms the good case is worthless, so the stretched pair is the more
    important of the two: it is the failure this file exists to catch.
    """
    narrow_w, wide_w, h = 512, 684, 240
    expected_dx = (wide_w - narrow_w) // 2
    pixels = _pattern(narrow_w, h)
    narrow = (narrow_w, h, pixels)
    widened = (wide_w, h, _translated(pixels, narrow_w, wide_w, h, expected_dx))
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

    failures = []
    cases = (("widened", widened, 0), ("stretched", stretched, 1))
    try:
        for name, wide, want in cases:
            lines = []
            images = {narrow_path: narrow, wide_path: wide}
            global read_image
            original = read_image
            read_image = lambda path: images[path]  # noqa: E731 — swap the reader, keep the report
            try:
                code = report(narrow_path, wide_path, out=lines.append)
            finally:
                read_image = original
            if code != want:
                failures.append(f"{name} pair returned {code}, expected {want}:\n    "
                                + "\n    ".join(lines))

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
    for failure in failures:
        out(f"widescreen_pair selftest: FAIL — {failure}")
    if failures:
        out(f"widescreen_pair selftest: {3 - len(failures)}/3 => FAIL")
        return 1
    out(f"widescreen_pair selftest: 3/3 => PASS (a pair translated by {expected_dx:+d} reads "
        f"WIDENED; the same picture resampled to {wide_w} reads STRETCHED; a capture that is not "
        f"on disk REFUSES by name)")
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
