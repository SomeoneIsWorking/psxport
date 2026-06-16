#!/usr/bin/env python3
"""Persistent INTERACTIVE driver for the native port (or the Beetle oracle).

Keeps ONE core process alive across many invocations via a FIFO, so the game can be
driven like a player — send a command, look at the result, send the next — instead of a
predetermined input script (continuous forced Start = pause menu = stops BGM; that
poisoned earlier captures). The core's REPL blocks reading the FIFO between commands, so
state persists and timing is whatever you type.

  tools/drive.py start [native|oracle] [extra env=val ...]   # launch (FMVs off by default for native)
  tools/drive.py send "run 30"                               # send command(s), print new output
  tools/drive.py send "tap start 6" "run 40" "shot scratch/screenshots/x.ppm"
  tools/drive.py out [N]                                     # reprint last N lines of output (default 40)
  tools/drive.py stop                                        # kill the core

Native REPL cmds: run N | r/rw addr [n] | w/w8 | watch lo hi | unwatch | hits | press/release/tap
  <btn> [n] | regs | seq | shot PATH | stage | quit.  Oracle: same-ish, buttons Capitalized.
Buttons (native): start select x/cross o/circle triangle square up down left right.
"""
import os, sys, time, subprocess, signal, re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RD   = os.path.join(ROOT, "scratch/repl")
FIN  = os.path.join(RD, "in")
FOUT = os.path.join(RD, "out")
PIDF = os.path.join(RD, "core.pid")
HPIDF= os.path.join(RD, "holder.pid")
OFFF = os.path.join(RD, "out.off")

def disc():
    p = os.environ.get("PSXPORT_TOMBA2_DISC") or os.environ.get("PSXPORT_DISC")
    if not p:
        env = os.path.join(ROOT, ".env")
        if os.path.exists(env):
            for ln in open(env):
                m = re.match(r'\s*PSXPORT_(?:TOMBA2_)?DISC\s*=\s*(.+)', ln)
                if m: p = m.group(1).strip()
    if not p: raise SystemExit("no disc (PSXPORT_TOMBA2_DISC or .env)")
    return p

def alive(pidf):
    try:
        pid = int(open(pidf).read().strip())
        os.kill(pid, 0); return pid
    except Exception:
        return None

def stop():
    for pf in (PIDF, HPIDF):
        pid = alive(pf)
        if pid:
            try: os.kill(pid, signal.SIGKILL)
            except Exception: pass
        try: os.remove(pf)
        except Exception: pass
    print("[drive] stopped")

def start(which, extra):
    stop()
    os.makedirs(RD, exist_ok=True)
    os.makedirs(os.path.join(ROOT, "scratch/screenshots"), exist_ok=True)
    if os.path.exists(FIN): os.remove(FIN)
    os.mkfifo(FIN)
    open(FOUT, "w").close()
    open(OFFF, "w").write("0")
    # "headed" => open a real window (+ audio) so a human can watch/hear and press buttons on the
    # window directly; default is headless (NOWINDOW/NOAUDIO) for batch diffing.
    headed = "headed" in extra
    extra = [e for e in extra if e != "headed"]
    env = dict(os.environ)
    if not headed:
        env["PSXPORT_NOWINDOW"] = "1"; env["PSXPORT_NOAUDIO"] = "1"
    for kv in extra:
        k, _, v = kv.partition("=")
        env[k] = v
    if which == "native":
        env.setdefault("PSXPORT_REPL", "1")
        env.setdefault("PSXPORT_NO_FMV", "1")
        env.setdefault("PSXPORT_BGMDBG", "1")
        if headed: env.setdefault("PSXPORT_GPU_WINDOW", "1")
        argv = [os.path.join(ROOT, "scratch/bin/tomba2_port"),
                os.path.join(ROOT, "scratch/bin/tomba2/MAIN.EXE")]
    else:
        argv = [os.path.join(ROOT, "runtime/wide60rt"), disc(),
                "-bios", os.path.join(ROOT, "scratch/bios"), "-repl"]
        if headed: argv += ["-play"]
        ls = env.get("PSXPORT_LOADSTATE")
        if ls: argv += ["-loadstate", ls]
    # O_RDWR on the FIFO keeps a writer end permanently open, so the core's blocking fgets
    # waits for input between commands instead of hitting EOF (O_NONBLOCK would EOF immediately).
    fin = os.open(FIN, os.O_RDWR)
    fout = open(FOUT, "wb")
    core = subprocess.Popen(argv, stdin=fin, stdout=fout, stderr=fout, env=env)
    open(PIDF, "w").write(str(core.pid))
    print(f"[drive] started {which} pid={core.pid} (env: {' '.join(extra) or 'defaults'})")
    time.sleep(1.0)
    _drain(print_it=True)

def _drain(print_it=True, settle=1.5, timeout=180.0):
    """Print output appended since last read; wait until it stops growing (settle) or timeout."""
    off = int(open(OFFF).read().strip() or "0")
    last_size = -1
    stable_since = None
    t0 = time.time()
    while True:
        sz = os.path.getsize(FOUT)
        if sz != last_size:
            last_size = sz; stable_since = time.time()
        elif stable_since and (time.time() - stable_since) >= settle:
            break
        if time.time() - t0 > timeout:
            break
        time.sleep(0.1)
    with open(FOUT, "rb") as f:
        f.seek(off)
        data = f.read()
    open(OFFF, "w").write(str(off + len(data)))
    if print_it:
        sys.stdout.write(data.decode("utf-8", "replace"))
        sys.stdout.flush()

def send(cmds):
    if not alive(PIDF): raise SystemExit("[drive] core not running (start first)")
    with open(FIN, "w") as f:
        for c in cmds:
            f.write(c.rstrip("\n") + "\n")
        f.flush()
    _drain(print_it=True)

def out(n):
    data = open(FOUT, "rb").read().decode("utf-8", "replace").splitlines()
    print("\n".join(data[-n:]))

def main():
    if len(sys.argv) < 2: print(__doc__); return 1
    cmd = sys.argv[1]
    if cmd == "start":
        which = sys.argv[2] if len(sys.argv) > 2 and not "=" in sys.argv[2] else "native"
        extra = [a for a in sys.argv[2:] if "=" in a]
        start(which, extra)
    elif cmd == "send":
        send(sys.argv[2:])
    elif cmd == "out":
        out(int(sys.argv[2]) if len(sys.argv) > 2 else 40)
    elif cmd == "stop":
        stop()
    else:
        print(__doc__); return 1
    return 0

if __name__ == "__main__":
    sys.exit(main())
