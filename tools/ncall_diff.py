#!/usr/bin/env python3
# ncall_diff — differential native-call tracer driver. Runs TWO port builds in the deterministic
# headless AUTO path, captures each one's native-call I/O trace (PSXPORT_NCALL_TRACE — every
# override/BIOS call's inputs a0-a3 and outputs v0/v1), and reports the FIRST divergence.
#
# Because the interpreter + guest memory are deterministic and byte-identical across the two builds,
# the native-call sequence is identical until a native function behaves differently. The first line
# with:
#   * identical inputs but DIFFERENT outputs  -> THAT override/BIOS fn is the broken one (its
#     conversion changed a return value / register effect).
#   * DIFFERENT inputs                          -> an earlier native call's MEMORY side-effect
#     diverged (the bad data was read into a register before this call); look just above.
#
# Usage:
#   tools/ncall_diff.py <exeA> <exeB> [frames]      # default frames=20
#   tools/ncall_diff.py            -> defaults A=scratch/bin/tomba2_port  B=scratch/oop/parent/...
# Drives both with AUTO_GAMEPLAY + the faithful flags (all liftable overrides OFF) so only the
# always-on natives (CD/timing/frame-update/cull/depth) + BIOS are in play — the OOP-regression set.
import os, sys, subprocess, time, re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAIN = os.path.join(ROOT, "scratch/bin/tomba2/MAIN.EXE")
FAITHFUL = dict(PSXPORT_SUBMIT_RECOMP="1", PSXPORT_PEROBJ_RECOMP="1", PSXPORT_OT_RECOMP="1",
                PSXPORT_GEOM_RECOMP="1", PSXPORT_LZ_RECOMP="1", PSXPORT_FAITHFUL_DEPTH="1")

def disc():
    for ln in open(os.path.join(ROOT, ".env")):
        m = re.match(r'\s*PSXPORT_(?:TOMBA2_)?DISC\s*=\s*(.+)', ln)
        if m: return m.group(1).strip()
    raise SystemExit("no disc in .env")

def run(exe, out, frames):
    fin = out + ".in"
    if os.path.exists(fin): os.remove(fin)
    os.mkfifo(fin)
    log = open(out + ".log", "wb")
    env = dict(os.environ, PSXPORT_REPL="1", PSXPORT_VK_HEADLESS="1", PSXPORT_NOAUDIO="1",
               PSXPORT_AUTO_GAMEPLAY="1", PSXPORT_TOMBA2_DISC=disc(), PSXPORT_NCALL_TRACE=out, **FAITHFUL)
    fd = os.open(fin, os.O_RDWR)
    p = subprocess.Popen([exe, MAIN], stdin=fd, stdout=log, stderr=log, env=env)
    time.sleep(2)
    w = os.fdopen(os.open(fin, os.O_WRONLY), "w")
    w.write("run %d\n" % frames); w.flush(); time.sleep(max(6, frames * 0.4))
    w.write("quit\n"); w.flush(); time.sleep(1)
    p.terminate()
    try: p.wait(timeout=5)
    except Exception: p.kill()
    return out

def main():
    a = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "scratch/bin/tomba2_port")
    b = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "scratch/oop/parent/scratch/bin/tomba2_port")
    frames = int(sys.argv[3]) if len(sys.argv) > 3 else 20
    od = os.path.join(ROOT, "scratch/oop"); os.makedirs(od, exist_ok=True)
    ta = run(a, os.path.join(od, "ncall_A.trace"), frames)
    tb = run(b, os.path.join(od, "ncall_B.trace"), frames)
    A = open(ta).read().splitlines()
    B = open(tb).read().splitlines()
    print(f"[ncall_diff] A={len(A)} calls  B={len(B)} calls")
    def parts(l):  # "seq kind tgt  a:.. -> v:.."  -> (kind+tgt+inputs, outputs)
        m = re.match(r'\d+ (\S) (\S+)\s+a:(.+?) -> v:(.+)', l)
        return (m.group(1), m.group(2), m.group(3).strip(), m.group(4).strip()) if m else (None,)*4
    n = min(len(A), len(B))
    for i in range(n):
        ka, ta_, ia, oa = parts(A[i]); kb, tb_, ib, ob = parts(B[i])
        if (ka, ta_, ia) != (kb, tb_, ib) or oa != ob:
            kind = "DIFFERENT INPUTS (upstream memory side-effect diverged — look above)" \
                   if (ka, ta_, ia) != (kb, tb_, ib) else \
                   "SAME INPUTS, DIFFERENT OUTPUTS  <<< this native fn is the broken one"
            print(f"\n=== FIRST DIVERGENCE at call #{i}: {kind} ===")
            lo = max(0, i - 4)
            print("--- A (mine) ---");   [print("  " + A[j]) for j in range(lo, min(i + 2, len(A)))]
            print("--- B (parent) ---"); [print("  " + B[j]) for j in range(lo, min(i + 2, len(B)))]
            return 0
    print("[ncall_diff] no divergence in the common prefix (A and B identical up to %d calls)" % n)
    return 0

if __name__ == "__main__":
    sys.exit(main())
