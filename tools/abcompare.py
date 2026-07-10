#!/usr/bin/env python3
"""abcompare — the ./run.sh-vs-oracle compare (docs/abcompare-design.md).

Runs TWO fully isolated processes from one probe script and diffs them at each probe:

  side A  =  PSXPORT_PC_SKIP=0  (pc_faithful + pc_render — the native renderer under test)
  side B  =  PSXPORT_ORACLE=1   (pure recomp + pure PSX render — the trustworthy oracle)

pc_faithful is byte-exact to recomp_path per frame (the SBS Job#1 invariant), so the two
processes stay frame-locked by construction: at every probe the guest RAM must match
byte-for-byte (strict — pc_skip=0 needs no scratch mask), and any PIXEL difference is a
pure RENDERER divergence. No in-process coupling: no shared VRAM texture, no shared
g_mods/oracle_mode(), no SBS composite present, no SBS FMV pokes.

Probe script format (see tools/golden_drive.repl): plain REPL lines, plus
    probe <name>
which expands per side into `dumpram <out>/<side>/raw/<name>.ram` +
`shot <out>/<side>/shots/<name>.png`.

Usage:
    tools/abcompare.py [--script tools/golden_drive.repl] [--out scratch/abcompare/<runid>]
                       [--tol 40] [--thresh 0.5] [--timeout 1800] [--no-build] [disc.chd]

Mods are pinned to factory-neutral for BOTH sides (PSXPORT_SETTINGS -> nonexistent file:
4:3, 1x internal res, ssao/light/shadows/fps60 off) so the pictures are geometrically
comparable; the enhancements are not what this harness verifies.

Exit codes: 0 = no divergence, 1 = pixel divergence (renderer bug evidence written),
2 = STATE divergence (compare invalid from that probe on — fix Job#1 first), 3 = harness error.
"""
import argparse
import os
import subprocess
import sys
import time

import numpy as np
from PIL import Image

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RAM_SIZE = 0x200000
SPAD_SIZE = 0x400
RAM_BASE = 0x80000000
SPAD_BASE = 0x1F800000


def say(msg):
    print(f"[abcompare] {msg}", flush=True)


def resolve_disc(cli_arg):
    """Same precedence as run.sh: CLI > PSXPORT_TOMBA2_DISC > .env > *.chd drop-in."""
    if cli_arg:
        return cli_arg
    d = os.environ.get("PSXPORT_TOMBA2_DISC")
    if d:
        return d
    env_path = os.path.join(REPO, ".env")
    if os.path.isfile(env_path):
        for line in open(env_path):
            line = line.strip()
            for key in ("PSXPORT_TOMBA2_DISC", "PSXPORT_DISC"):
                if line.startswith(key):
                    _, _, v = line.partition("=")
                    v = v.strip()
                    if v:
                        return v
    import glob
    hits = sorted(glob.glob(os.path.join(REPO, "*.chd")))
    return hits[0] if hits else None


def build(disc):
    """Provision recomp + build the port (same steps as run.sh, minus the launch)."""
    say("building (cmake configure + discdump + ensure_recomp + tomba2_port)…")
    run = lambda *cmd, **kw: subprocess.run(cmd, cwd=REPO, check=True, **kw)
    run("cmake", "-S", ".", "-B", "build", "-DCMAKE_BUILD_TYPE=Release",
        stdout=subprocess.DEVNULL)
    run("cmake", "--build", "build", "-j", str(os.cpu_count() or 4), "--target", "discdump",
        stdout=subprocess.DEVNULL)
    discdump = os.path.join(REPO, "build/tools/discdump")
    env = dict(os.environ, PSXPORT_DISCDUMP=discdump)
    subprocess.run([sys.executable, "tools/ensure_recomp.py", disc], cwd=REPO, check=True, env=env)
    run("cmake", "--build", "build", "-j", str(os.cpu_count() or 4), "--target", "tomba2_port")


def expand_script(script_path, out_dir, side):
    """Expand `probe <name>` lines into per-side dumpram+shot; return (text, probe_names)."""
    probes = []
    lines = []
    for raw in open(script_path):
        line = raw.strip()
        if line.startswith("probe "):
            name = line.split(None, 1)[1].strip()
            probes.append(name)
            lines.append(f"dumpram {out_dir}/{side}/raw/{name}.ram")
            lines.append(f"shot {out_dir}/{side}/shots/{name}.png")
        else:
            lines.append(raw.rstrip("\n"))
    if not any(l.strip() == "quit" for l in lines):
        lines.append("quit")
    return "\n".join(lines) + "\n", probes


