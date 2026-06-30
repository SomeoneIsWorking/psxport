#!/usr/bin/env python3
"""ensure_recomp.py — the single, hash-checked recompilation step.

ONE entry point that guarantees the statically-recompiled substrate in generated/ is
PRESENT and matches a deterministic hash of its INPUTS. run.sh calls only this; all recomp
provisioning lives here, not scattered through the shell script.

What it does, in order:
  1. Resolve the disc image (CLI arg > $PSXPORT_TOMBA2_DISC > .env > *.chd drop-in — mirrors run.sh).
  2. Extract from the disc (via build/tools/discdump): MAIN.EXE, the boot stub SCUS_944.54, and
     EVERY overlay the recompiler needs — the stage/mode overlays (START/DEMO/GAME/SOP/OPN/CRD)
     AND the per-area field-code overlays (A00..A0L). The area overlays MUST all be present: emit.py
     recompiles each AND its overlay-scan seeds the resident MAIN functions they jal into, so a box
     missing them recompiles fewer MAIN fns and fail-fasts on a DIFFERENT miss than a complete box.
  3. Hash the INPUTS (MAIN.EXE + stub + each overlay .BIN + the recompiler module sources). If the
     stored hash (generated/.recomp.hash) matches AND the generated set is complete, do nothing.
     Otherwise re-run emit.py and rewrite the hash. Deterministic: "ensure present AND matches",
     no silent drift — every machine builds byte-identical recomp from the same disc.

Usage: python3 tools/ensure_recomp.py [/path/to/disc.chd]
Env:   PSXPORT_TOMBA2_DISC (disc path), PSXPORT_DISCDUMP (discdump binary override),
       PSXPORT_FORCE_RECOMP=1 (ignore the hash and always re-emit).
Exit:  0 on success (recomp present & current), non-zero with a diagnostic on any failure.
"""
import hashlib
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Overlays the recompiler needs extracted: stage/mode overlays + every per-area field overlay.
# Keep this in lockstep with the OVERLAY_BASES / per-area routing in tools/recomp/emit.py.
STAGE_OVERLAYS = ["START", "DEMO", "GAME", "SOP", "OPN", "CRD"]
AREA_OVERLAYS = ["A0" + c for c in "0123456789ABCDEFGHIJKL"]
ALL_OVERLAYS = STAGE_OVERLAYS + AREA_OVERLAYS

# Recompiler module sources — a change to any of these changes the emitted C, so they are hash inputs.
RECOMP_SRCS = ["tools/recomp/emit.py", "tools/recomp/decode.py", "tools/recomp/psexe.py"]

MAIN = "scratch/bin/tomba2/MAIN.EXE"
STUB = "scratch/bin/tomba2/SCUS_944.54"
OVL_DIR = "scratch/bin/overlays"
GEN_DIR = "generated"
GEN_MAIN = "generated/tomba2_rec.c"
HASH_FILE = "generated/.recomp.hash"


def say(msg):
    sys.stderr.write(f"\033[1;36m[ensure-recomp]\033[0m {msg}\n")


def die(msg):
    sys.stderr.write(f"\033[1;31m[ensure-recomp] error:\033[0m {msg}\n")
    sys.exit(1)


def resolve_disc(argv):
    """CLI arg > $PSXPORT_TOMBA2_DISC > .env (PSXPORT_TOMBA2_DISC|PSXPORT_DISC) > *.chd drop-in."""
    disc = argv[1] if len(argv) > 1 and argv[1] else os.environ.get("PSXPORT_TOMBA2_DISC", "")
    if not disc and os.path.isfile(os.path.join(ROOT, ".env")):
        env = open(os.path.join(ROOT, ".env")).read()
        for key in ("PSXPORT_TOMBA2_DISC", "PSXPORT_DISC"):
            m = re.search(rf"^\s*{key}\s*=\s*(.+?)\s*$", env, re.M)
            if m:
                disc = m.group(1)
                break
    if not disc:
        chds = sorted(p for p in os.listdir(ROOT) if p.lower().endswith(".chd"))
        if chds:
            disc = os.path.join(ROOT, chds[0])
    if not disc or not os.path.isfile(disc):
        die("no disc image — pass it as ./run.sh <disc.chd>, set PSXPORT_TOMBA2_DISC, or drop a *.chd here")
    return disc


def find_discdump():
    cand = os.environ.get("PSXPORT_DISCDUMP", "")
    if cand and os.access(cand, os.X_OK):
        return cand
    for p in ("build/tools/discdump", "build/tools/discdump.exe"):
        full = os.path.join(ROOT, p)
        if os.access(full, os.X_OK):
            return full
    die("discdump not built — run.sh builds it before calling ensure_recomp.py "
        "(cmake --build build --target discdump)")


