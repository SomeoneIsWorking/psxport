#!/usr/bin/env python3
"""Native-vs-PSX RENDER comparison harness (user 2026-06-24).

Renders the SAME deterministic native game state two ways — the PC-native world-coord renderer and the PSX
recomp render path (g_render_psx / PSXPORT_RENDER_PSX) — at 1x internal / 4:3 / 30fps, and diffs the frames.
The native renderer MUST match PSX. Writes a triptych (native | psx | diff-magenta) + prints the diff %.

Usage: tools/render_cmp.py [repl-script-before-shot]
  default script drives to the seaside walkable field. Pass a custom REPL prelude (no 'shot'/'quit') to
  compare a different scene, e.g.:  tools/render_cmp.py $'newgame\\nrun 60\\nskip 600\\ntap 4008\\nrun 200'
"""
import os, subprocess, sys
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN  = os.path.join(ROOT, "scratch/bin/tomba2_port")
SHOTS= os.path.join(ROOT, "scratch/screenshots")
CMP_INI = os.path.join(ROOT, "scratch/cmp.ini")

PRELUDE = sys.argv[1] if len(sys.argv) > 1 else "newgame\nrun 60\nskip 600\nrun 30"

def write_cmp_ini():
    open(CMP_INI, "w").write("aspect=0\nires=1\nires_auto=0\nssao=0\nlight=0\nshadows=0\nfps60=0\n")

def run(out_ppm, render_psx):
    env = dict(os.environ, PSXPORT_SETTINGS=CMP_INI, PSXPORT_REPL="1", PSXPORT_VK_HEADLESS="1",
               PSXPORT_NOAUDIO="1", PSXPORT_NO_FMV="1")
    if render_psx: env["PSXPORT_RENDER_PSX"] = "1"
    script = PRELUDE + f"\nshot {out_ppm}\nquit\n"
    subprocess.run([BIN], input=script.encode(), env=env,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, cwd=ROOT)

def main():
    write_cmp_ini()
    nat = os.path.join(SHOTS, "cmp_native.ppm"); psx = os.path.join(SHOTS, "cmp_psx.ppm")
    run(nat, False); run(psx, True)
    a = Image.open(nat).convert("RGB"); b = Image.open(psx).convert("RGB")
    a.save(os.path.join(SHOTS, "cmp_native.png")); b.save(os.path.join(SHOTS, "cmp_psx.png"))
    if a.size != b.size:
        print(f"SIZE MISMATCH native={a.size} psx={b.size}"); return 1
    W, H = a.size; pa = a.load(); pb = b.load()
    diff = Image.new("RGB", (W, H)); pd = diff.load(); n = 0
    for y in range(H):
        for x in range(W):
            if pa[x, y] != pb[x, y]: pd[x, y] = (255, 0, 255); n += 1
            else: pd[x, y] = pa[x, y]
    comp = Image.new("RGB", (W*3+8, H), (20, 20, 20))
    comp.paste(a, (0, 0)); comp.paste(b, (W+4, 0)); comp.paste(diff, (2*W+8, 0))
    out = os.path.join(SHOTS, "cmp_triptych.png"); comp.save(out)
    print(f"native | psx | diff  -> {out}")
    print(f"differing pixels: {n}/{W*H} = {100.0*n/(W*H):.2f}%   (target: ~0% at 1x/4:3/30fps)")
    return 0

if __name__ == "__main__":
    sys.exit(main())
