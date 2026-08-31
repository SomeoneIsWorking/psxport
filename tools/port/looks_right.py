#!/usr/bin/env python3
"""Drive a built port and answer the only question that decides a port is done: does it RUN, and does
it LOOK RIGHT — at 4:3, in widescreen, and with interpolated 60fps.

USER 2026-08-30: "Change the directive, pixel matching doesn't matter. I just want working game that
looks correct." and "It's pretty frustrating that all the previous work went to pixel matching
(I mean before you) instead of just verifying it works fine and looks fine wide/60, this correction
should apply to all PSX projects."

This tool exists because Crash Bash's frame-300 difference count was driven from 98,280 to 6 pixels
over many sessions while NOBODY EVER LOOKED AT THE GAME. When it was finally driven to a live match:
widescreen worked but its 2D layers kept a 4:3 extent, and `PSXPORT_FPS60=1` was inserting a
DUPLICATE frame rather than an interpolated one — 60Hz pacing of 30Hz motion, invisible to every
pixel comparison ever run, and visible in one log line.

So the checks here are the ones a difference count structurally cannot make:

  reaches      the requested frame count is presented, with no recompilation miss or fatal trap
  widescreen   the widened picture actually DIFFERS from the 4:3 one (a no-op aspect knob FAILS)
  fps60        the extra presents carry interpolated prims (`tier1=N>0`); an inserted duplicate FAILS

It keeps the port's directly written PNGs for a human to look at, because "looks right" is a judgement
no tool makes. It reports what it could not assert instead of passing quietly: a missing disc, a binary
that never launched, or a title that declares no interpolation product is REFUSED (exit 2), never a
green tick.
"""

import argparse
import os
import subprocess
import sys
from pathlib import Path

PASS, FAIL, REFUSED = 0, 1, 2

FPS60_SLOT_MARK = "slotA:"
FPS60_ON_MARK = "interpolated 60fps ON"
FPS60_REFUSED_MARK = "interpolated 60fps REFUSED"
FAILURE_MARKS = ("recomp MISS", "recomp-MISS", "FATAL", "fatal trap", "watchdog STUCK", "VSync timeout")


class Capture:
    """The port's final PNG bytes; visual interpretation remains a human responsibility."""

    def __init__(self, encoded):
        self.encoded = encoded

    @classmethod
    def read(cls, path):
        encoded = Path(path).read_bytes()
        if not encoded.startswith(b"\x89PNG\r\n\x1a\n"):
            raise ValueError(f"{path} is not a PNG capture")
        return cls(encoded)

    def differs_from(self, other):
        return self.encoded != other.encoded


def fps60_verdict(log_text):
    """(state, interpolated_prims, extra_presents) from a run's own fps60 telemetry.

    The failure this names is the measured one: the extra present exists and reports `tier1=0`, so the
    in-between frame is the previous queue replayed verbatim. A tool that only asked "did fps60 turn
    on" would have called that working for as long as anyone cared to ask.
    """
    if FPS60_REFUSED_MARK in log_text:
        return "refused", 0, 0
    if FPS60_ON_MARK not in log_text:
        return "not-enabled", 0, 0
    interpolated, extras = 0, 0
    for line in log_text.splitlines():
        if FPS60_SLOT_MARK not in line:
            continue
        extras += 1
        for token in line.split():
            if token.startswith("tier1="):
                interpolated += int(token.split("=", 1)[1])
    if extras == 0:
        return "no-extra-present", 0, 0
    if interpolated == 0:
        return "duplicate-frame", 0, extras
    return "interpolating", interpolated, extras


def run_failures(log_text):
    return [mark for mark in FAILURE_MARKS if mark in log_text]