def extract(discdump, disc, disc_path, dest_dir, optional=False):
    """Pull one file off the disc into dest_dir, if not already present. Returns the local path
    (or None when an optional file is absent on the disc)."""
    out = os.path.join(ROOT, dest_dir, os.path.basename(disc_path))
    if os.path.isfile(out):
        return out
    os.makedirs(os.path.join(ROOT, dest_dir), exist_ok=True)
    r = subprocess.run([discdump, "get", disc_path, disc, os.path.join(ROOT, dest_dir)],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if r.returncode != 0 or not os.path.isfile(out):
        if optional:
            return None
        die(f"could not extract {disc_path} from the disc")
    return out


def input_hash(overlay_files):
    """SHA-256 over MAIN.EXE + stub + each overlay .BIN + the recompiler sources. The overlay
    NAMES are folded in too, so adding/removing an overlay changes the hash even if bytes collide."""
    h = hashlib.sha256()

    def feed(label, path):
        h.update(label.encode())
        with open(path, "rb") as f:
            h.update(f.read())

    feed("MAIN.EXE", os.path.join(ROOT, MAIN))
    feed("SCUS_944.54", os.path.join(ROOT, STUB))
    for name in sorted(overlay_files):
        feed("OVL:" + name, os.path.join(ROOT, OVL_DIR, name))
    for src in RECOMP_SRCS:
        feed(src, os.path.join(ROOT, src))
    return h.hexdigest()


def generated_complete():
    """The generated set is complete iff the manifest exists and every TU it lists is present."""
    manifest = os.path.join(ROOT, GEN_DIR, "rec_sources.cmake")
    for f in (manifest, os.path.join(ROOT, GEN_DIR, "shard_disp.c"),
              os.path.join(ROOT, GEN_MAIN), os.path.join(ROOT, GEN_DIR, "overlay_table.c")):
        if not os.path.isfile(f):
            return False
    listed = re.findall(r"^\s*(\S+\.c)\s*$", open(manifest).read(), re.M)
    return all(os.path.isfile(os.path.join(ROOT, GEN_DIR, tu)) for tu in listed)


def run_emit():
    say("recompiling MAIN.EXE + overlays -> C (the execution substrate)…")
    cmd = [sys.executable, os.path.join(ROOT, "tools/recomp/emit.py"),
           os.path.join(ROOT, MAIN), os.path.join(ROOT, GEN_MAIN),
           "--overlays", os.path.join(ROOT, OVL_DIR),
           "--stub", os.path.join(ROOT, STUB)]
    r = subprocess.run(cmd)
    if r.returncode != 0:
        die("emit.py failed")


def main():
    disc = resolve_disc(sys.argv)
    say(f"disc: {disc}")
    discdump = find_discdump()

    # 1. Extract the recompiler inputs (idempotent — only fetches what's missing).
    extract(discdump, disc, "MAIN.EXE", "scratch/bin/tomba2")
    extract(discdump, disc, "SCUS_944.54", "scratch/bin/tomba2")
    overlay_files = []
    for stem in ALL_OVERLAYS:
        p = extract(discdump, disc, f"BIN/{stem}.BIN", OVL_DIR, optional=True)
        if p:
            overlay_files.append(os.path.basename(p))
        else:
            say(f"WARNING: overlay BIN/{stem}.BIN not on the disc — skipping")

    # 2. Hash the inputs; skip the emit if everything is present and current.
    os.makedirs(os.path.join(ROOT, GEN_DIR), exist_ok=True)
    want = input_hash(overlay_files)
    have = ""
    if os.path.isfile(os.path.join(ROOT, HASH_FILE)):
        have = open(os.path.join(ROOT, HASH_FILE)).read().strip()
    force = os.environ.get("PSXPORT_FORCE_RECOMP", "") not in ("", "0")
    if not force and have == want and generated_complete():
        say(f"recomp up to date (hash {want[:12]}…) — nothing to do")
        return

    if force:
        say("PSXPORT_FORCE_RECOMP set — re-emitting")
    elif have and have != want:
        say(f"inputs changed (hash {have[:12]}… -> {want[:12]}…) — re-emitting")
    elif not have:
        say("no recorded hash — emitting")
    else:
        say("generated set incomplete — re-emitting")

    run_emit()
    if not generated_complete():
        die("emit.py ran but the generated set is still incomplete")
    open(os.path.join(ROOT, HASH_FILE), "w").write(want + "\n")
    say(f"recomp current (hash {want[:12]}…)")


if __name__ == "__main__":
    main()
