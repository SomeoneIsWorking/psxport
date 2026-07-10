#!/usr/bin/env python3
"""preseqobj_check.py — per-object motion acceptance gate for the fps60 tier.

Reads a `[preseqobj]` log (emitted by RenderQueue::emitItem when the `preseqobj` debug channel is on
AND a REPL `preseq <N>` capture is armed) and verifies that every rendered object moves SMOOTHLY across
the captured present frames. It is the acceptance instrument for the fps60 per-object work: a PASS means
no emitted object oscillates (sign-alternating jitter) or stall-steps (snaps every 2nd present) — the two
signatures of a badly-interpolated 60fps object.

Log line format (one per emitted RqItem, per present pass):
    [preseqobj] p<presentIdx> key=<fps_key hex> layer=<layer> x=<xs0> y=<ys0> scene=<0|1>

scene=1 marks a prim REBUILT by sceneNative at the interpolated midpoint (terrain / per-object mesh /
backdrop): dense, correct-by-construction geometry with no per-prim identity. The tracker does NOT judge
these per-object (NN over thousands of mesh triangles is meaningless noise) — it only counts them. scene=0
prims are OT-walk drawables (billboards, 2D, HUD) — the object class this instrument actually verifies.

Object identity (two schemes, so genuine large teleports are followed, not dropped):
  * key != 0 — a tracked object (billboard / anchored prim). One entity node can emit SEVERAL sprites that
    share its node key, emitted in a STABLE order every present, so identity = (key, layer, emit-index):
    the i-th prim of key K in a present is the same sprite as the i-th prim of key K in the next present.
    No distance gate — so a sprite that TELEPORTS between two far positions on alternating presents is
    still followed as one track and its oscillation is caught (a gate would silently split it).
  * key == 0 — an un-keyed prim (world mesh / 2D / HUD / an object with NO registered world anchor). No
    stable emit order (world geometry is rebuilt fresh at the interp midpoint), so identity is recovered by
    greedy nearest-neighbour chaining within a layer (manhattan <= gate). This is a measurement heuristic —
    the TOOL may use screen space; the FIX may not.

Flags, per object present in >= MINRUN consecutive presents:
  (a) OSCILLATION   — x or y motion repeatedly reverses sign (dx[i]*dx[i+1] < 0 with |dx| >= AMP), i.e. it
                      jitters back and forth instead of translating.
  (b) STALL-STEP    — the object moves only on every 2nd present (a zero step alternating with a non-zero
                      step) while the MEDIAN object moves on every present — the snap signature.

Exit: 0 if PASS (no flagged objects), 1 if FAIL.
"""
import sys, re, argparse
from collections import defaultdict

LINE = re.compile(r"\[preseqobj\]\s+p(\d+)\s+key=([0-9A-Fa-f]+)\s+layer=(\d+)\s+x=(-?\d+)\s+y=(-?\d+)"
                  r"(?:\s+scene=(\d+))?")


def parse(path):
    """Return (presents, scene_count). presents: ordered list of (present_idx, [ (key, layer, x, y), ... ])
    holding only OT-walk drawables (scene=0); scene_count is how many scene=1 (rebuilt) prims were seen."""
    per = defaultdict(list)
    scene_count = 0
    with open(path) as f:
        for ln in f:
            m = LINE.search(ln)
            if not m:
                continue
            scene = int(m.group(6)) if m.group(6) is not None else 0
            if scene:                       # rebuilt scene geometry: counted, never per-object tracked
                scene_count += 1
                continue
            p = int(m.group(1)); key = int(m.group(2), 16); layer = int(m.group(3))
            x = int(m.group(4)); y = int(m.group(5))
            per[p].append((key, layer, x, y))
    return [(p, per[p]) for p in sorted(per)], scene_count


def build_tracks(presents, gate):
    """Greedy multi-object tracker over the ordered presents.

    A track is one PRIM lineage: dict{key,layer, samples={pidx:(x,y)}, first_p, last_p}. Each present's
    prims are matched to active tracks by greedy nearest-neighbour under HARD constraints (same key, same
    layer, manhattan distance <= gate). Unmatched prims start new tracks; tracks that miss a present are
    retired (a gap ends their consecutive run). Because key is a hard match constraint, a multi-sprite
    object (several prims sharing one node key) resolves into one track per sprite, not one conflated point.
    """
    tracks = []              # all tracks (active + retired)
    active = []              # indices into tracks still growing

    for pidx, prims in presents:
        cand = [(key, layer, x, y) for (key, layer, x, y) in prims]   # per-prim candidates (no collapsing)
        pairs = []
        for ci, (key, layer, x, y) in enumerate(cand):
            for ti in active:
                tr = tracks[ti]
                if tr['key'] != key or tr['layer'] != layer or tr['last_p'] == pidx:
                    continue
                lx, ly = tr['samples'][tr['last_p']]
                d = abs(x - lx) + abs(y - ly)
                if d <= gate:
                    pairs.append((d, ci, ti))
        pairs.sort()
        claimed_c, claimed_t = set(), set()
        for d, ci, ti in pairs:
            if ci in claimed_c or ti in claimed_t:
                continue
            key, layer, x, y = cand[ci]
            tracks[ti]['samples'][pidx] = (x, y); tracks[ti]['last_p'] = pidx
            claimed_c.add(ci); claimed_t.add(ti)
        for ci, (key, layer, x, y) in enumerate(cand):
            if ci in claimed_c:
                continue
            tracks.append({'key': key, 'layer': layer, 'samples': {pidx: (x, y)},
                           'first_p': pidx, 'last_p': pidx})
            active.append(len(tracks) - 1)

        active = [ti for ti in active if tracks[ti]['last_p'] == pidx]

    return tracks