def launch(side, extra_env, script_text, out_dir, disc):
    """Start one isolated headless run; returns (Popen, logfile handle)."""
    port = os.path.join(REPO, "scratch/bin/tomba2_port")
    main = os.path.join(REPO, "scratch/bin/tomba2/MAIN.EXE")
    script_file = os.path.join(out_dir, f"{side}.repl")
    with open(script_file, "w") as f:
        f.write(script_text)
    env = dict(os.environ)
    env.update({
        "PSXPORT_VK_HEADLESS": "1",
        "PSXPORT_REPL": "1",
        "PSXPORT_DEBUG_SERVER": "0",
        # Pin factory-neutral mods (points at a file that never exists in the run dir).
        "PSXPORT_SETTINGS": os.path.join(out_dir, "no_settings.ini"),
        "PSXPORT_TOMBA2_DISC": disc,
    })
    env.update(extra_env)
    log = open(os.path.join(out_dir, f"{side}.log"), "wb")
    proc = subprocess.Popen([port, main], cwd=REPO, env=env,
                            stdin=open(script_file, "rb"), stdout=log, stderr=log)
    return proc, log


def diff_ram(path_a, path_b, base):
    """Byte-diff two raw dumps; returns (ndiff, first_addr, last_addr)."""
    a = np.fromfile(path_a, dtype=np.uint8)
    b = np.fromfile(path_b, dtype=np.uint8)
    n = min(len(a), len(b))
    neq = np.nonzero(a[:n] != b[:n])[0]
    if len(a) != len(b):
        return max(len(a), len(b)) - n + len(neq), base + (neq[0] if len(neq) else n), base + n
    if len(neq) == 0:
        return 0, 0, 0
    return len(neq), base + int(neq[0]), base + int(neq[-1])


def _shift_min_diff(ia, ib, tol):
    """Per-pixel structural-difference mask with ±1px spatial tolerance: a pixel differs only
    if NO pixel in the other image's 3x3 neighborhood matches it within ±tol/channel (both
    directions, so neither a missing nor an extra object can hide). Absorbs the 1px edge
    jitter of native-float vs PSX-fixed rasterization; a missing character/prim still shows."""
    h, w, _ = ia.shape
    pad_b = np.pad(ib, ((1, 1), (1, 1), (0, 0)), mode="edge")
    pad_a = np.pad(ia, ((1, 1), (1, 1), (0, 0)), mode="edge")
    m_ab = np.ones((h, w), dtype=bool)
    m_ba = np.ones((h, w), dtype=bool)
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            sb = pad_b[1 + dy:1 + dy + h, 1 + dx:1 + dx + w]
            m_ab &= (np.abs(ia - sb) > tol).any(axis=2)
            sa = pad_a[1 + dy:1 + dy + h, 1 + dx:1 + dx + w]
            m_ba &= (np.abs(sa - ib) > tol).any(axis=2)
    return m_ab | m_ba