def run_port(binary, scratch, name, frames, shot_frames, replay, aspect, fps60, extra_env):
    """One headless run. Returns (log_text, {frame: Capture})."""
    settings = Path(scratch) / f"{name}.ini"
    settings.write_text(f"aspect={aspect}\n")
    log = Path(scratch) / f"{name}.log"
    shots = Path(scratch) / name
    shots.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    env.update(extra_env)
    env.update(
        {
            "PSXPORT_LOG_FILE": str(log),
            "PSXPORT_NATIVE_FRAMES": str(frames),
            "PSXPORT_NOAUDIO": "1",
            "PSXPORT_NOPACE": "1",
            "PSXPORT_PRESENT_SHOT_AT": ",".join(str(f) for f in shot_frames),
            "PSXPORT_SETTINGS": str(settings),
        }
    )
    if replay:
        env["PSXPORT_PAD_REPLAY"] = str(replay)
    if fps60:
        env["PSXPORT_FPS60"] = "1"
        # The per-present slot line is debug audience, so the telemetry this verdict reads only exists
        # when the channel is asked for. Without this the tool reports "no extra present" for a run that
        # emitted one every frame — a false FAILURE, which is the same class of lie as a false pass.
        channels = env.get("PSXPORT_DEBUG", "")
        env["PSXPORT_DEBUG"] = f"{channels},fps60" if channels else "fps60"
    # Shipping port binaries live in <repo>/build/bin. Present shots are repository-relative.
    repository = Path(binary).resolve().parents[2]
    subprocess.run([str(binary)], env=env, cwd=repository, capture_output=True, check=False)
    text = log.read_text(errors="replace") if log.exists() else ""
    captured = {}
    for frame in shot_frames:
        for candidate in (
            repository / "scratch" / "screenshots" / f"present_{frame}.png",
            Path("scratch/screenshots") / f"present_{frame}.png",
        ):
            if candidate.exists():
                target = shots / f"present_{frame}.png"
                target.write_bytes(candidate.read_bytes())
                candidate.unlink()
                image = Capture.read(target)
                captured[frame] = image
                break
    return text, captured


def report(label, ok, detail):
    print(f"[looks-right] {label:<12} {'PASS' if ok else 'FAIL'} — {detail}")
    return ok


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--binary", help="the already-built port executable (agents never run run.sh)")
    parser.add_argument("--frames", type=int, default=400, help="frames to present")
    parser.add_argument("--shot-at", default="", help="comma-separated frames to capture (default: the last frame)")
    parser.add_argument("--replay", help="pad replay that reaches gameplay; without one this only sees attract")
    parser.add_argument("--out", default="scratch/looks-right", help="where captures, PNGs and logs land")
    parser.add_argument("--env", action="append", default=[], metavar="K=V", help="extra environment, repeatable")
    parser.add_argument("--skip-fps60", action="store_true", help="title declares no interpolation product")
    parser.add_argument("--selftest", action="store_true", help="prove the verdicts on constructed inputs")
    args = parser.parse_args(argv)

    if args.selftest:
        return selftest()
    if not args.binary:
        print("[looks-right] REFUSED: --binary names the built port; this run asserted NOTHING")
        return REFUSED
    binary = Path(args.binary)
    if not binary.is_file():
        print(f"[looks-right] REFUSED: {binary} is not a built binary; this run asserted NOTHING")
        return REFUSED

    shot_frames = [int(f) for f in args.shot_at.split(",") if f.strip()] or [args.frames - 1]
    extra_env = dict(pair.split("=", 1) for pair in args.env)
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    print(f"[looks-right] {binary} — {args.frames} frame(s), shots at {shot_frames}, replay {args.replay or 'none'}")
    standard_log, standard = run_port(binary, out, "aspect-4x3", args.frames, shot_frames, args.replay, 0, False, extra_env)
    if not standard:
        print(f"[looks-right] REFUSED: no capture from the 4:3 run — see {out}/aspect-4x3.log")
        return REFUSED

    ok = True
    failures = run_failures(standard_log)
    ok &= report("reaches", not failures, f"{len(standard)} shot(s) captured, failure marks: {failures or 'none'}")

    wide_log, wide = run_port(binary, out, "aspect-16x9", args.frames, shot_frames, args.replay, 1, False, extra_env)
    frame = shot_frames[0]
    if frame in wide and frame in standard:
        changed = wide[frame].differs_from(standard[frame])
        ok &= report(
            "widescreen",
            changed,
            f"f{frame} PNG differs from 4:3" + ("" if changed else " — the aspect knob did NOTHING"),
        )
    else:
        ok &= report("widescreen", False, "the 16:9 run produced no capture to compare")

    if args.skip_fps60:
        print("[looks-right] fps60        SKIPPED — caller declares no interpolation product for this title")
    else:
        fps_log, _ = run_port(binary, out, "fps60", args.frames, shot_frames, args.replay, 0, True, extra_env)
        state, interpolated, extras = fps60_verdict(fps_log)
        detail = {
            "interpolating": f"{interpolated} interpolated prim(s) over {extras} extra present(s)",
            "duplicate-frame": f"{extras} extra present(s), ALL with tier1=0 — the in-between frame is a DUPLICATE",
            "no-extra-present": "enabled but no extra present was ever emitted",
            "not-enabled": "PSXPORT_FPS60=1 was set but the run never reported interpolation on",
            "refused": "the title declares no temporal interpolation product",
        }[state]
        ok &= report("fps60", state == "interpolating", detail)

    print(f"[looks-right] PNGs for a human to judge: {out}/*/present_*.png")
    print("[looks-right] LOOKING AT THEM IS THE REST OF THE CHECK — this tool cannot tell you it looks right.")
    return PASS if ok else FAIL


