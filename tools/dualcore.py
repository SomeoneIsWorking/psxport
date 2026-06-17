#!/usr/bin/env python3
"""Dual-core differential harness — run the native PORT and the Beetle ORACLE together, SYNC BOTH
ON A GAME STATE (the stage latch in guest RAM, NOT a frame number), then diff framebuffers there.

WHY: the port's native boot and the oracle's full-disc boot have totally different frame timing, so
comparing port-f1500 vs oracle-f1500 is apples-to-oranges. Instead we gate both cores to the same
guest-RAM stage word (0x801fe00c) — e.g. the attract/demo stage 0x801062E4 — then step both together
and compare at that synced state. (See memory dual-core-state-synced-diff / docs/diff-driver.md.)

  tools/dualcore.py start [oraclestate=PATH]   # launch BOTH cores (oracle can warm-start from a savestate)
  tools/dualcore.py stage                       # print each core's current stage word (0x801fe00c)
  tools/dualcore.py sync [STAGEHEX] [cap=N]     # advance each core until stage==STAGEHEX (default DEMO 801062E4)
  tools/dualcore.py step N                       # advance BOTH cores N frames (keeps them in lockstep)
  tools/dualcore.py shot NAME                    # screenshot both -> side-by-side + diff heatmap (scratch/screenshots/NAME_*)
  tools/dualcore.py send port|oracle "cmd" ...   # raw REPL passthrough to one core (tap/press/r/...)
  tools/dualcore.py stop

The two cores share the SAME MAIN.EXE game logic (port = recompiled, oracle = emulated), so 0x801fe00c
is the stage in both. Frame counts differ; the stage word does not.
"""
import os, sys, time, subprocess, signal, re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DUAL = os.path.join(ROOT, "scratch/dual")
SHOTS = os.path.join(ROOT, "scratch/screenshots")
STAGE_ADDR = 0x801fe00c
# Default sync latch = the SCENE-ACTIVE word 0x800BE258 (==2 when a field/gameplay scene incl. the
# attract DEMO is live; ==0 at title). Far better than the stage word 0x801fe00c, which is just
# "attract mode" (0x801062E4) and stays the same for BOTH the title screen and the playing demo.
DEFAULT_LATCH = (0x800BE258, 2)

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

class Core:
    """One core process driven over a private FIFO (same O_RDWR trick as drive.py)."""
    def __init__(self, name):
        self.name = name
        self.dir = os.path.join(DUAL, name)
        self.fin = os.path.join(self.dir, "in")
        self.fout = os.path.join(self.dir, "out")
        self.pidf = os.path.join(self.dir, "pid")
        self.off = 0

    def alive(self):
        try:
            pid = int(open(self.pidf).read()); os.kill(pid, 0); return pid
        except Exception:
            return None

    def stop(self):
        pid = self.alive()
        if pid:
            try: os.kill(pid, signal.SIGKILL)
            except Exception: pass
        try: os.remove(self.pidf)
        except Exception: pass

    def start(self, oraclestate=None):
        self.stop()
        os.makedirs(self.dir, exist_ok=True); os.makedirs(SHOTS, exist_ok=True)
        if os.path.exists(self.fin): os.remove(self.fin)
        os.mkfifo(self.fin); open(self.fout, "w").close(); self.off = 0
        env = dict(os.environ)
        env["PSXPORT_NOWINDOW"] = "1"; env["PSXPORT_NOAUDIO"] = "1"; env["PSXPORT_NO_FMV"] = "1"
        if self.name == "port":
            env.setdefault("PSXPORT_REPL", "1")
            argv = [os.path.join(ROOT, "scratch/bin/tomba2_port"),
                    os.path.join(ROOT, "scratch/bin/tomba2/MAIN.EXE")]
        else:
            argv = [os.path.join(ROOT, "runtime/wide60rt"), disc(),
                    "-bios", os.path.join(ROOT, "scratch/bios"), "-repl"]
            if oraclestate: argv += ["-loadstate", oraclestate]
        fin = os.open(self.fin, os.O_RDWR)
        fout = open(self.fout, "wb")
        p = subprocess.Popen(argv, stdin=fin, stdout=fout, stderr=fout, env=env)
        open(self.pidf, "w").write(str(p.pid))
        print(f"[{self.name}] started pid={p.pid}")
        time.sleep(1.0); self._drain(2.0, 60)

    def _drain(self, settle=1.0, timeout=300.0):
        last, stable, t0 = -1, None, time.time()
        while True:
            sz = os.path.getsize(self.fout)
            if sz != last: last, stable = sz, time.time()
            elif stable and time.time() - stable >= settle: break
            if time.time() - t0 > timeout: break
            time.sleep(0.05)
        with open(self.fout, "rb") as f:
            f.seek(self.off); data = f.read()
        self.off += len(data)
        return data.decode("utf-8", "replace")

    def send(self, *cmds, settle=1.0, timeout=300.0):
        if not self.alive(): raise SystemExit(f"[{self.name}] not running")
        with open(self.fin, "w") as f:
            for c in cmds: f.write(c.rstrip("\n") + "\n")
            f.flush()
        return self._drain(settle, timeout)

    def read32(self, addr):
        """Read a 32-bit guest-RAM word (both cores expose `r addr n`, little-endian bytes)."""
        out = self.send("r %x 4" % addr, settle=0.4)
        m = re.search(r':\s*((?:[0-9A-Fa-f]{2}\s+){3}[0-9A-Fa-f]{2})', out)
        if not m: return None
        bts = m.group(1).split()[:4]
        return int("".join(reversed([b for b in bts])), 16)   # LE bytes -> word

    def stage(self):
        return self.read32(STAGE_ADDR)

