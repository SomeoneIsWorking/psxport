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

WHOSE tiles those are is the next question, and --seq answers it. Given an fps60seq log from the
SAME run, every moving tile is credited to the smallest run covering it (smallest, because the
full-screen sky fill covers everything drawn in front of it). That turned the cutscene's 1,547 into
one line: 1,471 of them belong to LAYER-2 VERBATIM runs — content no native producer reconstructs,
so the interpolated present replays it from the previous queue and it can only ever sit on an
endpoint. TIER1 entities own 73.

USAGE (from a consuming game, where external/psxport is the framework)
  PSXPORT_DEBUG=fps60dump ... <the game binary> ...   # capture (cap 600 files)
  PSXPORT_DEBUG=fps60seq  ... <the game binary> ...   # the owner log, SAME replay and window
  external/psxport/tools/port/fps60_check.py                            # walk scratch/framedump/
  external/psxport/tools/port/fps60_check.py --dir scratch/framedump --tile 16 --top 12
  external/psxport/tools/port/fps60_check.py --dir scratch/framedump --seq scratch/seq.log
  external/psxport/tools/port/fps60_check.py --triple f001234    # one triple, with a tile map
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

    A run is (area, owned, node, layer, x0, x1, y0, y1). Area is precomputed because every
    lookup wants the SMALLEST covering run: crediting a tile to whatever run happens to come first
    credits the full-screen sky fill for everything drawn in front of it."""
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
                     m.group(5), int(m.group(1)), x0, x1, y0, y1)
            if entry not in runs[fence]:
                runs[fence].append(entry)
    return runs


def runs_bbox(runs_by_fence):
    """The union of every run's extent, in whatever space the log wrote them."""
    x0 = y0 = x1 = y1 = None
    for runs in runs_by_fence.values():
        for _area, _owned, _node, _layer, rx0, rx1, ry0, ry1 in runs:
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
    for area, owned, node, layer, x0, x1, y0, y1 in runs:
        if owned:
            continue
        if tx < x1 and x0 < tx + tile and ty < y1 and y0 < ty + tile:
            return True
    return False


def owner_of(runs, tx, ty, tile):
    """The smallest run covering this tile, or None when no run does."""
    best = None
    for run in runs:
        area, _owned, _node, _layer, x0, x1, y0, y1 = run
        if tx < x1 and x0 < tx + tile and ty < y1 and y0 < ty + tile:
            if best is None or area < best[0]:
                best = run
    return best


