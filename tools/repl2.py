#!/usr/bin/env python3
# Drive the native port and/or the Beetle oracle through their REPLs from one place.
# Both cores expose the same command set (run N | r/rw/w/w8 | watch lo hi / watch addr | unwatch |
# hits | press/release/tap <btn> | regs | seq | quit), so the same script drives either and the
# outputs can be diffed. Use for the BGM hunt: drive both to the menu, watch a libsnd address,
# compare what the oracle writes vs the native port.
#
#   tools/repl2.py native  "run 6; tap start; run 120; seq"
#   tools/repl2.py oracle  "run 200; r 80104c28 8"
#   tools/repl2.py both    "run 400; seq"            # runs each, prints both
# Commands are ';' or newline separated. A trailing 'quit' is appended automatically.
import os, sys, subprocess, re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

def disc():
    p = os.environ.get("PSXPORT_TOMBA2_DISC") or os.environ.get("PSXPORT_DISC")
    if p: return p
    env = os.path.join(ROOT, ".env")
    if os.path.exists(env):
        for ln in open(env):
            m = re.match(r'\s*PSXPORT_(?:TOMBA2_)?DISC\s*=\s*(.+)', ln)
            if m: return m.group(1).strip()
    raise SystemExit("no disc (set PSXPORT_TOMBA2_DISC or .env)")

def run_core(which, cmds, timeout=300):
    script = "\n".join(cmds) + "\nquit\n"
    env = dict(os.environ, PSXPORT_NOWINDOW="1", PSXPORT_NOAUDIO="1")
    if which == "native":
        env["PSXPORT_REPL"] = "1"
        argv = [os.path.join(ROOT, "scratch/bin/tomba2_port"),
                os.path.join(ROOT, "scratch/bin/tomba2/MAIN.EXE")]
    else:  # oracle
        argv = [os.path.join(ROOT, "runtime/wide60rt"), disc(),
                "-bios", os.path.join(ROOT, "scratch/bios"), "-repl"]
    p = subprocess.run(argv, input=script, capture_output=True, text=True, timeout=timeout)
    return p.stderr  # both cores log to stderr

def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    target = sys.argv[1]
    cmds = [c.strip() for c in re.split(r'[;\n]', sys.argv[2]) if c.strip()]
    cores = ["native", "oracle"] if target == "both" else [target]
    for which in cores:
        sys.stdout.write(f"\n===== {which} =====\n")
        out = run_core(which, cmds)
        # surface the REPL/state lines (skip the verbose boot spam)
        for ln in out.splitlines():
            if any(t in ln for t in ("[repl]", "[cw]", "[seqdbg]", "seq ", "stage=", "KON", "[spudbg]")):
                print(ln)

if __name__ == "__main__":
    main()