PORT, ORACLE = Core("port"), Core("oracle")

def sync(addr, value, cap):
    """Advance each core until guest RAM[addr]==value (the game-state latch), independent of frame count."""
    for c in (PORT, ORACLE):
        chunk, total = 120, 0
        while total < cap:
            v = c.read32(addr)
            if v == value:
                print(f"[{c.name}] latch [{addr:08X}]=={value} after ~{total}f"); break
            c.send("run %d" % chunk, settle=0.4, timeout=300)
            total += chunk
        else:
            print(f"[{c.name}] WARNING: [{addr:08X}]={c.read32(addr)} != {value} after {cap}f")

def shot(name):
    pp = os.path.join(SHOTS, f"{name}_port.ppm"); oo = os.path.join(SHOTS, f"{name}_oracle.ppm")
    PORT.send(f"shot {pp}", settle=0.6); ORACLE.send(f"shot {oo}", settle=0.6)
    pn = os.path.join(SHOTS, f"{name}_port.png"); on = os.path.join(SHOTS, f"{name}_oracle.png")
    sbs = os.path.join(SHOTS, f"{name}_sbs.png"); df = os.path.join(SHOTS, f"{name}_diff.png")
    def mg(*a): subprocess.run(["magick", *a], check=False)
    mg(pp, pn); mg(oo, on)
    # normalize oracle to port size for a fair compare, side-by-side + difference heatmap
    mg(on, "-resize", "%dx%d!" % _size(pn), os.path.join(SHOTS, f"{name}_oracle_n.png"))
    onn = os.path.join(SHOTS, f"{name}_oracle_n.png")
    subprocess.run(["magick", "montage", pn, onn, "-tile", "2x1", "-geometry", "+4+4",
                    "-title", f"{name}: PORT (left) vs ORACLE (right)", sbs], check=False)
    subprocess.run(["magick", "compare", pn, onn, df], check=False)
    print(f"[dual] wrote {sbs} and {df}")

def _size(png):
    out = subprocess.run(["identify", "-format", "%w %h", png], capture_output=True, text=True).stdout.split()
    return (int(out[0]), int(out[1])) if len(out) == 2 else (320, 240)

def main():
    if len(sys.argv) < 2: print(__doc__); return 1
    cmd = sys.argv[1]
    if cmd == "start":
        os_state = next((a.split("=",1)[1] for a in sys.argv[2:] if a.startswith("oraclestate=")), None)
        PORT.start(); ORACLE.start(os_state)
    elif cmd == "stage":
        for c in (PORT, ORACLE):
            s, sc = c.stage(), c.read32(DEFAULT_LATCH[0])
            print(f"{c.name:7}: stage={s:08X} scene[{DEFAULT_LATCH[0]:08X}]={sc}" if s is not None else f"{c.name}: ?")
    elif cmd == "sync":
        latch = next((a for a in sys.argv[2:] if ":" in a and not a.startswith("cap=")), None)
        addr, value = (int(latch.split(":")[0], 16), int(latch.split(":")[1])) if latch else DEFAULT_LATCH
        cap = int(next((a.split("=",1)[1] for a in sys.argv[2:] if a.startswith("cap=")), "16000"))
        sync(addr, value, cap)
    elif cmd == "step":
        n = int(sys.argv[2]); PORT.send("run %d" % n, settle=0.5); ORACLE.send("run %d" % n, settle=0.5)
    elif cmd == "shot":
        shot(sys.argv[2] if len(sys.argv) > 2 else "dual")
    elif cmd == "send":
        c = PORT if sys.argv[2] == "port" else ORACLE
        print(c.send(*sys.argv[3:]))
    elif cmd == "stop":
        PORT.stop(); ORACLE.stop(); print("[dual] stopped")
    else:
        print(__doc__); return 1
    return 0

if __name__ == "__main__":
    sys.exit(main())