def selftest():
    """Prove the attribution can say every answer it is capable of printing.

    The failure this guards is silent: owner_of returning the FIRST covering run instead of the
    smallest credits the full-screen sky fill for everything drawn in front of it, and every row but
    one goes to zero without anything looking wrong."""
    log = ("[fps60seq] f7 t=0.500 captured n=3\n"
           "  rqcur layer=2 verbatim  n=2 seq=[0..1] producer=00000000 node0=00000000 "
           "x=[-320..641) y=[0..241)\n"
           "  rqcur layer=1 TIER1     n=9 seq=[2..10] producer=0000ABCD node0=800E7E80 "
           "x=[100..140) y=[100..140)\n"
           "[fps60seq] f8 t=0.500 captured n=0\n"
           # f9 is the negative the overlap question needs: a reconstructed run with NO verbatim
           # run anywhere near it. Without this fence, dropping the ownership filter entirely still
           # passes, because every other tile that a TIER1 run covers is under the full-screen
           # verbatim fill as well. Measured 2026-09-19: the filter was disconnected and the
           # selftest stayed green.
           "[fps60seq] f9 t=0.500 captured n=1\n"
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
    check("small run wins where both cover", (inside[1], inside[2]), (True, "800E7E80"))
    # outside it, only the full-screen run covers
    outside = owner_of(runs[7], 16, 200, 16)
    check("big run owns what only it covers", (outside[1], outside[2]), (False, "00000000"))
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

    if failures:
        print("fps60_check selftest: FAIL")
        print("\n".join(failures))
        return 1
    print("fps60_check selftest: PASS (7 checks: fence parsing, smallest-run wins, "
          "big-run-only, empty fence, out-of-range tile, verbatim overlap both ways)")
    return 0


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
    ap.add_argument("--selftest", action="store_true",
                    help="check the attribution against fixtures with a known answer")
    args = ap.parse_args()

    if args.selftest:
        return selftest()

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
                    by_owner[("TIER1" if run[1] else "verbatim", run[3], run[2])][slot] += 1
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
        print(f"\nowners of the moving tiles ({len(sequence_runs)} fence(s) in {args.seq}), "
              f"credited to the smallest covering run:")
        attributed = sum(sum(v) for v in by_owner.values())
        total_moving = attributed + sum(unattributed)
        coverage = (100.0 * attributed / total_moving) if total_moving else 0.0
        bx0, by0, bx1, by1 = runs_bbox(sequence_runs)
        moved_attributed = sum(v[0] + v[1] for v in by_owner.values())
        moved_total = moved_attributed + unattributed[0] + unattributed[1]
        moved_coverage = (100.0 * moved_attributed / moved_total) if moved_total else 100.0
        print(f"  {attributed} of {total_moving} moving tile(s) got an owner ({coverage:.1f}%); "
              f"of the MOVED endpoint tiles, {moved_attributed} of {moved_total} "
              f"({moved_coverage:.1f}%)")
        # Every drawn item belongs to exactly one run by construction, so a MOVING tile with no
        # owner is not a normal outcome — it means the run extents and the presented frame are not
        # describing the same pixels. Off-screen geometry makes the bbox legitimately larger than
        # the frame, so this cannot be turned into a clean refusal without a proven mapping; what it
        # CAN do is refuse to let the table be read as complete.
        if coverage < 90.0:
            print(f"\n  INCOMPLETE — {100.0 - coverage:.1f}% of the moving tiles, and "
                  f"{100.0 - moved_coverage:.1f}% of the defects,\n  landed in the (no run) row. "
                  f"Every drawn item belongs to exactly one run, so this is\n  not geometry that "
                  f"nothing drew: the extents and the frame are not describing the\n  same pixels. "
                  f"Do not read the shares below as a breakdown of the whole.\n"
                  f"    frames    {w}x{h}\n"
                  f"    run bbox  x=[{bx0}..{bx1}) y=[{by0}..{by1})")
        if origins_missing:
            print(f"  WARNING: {origins_missing} of {n_triples} triple(s) had no gpu_shot line in "
                  f"the log,\n  so their runs were read at VRAM origin 0,0. A double-buffered title "
                  f"will mis-attribute\n  those. Capture fps60dump and fps60seq in ONE run so both "
                  f"land in the same log.")
        if uncovered_fences:
            print(f"  WARNING: {len(uncovered_fences)} of the {n_triples} triple(s) name a fence "
                  f"the log never described\n  (first: f{min(uncovered_fences)}) — their tiles are "
                  f"all in the (no run) row. Is this the same run?")
        rows = ([] if attribution_refused
                else sorted(by_owner.items(), key=lambda kv: -(kv[1][0] + kv[1][1])))
        if rows:
            print(f"  {'ownership':<10} {'layer':>5} {'node':<10} {'lerped':>8} {'endpoint':>9} "
                  f"{'MOVED to':>9} {'MOVED to':>9}")
            print(f"  {'':<10} {'':>5} {'':<10} {'':>8} {'':>9} {'prev':>9} {'next':>9}")
        for (ownership, layer, node), (mstale, mahead, still, lerped) in rows:
            print(f"  {ownership:<10} {layer:5d} {node:<10} {lerped:8d} "
                  f"{mstale + mahead + still:9d} {mstale:9d} {mahead:9d}")
        if not attribution_refused:
            print(f"  {'(no run)':<10} {'':>5} {'':<10} {unattributed[2]:8d} "
                  f"{sum(unattributed[:2]):9d} {unattributed[0]:9d} {unattributed[1]:9d}")
        print("  a MOVED endpoint tile is content that translated a whole pixel or more between the "
              "two real\n  frames and was still drawn at one of them. Those are the defects; the "
              "rest of the endpoint\n  column is sub-pixel quantisation and is correct output.\n"
              "  MOVED-to-next is the stronger signal: that content appeared at the NEXT real "
              "frame's\n  position a whole frame early, which is what never being interpolated "
              "looks like.")
        t1 = tier1_ahead[0] + tier1_ahead[1]
        if t1:
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
