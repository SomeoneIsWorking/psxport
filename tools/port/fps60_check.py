#!/usr/bin/env python3
"""fps60_check.py — find things that DON'T interpolate, straight off an fps60dump capture.

FRAMEWORK-OWNED and title-neutral: it reads two outputs this framework writes — the
frame_presenter.cpp dump (PSXPORT_DEBUG=fps60dump) and the Fps60 run log (PSXPORT_DEBUG=fps60seq) —
and knows nothing about any particular game. Every port that turns fps60 on can point it at its own
capture. The measurements quoted below are Tomba! 2's because that is where it was first used.

WHY: the fps60 directive is "there should technically be no difference between interpolated and
real frames" (docs/fps60-rework.md). Eyeballing a 60fps stream cannot tell you WHICH object is
stale — a rope that never lerps and a rope that lerps correctly look identical in a still, and in
motion the eye only reports "something judders". This walks the real/interp/real triples
`PSXPORT_DEBUG=fps60dump` writes to scratch/framedump/ and answers it per-REGION:

    real(N-1)  ---- interp ---- real(N)
                     ^ where is it actually?

For every 16x16 tile it classifies the interp pixel block as:
  STALE   — identical to real(N-1) while real(N) differs there  (drawn at the previous endpoint)
  AHEAD   — identical to real(N) while real(N-1) differs        (drawn at the next endpoint)
  BETWEEN — differs from both, which is what a lerped prim looks like
  STATIC  — all three agree (nothing moving here; not evidence either way)

STALE ALONE IS NOT A BUG REPORT, and reading it as one cost a session. When the true motion between
the two real frames is under a pixel, the rasterizer at t=0.5 has to land on one side or the other,
and a tile that lands on an endpoint is correct output, not a prim that failed to lerp. So this also
measures, per tile, the best integer translation that aligns real(N-1) onto real(N):

  STALE/AHEAD with a shift of >= 1px   an object that MOVED and was drawn at an endpoint anyway —
                                       the real defect, and the only one worth chasing
  STALE/AHEAD with a shift of 0px      sub-pixel change (dither, shading, a texel-sampling edge)
                                       quantised onto an endpoint — expected

Measured 2026-09-19 on Tomba! 2's hut interior, 120 triples: all 105 STALE and all 110 AHEAD tiles
had a 0px shift, so that scene has no interpolation failure left at this granularity. The opening
cutscene, 299 triples, is the other answer: 1,547 endpoint tiles moved a whole pixel or more.

WHOSE tiles those are is the next question, and --seq is the attempt to answer it: given an fps60seq
log from the SAME run, every moving tile is credited to the smallest run covering it (smallest,
because the full-screen sky fill covers everything drawn in front of it).

IT IS CURRENTLY REFUSED, and the owner tables that were once printed here have been retracted — see
psxport issue 0120. The dump described the CAPTURED queue while the picture is drawn from the merged
stream that replaces every reconstructed item, so the runs were not on the pixels; and the 16x16 tile
statistic saturates (72% of pixels identical between consecutive real frames, but 88.7% of tiles hold
at least one change), so nothing could score above chance anyway. The dump now describes the emitted
stream, which is what made the true mapping beat its wrong-offset controls for the first time (1.32x
vs 1.20x/0.93x), but 1.32x still is not enough to name an owner on a panning scene. mapping_verdict
keeps the refusal, control and all, so the retracted tables cannot come back quietly.

WHAT THAT CONTENT ACTUALLY DOES was got wrong here first, and the correction is the reason this tool
splits the two endpoints. It does NOT replay from the previous queue: FramePresenter::capturedFrame()
returns the CURRENT fence's items and both presents run over it, so an unreconstructed item is drawn
in the in-between present at the position the NEXT real frame will show it — a whole frame early, not
a frame late. It reads AHEAD, not STALE.

THE DISCRIMINATOR THAT SURVIVES ALL OF THAT is --forced, and it is the mode to reach for when a
defect needs a cause. Run the same deterministic route twice, once at the product's own factor and
once with PSXPORT_FPS60_TFORCE=0, and ask every pixel whose two real endpoints differ WHICH endpoint
it landed on. It needs no attribution, no run extents and no mapping between renderer space and
pixels. Measured 2026-09-22:

  Spyro 1, gameplay at 16:9, 81 triples   99.94% responded to t · 0.03% did not · all 81 triples <=1%
  Tomba! 2, gameplay, 299 triples         96.35% responded · 2.19% did not · no triple >=99%
  Tomba! 2, opening cutscene, 247 triples 64.66% responded · 30.21% did not · 66 CONTINUOUS triples
                                          wholly unresponsive (Tomba2Engine issue 0021)

Report the whole capture as one number and those three read as one blurred answer, which is why this
mode splits per triple and separates a discontinuity from continuous content: refusing to interpolate
across a cut is correct, and a capture spanning menus, a cutscene and gameplay is full of them.

USAGE (from a consuming game, where external/psxport is the framework)
  PSXPORT_DEBUG=fps60dump ... <the game binary> ...   # capture (cap 600 files)
  PSXPORT_DEBUG=fps60seq  ... <the game binary> ...   # the owner log, SAME replay and window
  external/psxport/tools/port/fps60_check.py                            # walk scratch/framedump/
  external/psxport/tools/port/fps60_check.py --dir scratch/framedump --tile 16 --top 12
  external/psxport/tools/port/fps60_check.py --dir scratch/framedump --seq scratch/seq.log
  external/psxport/tools/port/fps60_check.py --triple f001234    # one triple, with a tile map
  external/psxport/tools/port/fps60_check.py --dir <product> --forced <t=0 capture>
  external/psxport/tools/port/fps60_check.py --selftest          # the attribution's own fixtures

Filenames come from Fps60::dumpPresent: scratch/framedump/f<fence>_<seq>_<real|interp>.png.
Needs Pillow (already used elsewhere in tools/).
"""
import argparse, os, re, sys
from collections import defaultdict

try:
    from PIL import Image, ImageChops, ImageStat
except ImportError:
    sys.exit("fps60_check: needs Pillow (pip install pillow)")

NAME_RE = re.compile(r'^f(\d+)_(\d+)_(real|interp)\.png$')


def load_frames(d):
    """Ordered [(seq, kind, path, fence)] — seq is the dump counter, the true present order."""
    out = []
    for fn in os.listdir(d):
        m = NAME_RE.match(fn)
        if m:
            out.append((int(m.group(2)), m.group(3), os.path.join(d, fn), int(m.group(1))))
    out.sort()
    return out


def triples(frames):
    """Every real -> interp -> real run, in present order."""
    for i in range(len(frames) - 2):
        a, b, c = frames[i], frames[i + 1], frames[i + 2]
        if a[1] == "real" and b[1] == "interp" and c[1] == "real":
            yield a, b, c