def diff_pixels(path_a, path_b, tol, composite_path):
    """Tolerance-gated pixel diff (same doctrine as the SBS renderdiff: ±tol/channel absorbs
    PSX-fixed-vs-float dither noise; what survives is structural). Returns (pct, bbox, w, h)."""
    ia = np.asarray(Image.open(path_a).convert("RGB"), dtype=np.int16)
    ib = np.asarray(Image.open(path_b).convert("RGB"), dtype=np.int16)
    h = min(ia.shape[0], ib.shape[0])
    w = min(ia.shape[1], ib.shape[1])
    mask = _shift_min_diff(ia[:h, :w], ib[:h, :w], tol)
    ndiff = int(mask.sum())
    pct = 100.0 * ndiff / float(w * h)
    bbox = None
    if ndiff:
        ys, xs = np.nonzero(mask)
        bbox = (int(xs.min()), int(xs.max()), int(ys.min()), int(ys.max()))
        # Evidence composite: A | B | diff-mask (differing pixels red on black).
        dm = np.zeros((h, w, 3), dtype=np.uint8)
        dm[mask] = (255, 40, 40)
        comp = np.concatenate([ia[:h, :w].astype(np.uint8), ib[:h, :w].astype(np.uint8), dm], axis=1)
        Image.fromarray(comp).save(composite_path)
    return pct, bbox, w, h


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("disc", nargs="?", help="disc image (default: run.sh resolution order)")
    ap.add_argument("--script", default="tools/golden_drive.repl")
    ap.add_argument("--out", default=None, help="output dir (default scratch/abcompare/<runid>)")
    ap.add_argument("--tol", type=int, default=40, help="per-channel pixel tolerance (default 40)")
    ap.add_argument("--thresh", type=float, default=0.5, help="differing-pixel %% that counts as a divergence")
    ap.add_argument("--timeout", type=int, default=1800, help="per-side wall-clock timeout (s)")
    ap.add_argument("--no-build", action="store_true", help="skip the build step (binary is current)")
    args = ap.parse_args()

    disc = resolve_disc(args.disc)
    if not disc or not os.path.isfile(disc):
        say("error: no disc image (pass one, set PSXPORT_TOMBA2_DISC, or drop a *.chd in the repo)")
        return 3
    runid = time.strftime("%Y%m%d_%H%M%S")
    out_dir = os.path.abspath(args.out or os.path.join(REPO, "scratch/abcompare", runid))
    for side in ("A", "B"):
        os.makedirs(os.path.join(out_dir, side, "raw"), exist_ok=True)
        os.makedirs(os.path.join(out_dir, side, "shots"), exist_ok=True)
    os.makedirs(os.path.join(out_dir, "diff"), exist_ok=True)

    if not args.no_build:
        build(disc)

    text_a, probes = expand_script(args.script, out_dir, "A")
    text_b, _ = expand_script(args.script, out_dir, "B")
    if not probes:
        say(f"error: no `probe <name>` lines in {args.script} — nothing to compare")
        return 3

    say(f"out: {out_dir}")
    say(f"side A = PSXPORT_PC_SKIP=0 (pc_faithful + pc_render) | side B = PSXPORT_ORACLE=1 (recomp + psx_render)")
    say(f"launching both sides in parallel ({len(probes)} probes: {', '.join(probes)})…")
    t0 = time.time()
    pa, la = launch("A", {"PSXPORT_PC_SKIP": "0"}, text_a, out_dir, disc)
    pb, lb = launch("B", {"PSXPORT_ORACLE": "1"}, text_b, out_dir, disc)
    rc = {}
    for side, proc in (("A", pa), ("B", pb)):
        try:
            rc[side] = proc.wait(timeout=args.timeout)
        except subprocess.TimeoutExpired:
            proc.kill()
            rc[side] = "TIMEOUT"
    la.close(); lb.close()
    say(f"runs done in {time.time() - t0:.0f}s (A rc={rc['A']}, B rc={rc['B']})")
    for side in ("A", "B"):
        if rc[side] != 0:
            say(f"WARNING: side {side} exited {rc[side]} — see {out_dir}/{side}.log; "
                f"probes after its last output are missing")

    # ---- compare -------------------------------------------------------------------
    state_bad = pixel_bad = harness_bad = False
    print()
    print(f"{'probe':<16} {'RAM diff':<28} {'spad diff':<20} {'pixel diff':<30}")
    print("-" * 96)
    for name in probes:
        ra = os.path.join(out_dir, "A/raw", f"{name}.ram")
        rb = os.path.join(out_dir, "B/raw", f"{name}.ram")
        sa = os.path.join(out_dir, "A/shots", f"{name}.png")
        sb = os.path.join(out_dir, "B/shots", f"{name}.png")
        if not (os.path.isfile(ra) and os.path.isfile(rb) and os.path.isfile(sa) and os.path.isfile(sb)):
            print(f"{name:<16} MISSING ARTIFACTS (side crashed/timed out before this probe)")
            harness_bad = True
            continue
        n, lo, hi = diff_ram(ra, rb, RAM_BASE)
        ram_s = "0 (byte-exact)" if n == 0 else f"{n} bytes 0x{lo:08X}..0x{hi:08X}"
        ns, slo, shi = diff_ram(ra + ".spad", rb + ".spad", SPAD_BASE)
        spad_s = "0" if ns == 0 else f"{ns} bytes 0x{slo:08X}.."
        if n or ns:
            state_bad = True
        comp = os.path.join(out_dir, "diff", f"{name}.png")
        pct, bbox, w, h = diff_pixels(sa, sb, args.tol, comp)
        if pct >= args.thresh:
            pixel_bad = True
            px_s = f"{pct:.2f}% bbox x[{bbox[0]}..{bbox[1]}] y[{bbox[2]}..{bbox[3]}]"
        else:
            px_s = f"{pct:.2f}% (< {args.thresh}% thresh)"
        print(f"{name:<16} {ram_s:<28} {spad_s:<20} {px_s:<30}")
    print()

    if state_bad:
        say("STATE DIVERGENCE — pc_faithful is not byte-exact to the oracle at a probe; the pixel")
        say("compare is not meaningful past the first state diff. This is a Job#1 (SBS) bug first.")
    if pixel_bad:
        say(f"pixel divergence(s) — evidence composites (A | B | diff-mask) in {out_dir}/diff/")
    if not state_bad and not pixel_bad and not harness_bad:
        say("0-diff: state byte-exact and pictures match at every probe.")
    return 2 if state_bad else (3 if harness_bad else (1 if pixel_bad else 0))


if __name__ == "__main__":
    sys.exit(main())