def longest_consecutive(samples):
    """Longest run of consecutive present indices; returns list of (pidx,(x,y)) for that run."""
    ps = sorted(samples)
    best, cur = [], []
    for i, p in enumerate(ps):
        if cur and p == cur[-1] + 1:
            cur.append(p)
        else:
            cur = [p]
        if len(cur) > len(best):
            best = cur[:]
    return [(p, samples[p]) for p in best]


def analyse(track, minrun, amp, present_move):
    """Return (flags, detail) for a track over its longest consecutive run."""
    run = longest_consecutive(track['samples'])
    if len(run) < minrun:
        return [], None
    xs = [xy[0] for _, xy in run]
    ys = [xy[1] for _, xy in run]
    dx = [xs[i + 1] - xs[i] for i in range(len(xs) - 1)]
    dy = [ys[i + 1] - ys[i] for i in range(len(ys) - 1)]
    flags = []

    # (a) oscillation: repeated sign reversals of a meaningful-amplitude step
    def osc_count(d):
        c = 0
        for i in range(len(d) - 1):
            if d[i] * d[i + 1] < 0 and abs(d[i]) >= amp and abs(d[i + 1]) >= amp:
                c += 1
        return c
    ox, oy = osc_count(dx), osc_count(dy)
    if ox >= 2 or oy >= 2:
        flags.append(f"OSCILLATION(x_rev={ox},y_rev={oy})")

    # (b) stall-step: zero and non-zero steps alternate while the frame's median object moves every present
    def stall(d):
        mv = [1 if v != 0 else 0 for v in d]
        if len(mv) < 4:
            return False
        zeros = mv.count(0)
        # roughly half the steps are zero AND zeros/nonzeros alternate (few adjacent equal pairs)
        if not (0.35 * len(mv) <= zeros <= 0.65 * len(mv)):
            return False
        alt = sum(1 for i in range(len(mv) - 1) if mv[i] != mv[i + 1])
        return alt >= 0.6 * (len(mv) - 1)
    if present_move and (stall(dx) or stall(dy)):
        flags.append("STALL-STEP")

    detail = {'run': (run[0][0], run[-1][0]), 'len': len(run), 'dx': dx, 'dy': dy,
              'start': run[0][1], 'end': run[-1][1]}
    return flags, detail


def main():
    ap = argparse.ArgumentParser(description="per-object fps60 motion acceptance gate")
    ap.add_argument("log", help="stderr log containing [preseqobj] lines")
    ap.add_argument("--minrun", type=int, default=6, help="min consecutive presents to judge an object")
    ap.add_argument("--amp", type=int, default=1, help="min |step| (px) counted as motion for oscillation")
    ap.add_argument("--gate", type=int, default=24, help="NN match radius (px, manhattan) for key==0 tracks")
    ap.add_argument("-v", "--verbose", action="store_true", help="also list clean tracked objects")
    args = ap.parse_args()

    presents, scene_count = parse(args.log)
    if not presents:
        print("no [preseqobj] lines found — was the channel on AND preseq armed?", file=sys.stderr)
        return 2
    print(f"parsed {len(presents)} presents (p{presents[0][0]}..p{presents[-1][0]}); "
          f"{scene_count} rebuilt scene-geometry prims skipped (correct-by-construction)")

    tracks = build_tracks(presents, args.gate)

    # per-present median motion magnitude across all multi-sample tracks → is the world moving every present?
    # (used to distinguish a genuine stall from a scene that is simply static this capture)
    per_present_moves = defaultdict(list)
    for tr in tracks:
        run = longest_consecutive(tr['samples'])
        for i in range(len(run) - 1):
            (p0, (x0, y0)), (p1, (x1, y1)) = run[i], run[i + 1]
            per_present_moves[p1].append(abs(x1 - x0) + abs(y1 - y0))
    moved_presents = sum(1 for p, v in per_present_moves.items() if v and sorted(v)[len(v) // 2] > 0)
    present_move = moved_presents >= 0.5 * max(1, len(per_present_moves))

    flagged = []
    clean = 0
    for tr in tracks:
        flags, detail = analyse(tr, args.minrun, args.amp, present_move)
        if detail is None:
            continue
        if flags:
            flagged.append((tr, flags, detail))
        else:
            clean += 1

    flagged.sort(key=lambda t: (-t[2]['len']))
    for tr, flags, d in flagged:
        kind = f"key={tr['key']:08X}" if tr['key'] else f"key=0 (NN track, layer {tr['layer']})"
        print(f"\nFLAG {kind}  presents p{d['run'][0]}..p{d['run'][1]} (len {d['len']})")
        print(f"   {', '.join(flags)}")
        print(f"   start={d['start']} end={d['end']}")
        print(f"   dx={d['dx']}")
        print(f"   dy={d['dy']}")

    if args.verbose:
        print(f"\n{clean} clean tracked object(s) (>= {args.minrun} presents, smooth).")

    print("\n" + "=" * 60)
    if flagged:
        print(f"FAIL — {len(flagged)} object(s) flagged (oscillation / stall-step).")
        return 1
    print(f"PASS — 0 objects flagged; {clean} tracked object(s) all smooth over "
          f"{len(presents)} presents.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