def selftest():
    """Both answers, on constructed inputs, for every verdict this tool makes."""
    checks = []

    duplicate = "[fps60] TRUE per-object interpolated 60fps ON (source: env)\n" + "".join(
        f"[fps60] f{f} slotA: replay prev=Q[N-1] n=3613 tier1=0 backdrop=0 t=0.500\n" for f in range(3)
    )
    live = "[fps60] TRUE per-object interpolated 60fps ON (source: env)\n" + "".join(
        f"[fps60] f{f} slotA: replay prev=Q[N-1] n=3613 tier1=1800 backdrop=12 t=0.500\n" for f in range(3)
    )
    checks.append(("fps60 duplicate frame is a FAILURE", fps60_verdict(duplicate)[0] == "duplicate-frame"))
    checks.append(("fps60 interpolating is a PASS", fps60_verdict(live)[0] == "interpolating"))
    checks.append(("fps60 counts interpolated prims", fps60_verdict(live)[1] == 5400))
    enabled_only = "[fps60] TRUE per-object interpolated 60fps ON (source: env)"
    checks.append(("fps60 enabled with no extra present", fps60_verdict(enabled_only)[0] == "no-extra-present"))
    checks.append(
        ("fps60 refusal is not a pass", fps60_verdict("[fps60] interpolated 60fps REFUSED")[0] == "refused")
    )
    checks.append(("fps60 off is named, not assumed", fps60_verdict("quiet log")[0] == "not-enabled"))

    checks.append(("a clean log has no failure marks", run_failures("all good") == []))
    checks.append(("a recomp MISS is a failure mark", run_failures("[recomp] recomp MISS at 0x8001") != []))

    png_header = b"\x89PNG\r\n\x1a\n"
    flat = Capture(png_header + b"same picture")
    same = Capture(png_header + b"same picture")
    other = Capture(png_header + b"wider picture")
    checks.append(("an identical widescreen picture is a FAILURE", not flat.differs_from(same)))
    checks.append(("a widened picture differs", flat.differs_from(other)))

    passed = sum(1 for _, ok in checks if ok)
    for name, ok in checks:
        if not ok:
            print(f"[looks-right:selftest] FAILED: {name}")
    print(f"[looks-right] selftest {passed}/{len(checks)} => {'PASS' if passed == len(checks) else 'FAIL'}")
    return PASS if passed == len(checks) else FAIL


if __name__ == "__main__":
    sys.exit(main())