def best_shift(pa, pc, tx, ty, tile, radius=2):
    """The integer (dx,dy) that best aligns pa's tile onto pc's, and how far that is.

    A tile that is STALE because its object genuinely moved shows a non-zero shift; a tile that is
    STALE because a sub-pixel change quantised onto an endpoint shows zero. Offsets that would leave
    the image are skipped rather than clamped, so an edge tile cannot be scored against a repeated
    border column."""
    reference = pc.crop((tx, ty, tx + tile, ty + tile))
    best, best_distance = 0, None
    for dy in range(-radius, radius + 1):
        for dx in range(-radius, radius + 1):
            box = (tx + dx, ty + dy, tx + dx + tile, ty + dy + tile)
            if box[0] < 0 or box[1] < 0 or box[2] > pa.width or box[3] > pa.height:
                continue
            distance = sum(ImageStat.Stat(ImageChops.difference(pa.crop(box), reference)).mean)
            if best_distance is None or distance < best_distance:
                best_distance, best = distance, max(abs(dx), abs(dy))
    return best


SHOT_RE = re.compile(r'wrote (\S+) \((\d+)x(\d+) @ (-?\d+),(-?\d+)\)')


def load_shot_origins(path):
    """{dump basename: (sx, sy)} — where in VRAM each captured frame was read from.

    The dump is the DISPLAY region, but a run's extent is in the draw space the queue built, which
    for a double-buffered title is the OTHER buffer half the time. Tomba! 2 always displays at 0,0
    and so needs no correction; Spyro 1 alternates 0,0 and 0,240 every present, which silently put
    half its frames 240 rows away from their own geometry."""
    origins = {}
    for line in open(path, errors="replace"):
        m = SHOT_RE.search(line)
        if m:
            origins[os.path.basename(m.group(1))] = (int(m.group(4)), int(m.group(5)))
    return origins


SEQ_FENCE_RE = re.compile(r'\[fps60seq\] f(\d+) t=')
SEQ_RUN_RE = re.compile(
    r'rqcur layer=(\d+) (TIER1|verbatim)\s+n=(\d+) seq=\[\S+\] producer=([0-9A-F]+) '
    r'node0=([0-9A-F]+) x=\[(-?\d+)\.\.(-?\d+)\) y=\[(-?\d+)\.\.(-?\d+)\)')


def load_sequence_runs(path):
    """{fence: [run]} from a PSXPORT_DEBUG=fps60seq log.

    A run is (area, owned, producer, node, layer, x0, x1, y0, y1). Area is precomputed because every
    lookup wants the SMALLEST covering run: crediting a tile to whatever run happens to come first
    credits the full-screen sky fill for everything drawn in front of it.

    The PRODUCER is carried as well as the entity node because they answer different questions. The
    node names which instance was drawn; the producer names the code that drew it, which is the thing
    somebody has to write a temporal source for. Reporting only the node leaves a defect with no
    actionable owner."""
    runs, fence = defaultdict(list), None
    for line in open(path, errors="replace"):
        m = SEQ_FENCE_RE.search(line)
        if m:
            fence = int(m.group(1))
            continue
        m = SEQ_RUN_RE.search(line)
        if m and fence is not None:
            x0, x1, y0, y1 = (int(m.group(i)) for i in (6, 7, 8, 9))
            entry = (max(0, (x1 - x0)) * max(0, (y1 - y0)), m.group(2) == "TIER1",
                     m.group(4), m.group(5), int(m.group(1)), x0, x1, y0, y1)
            if entry not in runs[fence]:
                runs[fence].append(entry)
    return runs


def covered_by_specific_run(runs, tx, ty, tile, frame_area):
    """Is this tile covered by a run that is NOT a screen-sized fill?

    Comparing the widest run to the frame width does not work: the widest run is a clip guard, three
    screens across on both Tomba! 2 and Spyro 1, not a fill the size of the screen. What DOES
    distinguish a right mapping from a wrong one is whether the SPECIFIC runs land on the pixels
    that moved. A full-screen fill covers every tile under any mapping, so it can never disagree;
    a run around one actor either sits on that actor or it does not."""
    for area, _owned, _producer, _node, _layer, x0, x1, y0, y1 in runs:
        if area * 2 >= frame_area:
            continue
        if tx < x1 and x0 < tx + tile and ty < y1 and y0 < ty + tile:
            return True
    return False


def mapping_lift(counts):
    """P(tile moved | inside a specific run) / P(tile moved | inside none), or None.

    An absolute rate cannot judge a mapping. A title whose moving content is a scrolling
    full-screen backdrop legitimately has most of its motion outside every specific run, and Tomba!
    2 scores 43.9% for that reason while being correctly mapped. What a WRONG mapping cannot
    produce is a difference: if a run around one actor is not where that actor's pixels are, then
    being inside it says nothing about whether those pixels moved, and the ratio collapses to 1.
    """
    inside_moved, inside_total, outside_moved, outside_total = counts
    if not inside_total or not outside_total:
        return None
    inside = inside_moved / inside_total
    outside = outside_moved / outside_total
    if outside <= 0.0:
        return None
    return inside / outside


def mapping_verdict(scores, minimum_lift=2.0):
    """(agree, detail) from {offset name: counts} — is the chosen mapping telling us anything?

    This is the check that was missing when attribution reported 100% coverage on a mapping that was
    wrong: coverage counted the screen-sized fill, which covers everything either way."""
    if not scores or "display" not in scores:
        return False, "no mapping was scored"
    lifts = {name: mapping_lift(counts) for name, counts in scores.items()}
    chosen = lifts.get("display")
    detail = ", ".join(f"{name} {'n/a' if lift is None else format(lift, '.2f')}x"
                       for name, lift in sorted(lifts.items()))
    if chosen is None:
        return False, f"the chosen mapping could not be scored ({detail})"
    control = lifts.get("shifted")
    # The control is the bar, not the constant: a weak measure that raises no mapping above 1.2 is
    # not evidence the mapping is wrong, and a measure that puts the deliberately-wrong offset level
    # with the real one is not evidence it is right.
    if control is not None and chosen < control * 1.25:
        return False, (f"the deliberately-wrong control mapping scores as well as the real one, so "
                       f"the runs are not landing on the pixels ({detail})")
    if control is None and chosen < minimum_lift:
        return False, (f"a tile inside a specific run is only {chosen:.2f}x as likely to have moved "
                       f"as one inside none, and there is no control to compare it to ({detail})")
    candidates = {name: lift for name, lift in lifts.items()
                  if name != "shifted" and lift is not None}
    best = max(candidates.values()) if candidates else chosen
    if chosen < best * 0.9:
        return False, f"another offset explains the moving pixels better ({detail})"
    return True, (f"a tile inside a specific run is {chosen:.2f}x as likely to have moved as one "
                  f"inside none, against {control:.2f}x for a deliberately-wrong offset ({detail})"
                  if control is not None else
                  f"a tile inside a specific run is {chosen:.2f}x as likely to have moved ({detail})")


def runs_bbox(runs_by_fence):
    """The union of every run's extent, in whatever space the log wrote them."""
    x0 = y0 = x1 = y1 = None
    for runs in runs_by_fence.values():
        for _area, _owned, _producer, _node, _layer, rx0, rx1, ry0, ry1 in runs:
            x0 = rx0 if x0 is None else min(x0, rx0)
            y0 = ry0 if y0 is None else min(y0, ry0)
            x1 = rx1 if x1 is None else max(x1, rx1)
            y1 = ry1 if y1 is None else max(y1, ry1)
    return (x0, y0, x1, y1)


def any_verbatim_over(runs, tx, ty, tile):
    """Does a NON-reconstructed run also cover this tile?

    Attribution credits a tile to the smallest run covering it, which can be a TIER1 run whose
    screen area happens to contain verbatim pixels drawn by a different item. Those pixels cannot
    lerp, so they snap forward, and the tile is then filed under a producer that is doing its job.
    This is what separates the two readings.
    """
    for area, owned, producer, node, layer, x0, x1, y0, y1 in runs:
        if owned:
            continue
        if tx < x1 and x0 < tx + tile and ty < y1 and y0 < ty + tile:
            return True
    return False


def owner_of(runs, tx, ty, tile):
    """The smallest run covering this tile, or None when no run does."""
    best = None
    for run in runs:
        area, _owned, _producer, _node, _layer, x0, x1, y0, y1 = run
        if tx < x1 and x0 < tx + tile and ty < y1 and y0 < ty + tile:
            if best is None or area < best[0]:
                best = run
    return best



# ---------------------------------------------------------------------------------------------
# Did the in-between present RESPOND to the interpolation factor?
# ---------------------------------------------------------------------------------------------
#
# The tile classification above asks where a region SITS. This asks something the single capture
# cannot: whether it MOVES when the factor does. Run the same deterministic route twice, once at
# the product's own factor and once with PSXPORT_FPS60_TFORCE=0, and every pixel whose two real
# endpoints differ has to answer for itself.
#
# A pixel that is interpolated sits at the PREVIOUS endpoint when the factor is forced to 0. One
# that is not interpolated is drawn from the current update's queue in both presents, so it sits
# at the NEXT endpoint whatever the factor is: a whole frame early, every frame, unmoved by t.
#
# This needs no attribution, no run extents and no mapping between renderer space and pixels, which
# is why it survives psxport issue 0120. It replaces an earlier two-way version that asked only
# whether the two runs DIFFER at a pixel: measured on Spyro 1 that read 33% invariant and spread
# evenly over the screen, because a pixel can land on the same colour at both factors. Asking WHICH
# endpoint it landed on instead read 0.03%, concentrated in one region.

FORCED_BANDS = 12
# Above this mean per-pixel channel difference the two real endpoints are not the same shot. A cut,
# a scene load and a screen-filling fade step all land here, and declining to interpolate across one
# is CORRECT: interpolation blends matching source geometry with explicit provenance, and across a
# discontinuity there is no match to blend. Only a continuous triple that still failed to respond is
# a defect, so the two populations are never reported as one number.
CUT_MAD = 24.0


def _changed_pixel_verdicts(pa, pc, p0, w, h, at_next_counts):
    """(previous, next, neither) over pixels whose two real endpoints differ."""
    prev = nxt = neither = 0
    for y in range(h):
        row = y * w
        for x in range(w):
            va, vc = pa[x, y], pc[x, y]
            if va == vc:
                continue
            v0 = p0[x, y]
            if v0 == va:
                prev += 1
            elif v0 == vc:
                nxt += 1
                at_next_counts[row + x] += 1
            else:
                neither += 1
    return prev, nxt, neither



def _mean_abs_diff(a, b):
    """Mean per-pixel channel difference between two frames — how far apart the endpoints are."""
    return ImageStat.Stat(ImageChops.difference(a, b)).mean[0]


def _fence_runs(fences, cap=8):
    """"12..40, 77, 903..910" — contiguous fence runs, so a stretch reads as a stretch."""
    runs = []
    for f in fences:
        if runs and f == runs[-1][1] + 1:
            runs[-1][1] = f
        else:
            runs.append([f, f])
    shown = ", ".join(f"{a}" if a == b else f"{a}..{b}" for a, b in runs[:cap])
    if len(runs) > cap:
        shown += f", and {len(runs) - cap} more run(s)"
    return f"{len(fences)} fence(s) in {len(runs)} run(s): {shown}"


def forced_factor_report(product_dir, forced_dir, out=print):
    """Compare a product-factor dump against a forced-factor dump of the SAME route."""
    for d in (product_dir, forced_dir):
        if not os.path.isdir(d):
            out(f"REFUSED: no capture dir {d}. NOTHING WAS COMPARED, and this is not a pass.")
            return 2
    product = load_frames(product_dir)
    forced = {(f[3], f[0], f[1]): f[2] for f in load_frames(forced_dir)}

    # THE CONTROL, and it is not optional: the factor must reach only the in-between present. If a
    # single real frame differs between the runs, the route drifted and every number below is
    # comparing two different journeys.
    checked = drifted = 0
    for fence, seq, kind, path in [(f[3], f[0], f[1], f[2]) for f in product if f[1] == "real"]:
        other = forced.get((fence, seq, kind))
        if other is None:
            continue
        checked += 1
        with open(path, "rb") as a, open(other, "rb") as b:
            if a.read() != b.read():
                drifted += 1
    if checked == 0:
        out(f"REFUSED: {product_dir} and {forced_dir} share no real frame by fence and sequence, "
            f"so they are not two runs of one route. NOTHING WAS COMPARED.")
        return 2
    if drifted:
        out(f"REFUSED: {drifted} of {checked} real frames differ between the two runs, so the "
            f"route drifted and the interpolation factor is not the only thing that changed. "
            f"NOTHING WAS COMPARED.")
        return 2

    w = h = None
    at_next = None
    totals = [0, 0, 0]
    used = 0
    per_triple = []  # (share of changed pixels that did NOT respond, fence)
    for a, b, c in triples(product):
        other = forced.get((b[3], b[0], b[1]))
        if other is None:
            continue
        ia, ic, i0 = (Image.open(a[2]).convert("RGB"), Image.open(c[2]).convert("RGB"),
                      Image.open(other).convert("RGB"))
        if w is None:
            w, h = ia.size
            at_next = [0] * (w * h)
        got = _changed_pixel_verdicts(ia.load(), ic.load(), i0.load(), w, h, at_next)
        for i in range(3):
            totals[i] += got[i]
        used += 1
        changed = sum(got)
        if changed:
            per_triple.append((got[1] / changed, b[3], _mean_abs_diff(ia, ic), changed))

    if used == 0:
        out(f"REFUSED: no real/interp/real triple of {product_dir} has a matching in-between "
            f"frame in {forced_dir}. NOTHING WAS COMPARED.")
        return 2
    total = sum(totals)
    if total == 0:
        out(f"REFUSED: across {used} triples no pixel changed between the two real endpoints, so "
            f"nothing in this capture could have been interpolated either way. This is a still "
            f"scene, not a passing one. NOTHING WAS MEASURED.")
        return 2

    prev, nxt, neither = totals
    out(f"{used} triples, {w}x{h}, real frames identical across both runs: {checked}/{checked}")
    out(f"  responded to t (at the PREVIOUS endpoint when forced) : {prev:9d} "
        f"({100.0 * prev / total:5.2f}%)")
    out(f"  did NOT respond (at the NEXT endpoint whatever t is)   : {nxt:9d} "
        f"({100.0 * nxt / total:5.2f}%)")
    out(f"  neither endpoint (partial coverage, blending)          : {neither:9d} "
        f"({100.0 * neither / total:5.2f}%)")

    # WHEN, not only where. A capture that spans menus, a cutscene and gameplay can average 30%
    # unresponsive out of a handful of wholly unresponsive triples and a majority of clean ones, and
    # the spatial bands cannot tell those two apart: content that is never interpolated for one
    # PHASE of a run spreads over the whole screen exactly like a picture that never interpolates.
    if per_triple:
        per_triple.sort()
        clean = sum(1 for t in per_triple if t[0] <= 0.01)
        broken = [t for t in per_triple if t[0] >= 0.99]
        out(f"  per triple: {clean} of {len(per_triple)} are <=1% unresponsive, {len(broken)} are "
            f">=99%, median {per_triple[len(per_triple) // 2][0] * 100:.2f}%")
        worst = per_triple[-1]
        out(f"    worst triple: fence {worst[1]} at {worst[0] * 100:.2f}% unresponsive of "
            f"{worst[3]} changed pixel(s), endpoints {worst[2]:.1f} apart")
        if broken:
            cuts = [t for t in broken if t[2] > CUT_MAD]
            same = [t for t in broken if t[2] <= CUT_MAD]
            out(f"    of the {len(broken)} wholly unresponsive triples, {len(cuts)} are across a "
                f"DISCONTINUITY (endpoints more than {CUT_MAD:g} apart: a cut, a load or a "
                f"screen-filling fade step), where refusing to interpolate is correct")
            if same:
                # WITH ITS DENOMINATOR. A 100% share over 50 pixels and over 50,000 are different
                # findings, and a share alone cannot tell them apart: a nearly black fade-in frame
                # has almost nothing changing, so every pixel that does change carries the whole
                # percentage. Report the changed-pixel count alongside it or the verdict overstates.
                px = sorted(t[3] for t in same)
                out(f"    and {len(same)} are CONTINUOUS and still did not respond — the defect, at "
                    f"fence(s) " + ", ".join(str(t[1]) for t in sorted(same, key=lambda t: t[1])[:8])
                    + (f" and {len(same) - 8} more" if len(same) > 8 else ""))
                out(f"      those cover {sum(px)} changed pixel(s): median {px[len(px) // 2]} per "
                    f"triple, smallest {px[0]}, largest {px[-1]}, against {w * h} in a frame")
            else:
                out(f"    and none is continuous, so nothing here is a failure to interpolate")
        if broken and clean:
            # WHICH fences, because that is the difference between a defect and a phase. A run that
            # covers a title card, a cutscene and gameplay contains stretches that are not supposed
            # to interpolate at all, and they are indistinguishable from broken gameplay in every
            # number above. Contiguous fence runs name the stretch to go and look at.
            out(f"    Two populations, so this capture is not one behaviour. The >=99% fences:")
            out("      " + _fence_runs(sorted(t[1] for t in broken)))
            out("    and the <=1% fences:")
            out("      " + _fence_runs(sorted(t[1] for t in per_triple if t[0] <= 0.01)))
            out(f"    Re-measure each stretch on its own before reading the totals above as one "
                f"number; a stretch that is not meant to interpolate is not a defect.")

    if nxt:
        rows = [0] * FORCED_BANDS
        cols = [0] * FORCED_BANDS
        xs = []
        ys = []
        for y in range(h):
            for x in range(w):
                v = at_next[y * w + x]
                if v:
                    rows[min(FORCED_BANDS - 1, y * FORCED_BANDS // h)] += v
                    cols[min(FORCED_BANDS - 1, x * FORCED_BANDS // w)] += v
                    xs.append(x)
                    ys.append(y)
        out(f"  the unresponsive pixels are {len(xs)} distinct positions in "
            f"x {min(xs)}..{max(xs)} y {min(ys)}..{max(ys)}")
        out("    by row band (top to bottom): "
            + " ".join(f"{100.0 * v / nxt:4.1f}%" for v in rows))
        out("    by col band (left to right): "
            + " ".join(f"{100.0 * v / nxt:4.1f}%" for v in cols))
        out(f"  A population spread evenly over both bands is the whole picture failing to "
            f"interpolate; one concentrated in a band or two is specific content, and the bbox "
            f"says where to look.")
    else:
        out("  Every changed pixel responded to the interpolation factor. Nothing in this capture "
            "is drawn at the next endpoint regardless of t.")
    return 0



def _forced_factor_selftest(check):
    """Drive forced_factor_report over dumps whose right answer is known by construction.

    Every branch here can print a number, so every branch gets a case: the responding scene, the
    snapping scene, the still scene, the drifted route, and the two empty-input refusals. The one
    that matters is the SNAP — a measure that says "interpolated" on content drawn at the next
    endpoint regardless of t would be exactly as reassuring as a correct one."""
    import shutil, tempfile

    def frame(path, boxes):
        im = Image.new("RGB", (48, 24), (0, 0, 0))
        px = im.load()
        for (x0, colour) in boxes:
            for y in range(2, 6):
                for x in range(x0, x0 + 3):
                    px[x, y] = colour
        im.save(path)

    def dump(d, interp_at):
        """real(A) has the box at x=2, real(C) at x=8; the in-between sits where told."""
        os.makedirs(d, exist_ok=True)
        frame(os.path.join(d, "f100_0_real.png"), [(2, (200, 30, 30))])
        frame(os.path.join(d, "f100_1_interp.png"), [(interp_at, (200, 30, 30))])
        frame(os.path.join(d, "f101_2_real.png"), [(8, (200, 30, 30))])
        return d

    root = tempfile.mkdtemp(prefix="fps60_forced_")
    try:
        product = dump(os.path.join(root, "product"), 5)   # halfway: the product's own factor
        responds = dump(os.path.join(root, "responds"), 2)  # forced to 0 -> at the PREVIOUS endpoint
        snaps = dump(os.path.join(root, "snaps"), 8)        # unmoved by t -> at the NEXT endpoint

        def run(a, b):
            lines = []
            code = forced_factor_report(a, b, out=lines.append)
            return code, "\n".join(lines)

        code, text = run(product, responds)
        check("responding scene passes", code, 0)
        check("responding scene is all previous", "100.00%)" in text
              and "PREVIOUS endpoint when forced) :        24 (100.00%" in text, True)
        check("responding scene says so", "Every changed pixel responded" in text, True)
        check("a clean triple is counted clean",
              "per triple: 1 of 1 are <=1% unresponsive, 0 are >=99%" in text, True)

        code, text = run(product, snaps)
        check("one triple is counted once", "per triple: 0 of 1 are <=1% unresponsive, 1 are >=99%"
              in text, True)
        # The snapping fixture moves a small box, so its endpoints are close: a continuous triple
        # that did not respond, which is the defect and must not be excused as a cut.
        check("a snap on continuous content is a defect",
              "1 are CONTINUOUS and still did not respond — the defect, at fence(s) 100" in text,
              True)
        # The share needs its denominator beside it or a nearly-black frame reads as a whole-screen
        # defect; the box is 3x4 at two positions, so 24 pixels changed out of 48x24.
        check("the defect carries its denominator",
              "those cover 24 changed pixel(s): median 24 per triple, smallest 24, largest 24, "
              "against 1152 in a frame" in text, True)
        check("the worst triple carries it too",
              "at 100.00% unresponsive of 24 changed pixel(s)" in text, True)
        check("and is not filed as a cut", "0 are across a DISCONTINUITY" in text, True)
        check("fence runs collapse", _fence_runs([1, 2, 3, 9, 20, 21]),
              "6 fence(s) in 3 run(s): 1..3, 9, 20..21")
        check("snapping scene still returns 0", code, 0)
        check("snapping scene is all next",
              "at the NEXT endpoint whatever t is)   :        24 (100.00%" in text, True)
        check("snapping scene localises", "distinct positions in x 2..10 y 2..5" in text, True)
        # The bands have to place it, not just count it: the box occupies four of twelve row bands
        # and none of the outer ones, so a localisation that files everything under one band or
        # spreads it evenly is wrong even though the totals above are right.
        check("row bands place the box",
              "by row band (top to bottom):  0.0% 50.0% 50.0%  0.0%  0.0%  0.0%  0.0%  0.0% "
              " 0.0%  0.0%  0.0%  0.0%" in text, True)
        check("col bands place the box",
              "by col band (left to right): 33.3% 16.7% 50.0%  0.0%" in text, True)

        # Across a CUT the same non-response is correct, and the split has to say so, or every
        # scene change in a capture reads as an interpolation failure.
        cut = os.path.join(root, "cut")
        os.makedirs(cut)
        Image.new("RGB", (48, 24), (0, 0, 0)).save(os.path.join(cut, "f100_0_real.png"))
        Image.new("RGB", (48, 24), (255, 255, 255)).save(os.path.join(cut, "f100_1_interp.png"))
        Image.new("RGB", (48, 24), (255, 255, 255)).save(os.path.join(cut, "f101_2_real.png"))
        cut_product = os.path.join(root, "cut_product")
        os.makedirs(cut_product)
        Image.new("RGB", (48, 24), (0, 0, 0)).save(os.path.join(cut_product, "f100_0_real.png"))
        Image.new("RGB", (48, 24), (128, 128, 128)).save(os.path.join(cut_product, "f100_1_interp.png"))
        Image.new("RGB", (48, 24), (255, 255, 255)).save(os.path.join(cut_product, "f101_2_real.png"))
        code, text = run(cut_product, cut)
        check("a cut is not called a defect", "1 are across a DISCONTINUITY" in text, True)
        check("a cut says nothing is continuous", "none is continuous" in text, True)

        # Several triples at once, because every statistic above is a single number over a list and
        # a one-triple fixture cannot tell a sorted list from an unsorted one. Here the FIRST triple
        # is the broken one, so "worst triple" naming the last fence would be the tell.
        def series(d, interp_at):
            """Five reals with the box at 2, 8, 14, 20, 26; interp_at(k) places the in-between."""
            os.makedirs(d)
            for k in range(5):
                frame(os.path.join(d, f"f{200 + k}_{2 * k}_real.png"), [(2 + 6 * k, (200, 30, 30))])
            for k in range(4):
                frame(os.path.join(d, f"f{200 + k}_{2 * k + 1}_interp.png"),
                      [(interp_at(k), (200, 30, 30))])
            return d

        many_product = series(os.path.join(root, "many_product"), lambda k: 5 + 6 * k)
        # forced: triple 0 snaps to the NEXT endpoint, the other three fall back to the previous.
        many_forced = series(os.path.join(root, "many_forced"),
                             lambda k: 8 if k == 0 else 2 + 6 * k)
        code, text = run(many_product, many_forced)
        check("four triples are all used", "4 triples" in text, True)
        check("the broken one is 1 of 4",
              "per triple: 3 of 4 are <=1% unresponsive, 1 are >=99%" in text, True)
        check("the worst triple is the FIRST fence, not the last",
              "worst triple: fence 200 at 100.00% unresponsive" in text, True)

        # A still scene must REFUSE, not score 100%: with no pixel changing between the endpoints
        # there is nothing for the factor to move, and reporting a pass there is the silent lie.
        still = os.path.join(root, "still")
        os.makedirs(still)
        for name in ("f100_0_real.png", "f100_1_interp.png", "f101_2_real.png"):
            frame(os.path.join(still, name), [(2, (200, 30, 30))])
        code, text = run(still, still)
        check("still scene refuses", code, 2)
        # ...and so must the TILE path, which had no such guard. The decision is
        # `tile_census_is_still`, driven here over the class counts a census produces, so both answers
        # are pinned: a still census refuses, and a census with any motion at all does not. The two
        # failure classes matter most -- an all-AHEAD or all-STALE census is a DEFECT (everything moved
        # and nothing was reconstructed), and the guard must not swallow it into "nothing measured",
        # because that would hide the very defect it exists to surface.
        check("tile census calls an all-STATIC census still",
              tile_census_is_still({"STATIC": 480, "BETWEEN": 0, "STALE": 0, "AHEAD": 0}), True)
        check("tile census does not call a reconstructed census still",
              tile_census_is_still({"STATIC": 100, "BETWEEN": 380, "STALE": 0, "AHEAD": 0}), False)
        check("tile census does not swallow an all-AHEAD defect into still",
              tile_census_is_still({"STATIC": 0, "BETWEEN": 0, "STALE": 0, "AHEAD": 480}), False)
        check("tile census does not swallow an all-STALE defect into still",
              tile_census_is_still({"STATIC": 0, "BETWEEN": 0, "STALE": 480, "AHEAD": 0}), False)
        check("still scene says nothing was measured", "NOTHING WAS MEASURED" in text, True)

        # The control: if a real frame differs, the two runs are not one route.
        drift = dump(os.path.join(root, "drift"), 2)
        frame(os.path.join(drift, "f101_2_real.png"), [(9, (200, 30, 30))])
        code, text = run(product, drift)
        check("a drifted route refuses", code, 2)
        check("drift names the count", "1 of 2 real frames differ" in text, True)

        # No shared fence at all is a different refusal from a drifted one.
        apart = os.path.join(root, "apart")
        os.makedirs(apart)
        frame(os.path.join(apart, "f900_0_real.png"), [(2, (200, 30, 30))])
        frame(os.path.join(apart, "f900_1_interp.png"), [(2, (200, 30, 30))])
        frame(os.path.join(apart, "f901_2_real.png"), [(8, (200, 30, 30))])
        code, text = run(product, apart)
        check("no shared fence refuses", code, 2)
        check("no shared fence says why", "share no real frame" in text, True)

        code, text = run(product, os.path.join(root, "absent"))
        check("a missing dir refuses", code, 2)
        check("a missing dir says nothing was compared", "NOTHING WAS COMPARED" in text, True)
    finally:
        shutil.rmtree(root, ignore_errors=True)


def selftest():
    """Prove the attribution can say every answer it is capable of printing.

    The failure this guards is silent: owner_of returning the FIRST covering run instead of the
    smallest credits the full-screen sky fill for everything drawn in front of it, and every row but
    one goes to zero without anything looking wrong."""
    log = ("[fps60seq] f7 t=0.500 emitted n=3\n"
           "  rqcur layer=2 verbatim  n=2 seq=[0..1] producer=00000000 node0=00000000 "
           "x=[-320..641) y=[0..241)\n"
           "  rqcur layer=1 TIER1     n=9 seq=[2..10] producer=0000ABCD node0=800E7E80 "
           "x=[100..140) y=[100..140)\n"
           "[fps60seq] f8 t=0.500 emitted n=0\n"
           # f9 is the negative the overlap question needs: a reconstructed run with NO verbatim
           # run anywhere near it. Without this fence, dropping the ownership filter entirely still
           # passes, because every other tile that a TIER1 run covers is under the full-screen
           # verbatim fill as well. Measured 2026-09-19: the filter was disconnected and the
           # selftest stayed green.
           "[fps60seq] f9 t=0.500 emitted n=1\n"
           "  rqcur layer=1 TIER1     n=4 seq=[0..3] producer=0000BEEF node0=800E7E80 "
           "x=[10..50) y=[10..50)\n")
    import tempfile
    with tempfile.NamedTemporaryFile("w", suffix=".log", delete=False) as fh:
        fh.write(log)
        path = fh.name
    try:
        runs = load_sequence_runs(path)
    finally:
        os.unlink(path)

    failures = []
    def check(name, got, want):
        if got != want:
            failures.append(f"  {name}: got {got!r}, want {want!r}")

    # f8 has a marker but no runs, so it gets no entry at all: a fence the log never described and
    # a fence the log described as empty are the same answer to "who drew here", and both must land
    # in the (no run) row rather than being credited to a neighbouring fence.
    check("fences with runs", sorted(runs), [7, 9])
    check("runs at f7", len(runs[7]), 2)

    # inside the small run: the SMALL one must win even though the big one also covers it
    inside = owner_of(runs[7], 112, 112, 16)
    check("small run wins where both cover", (inside[1], inside[2], inside[3]),
          (True, "0000ABCD", "800E7E80"))
    # outside it, only the full-screen run covers
    outside = owner_of(runs[7], 16, 200, 16)
    check("big run owns what only it covers", (outside[1], outside[2], outside[3]),
          (False, "00000000", "00000000"))
    # the producer is what a defect is actionable by, so it must not collapse into the node
    check("producer is carried apart from the node", inside[2] != inside[3], True)
    # a fence with no runs attributes nothing — the branch that prints "(no run)"
    check("no owner when no run covers", owner_of(runs.get(8, ()), 16, 16, 16), None)
    # and a tile outside every run in a populated fence is also unowned
    check("no owner above the full-screen run", owner_of(runs[7], 16, 300, 16), None)

    # The TIER1-vs-verbatim overlap question needs both answers too: a tile inside the small
    # reconstructed run also has the full-screen verbatim run drawn over it, while one outside
    # every verbatim extent does not. Without the negative, "they all overlap" is unfalsifiable.
    check("verbatim also covers the reconstructed tile",
          any_verbatim_over(runs[7], 112, 112, 16), True)
    check("no verbatim over a tile only a reconstructed run covers",
          any_verbatim_over(runs[9], 16, 16, 16), False)

    # The mapping check needs both answers or it is decoration.
    check("a mapping where runs predict motion is accepted",
          mapping_verdict({"display": (80, 100, 10, 100)})[0], True)
    check("a mapping where they predict nothing is refused",
          mapping_verdict({"display": (30, 100, 30, 100)})[0], False)
    # The control decides, not the constant: the SAME 1.25x lift is a pass when a wrong offset
    # scores 0.9 and a refusal when it scores 1.2. Without this pair the threshold is a guess.
    check("a weak lift the control cannot match is accepted",
          mapping_verdict({"display": (25, 100, 20, 100),
                           "shifted": (18, 100, 20, 100)})[0], True)
    check("a weak lift the control matches is refused",
          mapping_verdict({"display": (25, 100, 20, 100),
                           "shifted": (24, 100, 20, 100)})[0], False)
    check("a mapping another offset beats is refused",
          mapping_verdict({"display": (30, 100, 10, 100),
                           "raw": (90, 100, 10, 100)})[0], False)
    check("nothing scored is a refusal, not an agreement", mapping_verdict({})[0], False)

    # The fill must not be able to vouch for a mapping: f7's full-screen run covers everything.
    big_area = 961 * 241
    check("a screen-sized fill is not a specific run",
          covered_by_specific_run(runs[7], 16, 200, 16, big_area), False)
    check("a small run is a specific run",
          covered_by_specific_run(runs[7], 112, 112, 16, big_area), True)

    _forced_factor_selftest(check)

    if failures:
        print("fps60_check selftest: FAIL")
        print("\n".join(failures))
        return 1
    print("fps60_check selftest: PASS (43 checks: fence parsing, smallest-run wins, "
          "big-run-only, empty fence, out-of-range tile, verbatim overlap both ways, "
          "producer kept apart from node, mapping verdict six ways incl. a wrong-offset "
          "control, fill is not specific; forced-factor responds/snaps/still/drifted/"
          "disjoint/missing, band and per-triple placement, fence runs, cut vs continuous)")
    return 0


def tile_census_is_still(counts):
    """True when the census saw no motion at all, and so has nothing to say about interpolation.

    A still scene must REFUSE, not pass. The forced-factor path already refuses one correctly ("no
    pixel changed between the two real endpoints ... NOTHING WAS MEASURED"); the tile path had no
    equivalent, so a census over a capture where NOTHING MOVED reported "no STALE tiles -- everything
    that moved was interpolated" and "No interpolation failure at this tile size", and exited 0.
    Measured 2026-09-26 on Crash Bash, where the product reaches no game scene: four byte-identical
    black 512x234 presents, one triple, 480/480 tiles STATIC, and a pass-shaped verdict with a zero
    denominator for anything that moved.

    "No STALE and no AHEAD" is only good news if something moved AND was interpolated. BETWEEN being
    zero as well is what makes the census empty rather than clean: BETWEEN is the class that means
    "differed from BOTH endpoints", i.e. genuinely reconstructed, and STALE (identical to the older
    real frame) and AHEAD (identical to the newer one) are the two ways a MOVING tile can fail. A
    census with none of the three has measured nothing.
    """
    return counts["BETWEEN"] == 0 and counts["STALE"] == 0 and counts["AHEAD"] == 0


def classify(pa, pb, pc, w, h, tile):
    """Per-tile verdict counts + the tile grid. pa/pc real, pb interp."""
    grid = {}
    for ty in range(0, h, tile):
        for tx in range(0, w, tile):
            box = (tx, ty, min(tx + tile, w), min(ty + tile, h))
            ta, tb, tc = pa.crop(box).tobytes(), pb.crop(box).tobytes(), pc.crop(box).tobytes()
            if ta == tc:
                grid[(tx, ty)] = "STATIC" if tb == ta else "BETWEEN"
            elif tb == ta:
                grid[(tx, ty)] = "STALE"
            elif tb == tc:
                grid[(tx, ty)] = "AHEAD"
            else:
                grid[(tx, ty)] = "BETWEEN"
    return grid


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default="scratch/framedump")
    ap.add_argument("--tile", type=int, default=16)
    ap.add_argument("--top", type=int, default=12, help="how many worst regions to print")
    ap.add_argument("--triple", help="analyse only the triple starting at this fence/seq prefix")
    ap.add_argument("--seq", help="an fps60seq log for the same run: credits every MOVED endpoint "
                                  "tile to the smallest run covering it, so the defect has an owner")
    ap.add_argument("--forced", help="a second capture of the SAME route taken with "
                                     "PSXPORT_FPS60_TFORCE=0: reports, per pixel, whether the "
                                     "in-between present responded to the interpolation factor")
    ap.add_argument("--selftest", action="store_true",
                    help="check the attribution against fixtures with a known answer")
    args = ap.parse_args()

    if args.selftest:
        return selftest()

    if args.forced:
        return forced_factor_report(args.dir, args.forced)

    if not os.path.isdir(args.dir):
        sys.exit(f"fps60_check: no capture dir {args.dir} — run with PSXPORT_DEBUG=fps60dump first")
    frames = load_frames(args.dir)
    if len(frames) < 3:
        sys.exit(f"fps60_check: only {len(frames)} dumps in {args.dir} — need at least one "
                 f"real/interp/real triple")

    stale_hits = defaultdict(int)   # tile -> how many triples it was STALE in
    ahead_hits = defaultdict(int)
    # STALE/AHEAD split by whether the content actually translated between the two real frames.
    endpoint_shifts = {"STALE": defaultdict(int), "AHEAD": defaultdict(int)}
    moved_hits = defaultdict(int)   # tile -> how many triples anything moved there at all
    n_triples = totals = 0
    counts = defaultdict(int)
    sequence_runs = load_sequence_runs(args.seq) if args.seq else None
    shot_origins = load_shot_origins(args.seq) if args.seq else {}
    origins_missing = 0
    # An endpoint tile that MOVED is split by WHICH endpoint it landed on, because the two mean
    # opposite things. A moved STALE tile lerped to the wrong place or lerped too little. A moved
    # AHEAD tile shows the NEXT real frame's content a whole frame early, which is what content
    # that is never interpolated at all looks like: the extra frame draws it at its new position.
    # owner -> [moved STALE, moved AHEAD, 0px-endpoint, lerped]
    by_owner = defaultdict(lambda: [0, 0, 0, 0])
    unattributed = [0, 0, 0]  # moved STALE, moved AHEAD, lerped
    uncovered_fences = set()  # dump fences the fps60seq log never described
    # moved-AHEAD tiles filed under a TIER1 run, split by whether verbatim content is drawn there too
    tier1_ahead = [0, 0]  # [verbatim also covers this tile, TIER1 runs only]
    # {offset name: [hits, total]} — how well each candidate mapping explains the moving pixels
    # per candidate: [moved inside a specific run, tiles inside one, moved outside, tiles outside]
    # "shifted" is a NEGATIVE CONTROL, not a candidate: the display mapping moved half a frame
    # sideways and down, which cannot be right. It is what chance looks like on this data, and
    # without it a lift of 1.25 cannot be told from a lift the measure is simply too weak to raise.
    mapping_scores = {"display": [0, 0, 0, 0], "raw": [0, 0, 0, 0], "shifted": [0, 0, 0, 0]}
    attribution_refused = False

    for a, b, c in triples(frames):
        if args.triple and args.triple not in os.path.basename(a[2]):
            continue
        ia, ib, ic = (Image.open(p[2]).convert("RGB") for p in (a, b, c))
        w, h = ia.size
        if ib.size != (w, h) or ic.size != (w, h):
            print(f"  skip {os.path.basename(b[2])}: size mismatch", file=sys.stderr)
            continue
        grid = classify(ia, ib, ic, w, h, args.tile)
        # A tile's image coordinate is display-relative; a run's extent is VRAM-absolute in the
        # buffer that fence drew into, which is the one this real frame displays.
        origin = shot_origins.get(os.path.basename(c[2]), (0, 0))
        if sequence_runs is not None and os.path.basename(c[2]) not in shot_origins:
            origins_missing += 1
        n_triples += 1
        for tile, verdict in grid.items():
            counts[verdict] += 1
            totals += 1
            if verdict != "STATIC":
                moved_hits[tile] += 1
            if verdict == "STALE":
                stale_hits[tile] += 1
            elif verdict == "AHEAD":
                ahead_hits[tile] += 1
            shift = None
            if verdict in endpoint_shifts:
                shift = best_shift(ia, ic, tile[0], tile[1], args.tile)
                endpoint_shifts[verdict][shift] += 1
            if sequence_runs is not None:
                # Every tile is scored, moving or not: the question is whether being inside a run
                # PREDICTS motion, which a wrong mapping cannot make it do.
                fence_runs = sequence_runs.get(c[3], ())
                moved = 1 if verdict != "STATIC" else 0
                candidates = (("display", origin), ("raw", (0, 0)),
                              ("shifted", (origin[0] + w // 2, origin[1] + h // 2)))
                for name, (ox, oy) in candidates:
                    base = 0 if covered_by_specific_run(fence_runs, tile[0] + ox, tile[1] + oy,
                                                        args.tile, w * h) else 2
                    mapping_scores[name][base] += moved
                    mapping_scores[name][base + 1] += 1
            if sequence_runs is not None and verdict != "STATIC":
                if c[3] not in sequence_runs:
                    uncovered_fences.add(c[3])
                run = owner_of(sequence_runs.get(c[3], ()),
                               tile[0] + origin[0], tile[1] + origin[1], args.tile)
                if shift is not None and shift >= 1:
                    slot = 0 if verdict == "STALE" else 1
                elif shift is not None:
                    slot = 2
                else:
                    slot = 3
                if run is None:
                    unattributed[slot if slot < 2 else 2] += 1
                else:
                    by_owner[("TIER1" if run[1] else "verbatim", run[4], run[2], run[3])][slot] += 1
                    if slot == 1 and run[1]:
                        fence_runs = sequence_runs.get(c[3], ())
                        covered = any_verbatim_over(fence_runs, tile[0] + origin[0],
                                                    tile[1] + origin[1], args.tile)
                        tier1_ahead[0 if covered else 1] += 1
        if args.triple:
            print(f"tile map for {os.path.basename(b[2])} ({w}x{h}, tile={args.tile}):")
            sym = {"STATIC": ".", "BETWEEN": "-", "STALE": "S", "AHEAD": "A"}
            for ty in range(0, h, args.tile):
                print("  " + "".join(sym[grid[(tx, ty)]] for tx in range(0, w, args.tile)))

    if not n_triples:
        sys.exit("fps60_check: no real/interp/real triples found (is fps60 actually on?)")

    # A STILL SCENE MUST REFUSE, not pass -- see `tile_census_is_still` for the measurement that
    # motivated it. The guard is a named predicate rather than an inline test so the selftest drives
    # the same decision this path makes.
    if tile_census_is_still(counts):
        print(f"\n{n_triples} triple(s), tile={args.tile}px")
        print(f"  STATIC      {counts['STATIC']:6d}  (100.0%)")
        print(f"\nREFUSED: across {n_triples} triple(s) no tile was BETWEEN, STALE or AHEAD -- every tile "
              f"was STATIC, so nothing in this capture moved between the two real endpoints and nothing "
              f"could have been interpolated either way. This is a still scene, not a passing one. "
              f"NOTHING WAS MEASURED.")
        sys.exit(2)

    print(f"\n{n_triples} triple(s), tile={args.tile}px")
    for k in ("STATIC", "BETWEEN", "STALE", "AHEAD"):
        print(f"  {k:<8} {counts[k]:6d}  ({100.0 * counts[k] / totals:5.1f}%)")

    def report(name, hits, why):
        if not hits:
            print(f"\nno {name} tiles — {why}")
            return
        print(f"\nworst {name} regions (tile x,y -> triples affected / triples where it moved):")
        for tile, n in sorted(hits.items(), key=lambda kv: -kv[1])[:args.top]:
            print(f"  ({tile[0]:4d},{tile[1]:4d})  {n:4d} / {moved_hits[tile]:4d}")

    report("STALE", stale_hits, "everything that moved was interpolated")
    report("AHEAD", ahead_hits, "nothing snapped early")

    # The number that decides whether any of the above is a defect at all.
    print("\nendpoint tiles by how far their content actually translated between the two real "
          "frames:")
    moved_endpoint = 0
    for verdict in ("STALE", "AHEAD"):
        shifts = endpoint_shifts[verdict]
        total = sum(shifts.values())
        if not total:
            print(f"  {verdict:<6} none")
            continue
        parts = " ".join(f"{px}px:{n}" for px, n in sorted(shifts.items()))
        moved = total - shifts.get(0, 0)
        moved_endpoint += moved
        print(f"  {verdict:<6} {total:5d} tile(s): {parts}   -> {moved} that MOVED and still "
              f"landed on an endpoint")
    if sequence_runs is not None:
        if not sequence_runs:
            sys.exit(f"fps60_check: {args.seq} holds no fps60seq runs — was PSXPORT_DEBUG=fps60seq "
                     f"set for that run?")
        agree, detail = mapping_verdict({k: tuple(v) for k, v in mapping_scores.items()})
        attribution_refused = not agree
        print(f"\nowners of the moving tiles ({len(sequence_runs)} fence(s) in {args.seq}), "
              f"credited to the smallest covering run:")
        if attribution_refused:
            print(f"  REFUSED — {detail}.\n"
                  f"  Runs and image pixels are not describing the same place, so a tile would be\n"
                  f"  credited to whichever misaligned run happens to cover it. A screen-sized fill\n"
                  f"  covers every tile under any mapping, which is why coverage alone reported "
                  f"100%.\n  No owner table is printed. The classification above does not use "
                  f"attribution and\n  is unaffected.")
        else:
            print(f"  mapping checked: {detail}")
        attributed = sum(sum(v) for v in by_owner.values())
        total_moving = attributed + sum(unattributed)
        coverage = (100.0 * attributed / total_moving) if total_moving else 0.0
        bx0, by0, bx1, by1 = runs_bbox(sequence_runs)
        moved_attributed = sum(v[0] + v[1] for v in by_owner.values())
        moved_total = moved_attributed + unattributed[0] + unattributed[1]
        moved_coverage = (100.0 * moved_attributed / moved_total) if moved_total else 100.0
        if not attribution_refused:
            print(f"  {attributed} of {total_moving} moving tile(s) got an owner ({coverage:.1f}%); "
                  f"of the MOVED endpoint tiles, {moved_attributed} of {moved_total} "
                  f"({moved_coverage:.1f}%)")
        # Every drawn item belongs to exactly one run by construction, so a MOVING tile with no
        # owner is not a normal outcome — it means the run extents and the presented frame are not
        # describing the same pixels. Off-screen geometry makes the bbox legitimately larger than
        # the frame, so this cannot be turned into a clean refusal without a proven mapping; what it
        # CAN do is refuse to let the table be read as complete.
        if coverage < 90.0 and not attribution_refused:
            print(f"\n  INCOMPLETE — {100.0 - coverage:.1f}% of the moving tiles, and "
                  f"{100.0 - moved_coverage:.1f}% of the defects,\n  landed in the (no run) row. "
                  f"Every drawn item belongs to exactly one run, so this is\n  not geometry that "
                  f"nothing drew: the extents and the frame are not describing the\n  same pixels. "
                  f"Do not read the shares below as a breakdown of the whole.\n"
                  f"    frames    {w}x{h}\n"
                  f"    run bbox  x=[{bx0}..{bx1}) y=[{by0}..{by1})")
        if origins_missing and not attribution_refused:
            print(f"  WARNING: {origins_missing} of {n_triples} triple(s) had no gpu_shot line in "
                  f"the log,\n  so their runs were read at VRAM origin 0,0. A double-buffered title "
                  f"will mis-attribute\n  those. Capture fps60dump and fps60seq in ONE run so both "
                  f"land in the same log.")
        if uncovered_fences and not attribution_refused:
            print(f"  WARNING: {len(uncovered_fences)} of the {n_triples} triple(s) name a fence "
                  f"the log never described\n  (first: f{min(uncovered_fences)}) — their tiles are "
                  f"all in the (no run) row. Is this the same run?")
        rows = ([] if attribution_refused
                else sorted(by_owner.items(), key=lambda kv: -(kv[1][0] + kv[1][1])))
        if rows:
            print(f"  {'ownership':<10} {'layer':>5} {'producer':<10} {'node':<10} "
                  f"{'lerped':>8} {'endpoint':>9} {'MOVED to':>9} {'MOVED to':>9}")
            print(f"  {'':<10} {'':>5} {'':<10} {'':<10} {'':>8} {'':>9} {'prev':>9} {'next':>9}")
        for (ownership, layer, producer, node), (mstale, mahead, still, lerped) in rows:
            print(f"  {ownership:<10} {layer:5d} {producer:<10} {node:<10} {lerped:8d} "
                  f"{mstale + mahead + still:9d} {mstale:9d} {mahead:9d}")
        if not attribution_refused:
            print(f"  {'(no run)':<10} {'':>5} {'':<10} {'':<10} {unattributed[2]:8d} "
                  f"{sum(unattributed[:2]):9d} {unattributed[0]:9d} {unattributed[1]:9d}")
        print("  a MOVED endpoint tile is content that translated a whole pixel or more between the "
              "two real\n  frames and was still drawn at one of them. Those are the defects; the "
              "rest of the endpoint\n  column is sub-pixel quantisation and is correct output.\n"
              "  MOVED-to-next is the stronger signal: that content appeared at the NEXT real "
              "frame's\n  position a whole frame early, which is what never being interpolated "
              "looks like.")
        by_producer = defaultdict(int)
        for (ownership, _layer, producer, _node), counts in by_owner.items():
            if ownership == "verbatim":
                by_producer[producer] += counts[1]
        ranked = sorted((n, p) for p, n in by_producer.items() if n)
        if ranked and not attribution_refused:
            total_snap = sum(n for n, _ in ranked)
            print(f"\n  verbatim producers by forward snap — this is the list of temporal sources "
                  f"still to write:")
            for n, producer in sorted(ranked, reverse=True)[:10]:
                print(f"    {producer:<10} {n:7d}  ({100.0 * n / total_snap:.1f}% of the verbatim snap)")

        t1 = tier1_ahead[0] + tier1_ahead[1]
        if t1 and not attribution_refused:
            share = 100.0 * tier1_ahead[0] / t1
            print(f"\n  of the {t1} MOVED-to-next tile(s) filed under a TIER1 run, {tier1_ahead[0]} "
                  f"({share:.1f}%)\n  also have verbatim content drawn over them, and "
                  f"{tier1_ahead[1]} do not. A tile in the first\n  group is filed under a "
                  f"producer that may be working correctly: the pixels that snapped\n  forward "
                  f"can be the verbatim item sharing its screen area, not the reconstruction.")

    if moved_endpoint == 0:
        print("  every endpoint tile had a 0px shift: sub-pixel change quantised onto one side, "
              "which is correct output. No interpolation failure at this tile size.")
    else:
        print(f"  {moved_endpoint} tile(s) translated by a whole pixel or more and were still drawn "
              f"at an endpoint. THAT is the defect; the 0px ones are not.")


if __name__ == "__main__":
    sys.exit(main() or 0)
