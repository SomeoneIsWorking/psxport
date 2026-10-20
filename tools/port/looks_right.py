#!/usr/bin/env python3
"""Drive a built port and report whether it runs and looks right at 4:3, widescreen and 60 fps.

  reaches      the requested frames are presented with no executor fault or fatal trap
  widescreen   the widened picture differs from the 4:3 one (a no-op aspect knob fails)
  coverage     how much wider the drawn picture became (a rescale reads no-gain)
  fps60        the extra presents carry interpolated prims (tier1=N>0); a duplicate frame fails, and
               a run with no --replay/--route that sees no in-between is UNJUDGED

The PNGs are kept for a person to look at; nothing here decides that a picture looks right. A missing
binary or capture is REFUSED (exit 2).
"""

import argparse
import io
import os
import shlex
import subprocess
import sys
from pathlib import Path

from launch_environment import agent_environment

PASS, FAIL, REFUSED = 0, 1, 2

FPS60_SLOT_MARK = "slotA:"
FPS60_ON_MARK = "interpolated 60fps ON"
FPS60_REFUSED_MARK = "interpolated 60fps REFUSED"
FAILURE_MARKS = ("executor fault", "unsupported translation", "FATAL", "fatal trap", "watchdog STUCK", "VSync timeout")


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

    # Non-black bounding box; only compared between legs of the same scene.
    def drawn_extent(self):
        from PIL import Image

        image = Image.open(io.BytesIO(self.encoded)).convert("RGB")
        box = image.point(lambda v: 255 if v > 8 else 0).convert("L").getbbox()
        width, height = image.size
        if box is None:
            return None, width, height
        return box, width, height

    def drawn_aspect(self):
        box, _, _ = self.drawn_extent()
        if box is None:
            return None
        x0, y0, x1, y1 = box
        return None if y1 == y0 else (x1 - x0) / (y1 - y0)


def fps60_verdict(log_text):
    """(state, interpolated_prims, extra_presents) from a run's fps60 telemetry; tier1=0 is a duplicate."""
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


def presentation_fences(log_text):
    """How many presentation fences the run actually produced, or None if it never said."""
    for line in reversed(log_text.splitlines()):
        for token in line.split():
            if token.startswith("presentation_fences="):
                try:
                    return int(token.split("=", 1)[1])
                except ValueError:
                    return None
    return None


def fps60_unjudged(state, routed):
    """Whether no-extra-present means nothing: boot and attract can present with no geometry."""
    return state == "no-extra-present" and not routed


def coverage_measure(standard, wide):
    """(state, detail): how much wider the drawn picture became; whether it should is for a person."""
    flat, widened = standard.drawn_aspect(), wide.drawn_aspect()
    if flat is None or widened is None:
        return "blank", "a leg drew nothing at all"
    if widened <= flat * 1.02:
        return "no-gain", (f"drawn aspect {flat:.3f} -> {widened:.3f} — the same picture rescaled, no "
                           f"horizontal gain. Correct for a scene that draws black; a DEFECT for a "
                           f"full-screen page (Tomba! 2 issue 0010)")
    return "wider", f"drawn aspect {flat:.3f} -> {widened:.3f}"


def run_failures(log_text):
    return [mark for mark in FAILURE_MARKS if mark in log_text]


def reset_run_outputs(log, capture_paths):
    """Prevent an incomplete run from inheriting evidence from an earlier run."""
    Path(log).write_text("")
    for path in capture_paths:
        Path(path).unlink(missing_ok=True)


def route_command(template, shot, settings, log):
    """A title's own command for reaching the state worth judging, with this run's paths in it."""
    missing = [field for field in ("{shot}",) if field not in template]
    if missing:
        raise ValueError(f"--route must contain {' and '.join(missing)} so the capture has a path")
    return shlex.split(template.format(shot=shot, settings=settings, log=log))


def default_repository(binary):
    """Shipping port binaries live in <repo>/build/bin; a verifier build such as <repo>/build/ci/bin
    names its repository with --repository instead."""
    return Path(binary).resolve().parents[2]


def run_port(binary, scratch, name, frames, shot_frames, replay, aspect, fps60, extra_env, repository,
             route=None):
    """One hidden-window run rooted at the repository; returns (log_text, {frame: Capture}, exit_code).

    A leg that died still leaves shots and telemetry behind, so callers must check the exit code.
    """
    scratch = Path(scratch).resolve()
    settings = scratch / f"{name}.ini"
    settings.write_text(f"aspect={aspect}\n")
    log = scratch / f"{name}.log"
    shots = scratch / name
    shots.mkdir(parents=True, exist_ok=True)
    routed_shot = shots / "present.png"
    capture_paths = (
        {shot_frames[0]: routed_shot} if route
        else {frame: repository / "scratch" / "screenshots" / f"present_{frame}.png"
              for frame in shot_frames}
    )
    reset_run_outputs(log, capture_paths.values())
    env = agent_environment({**os.environ, **extra_env}, settings)
    if not route:
        # A route owns its log and frame cap; a log file here would swallow its REPL banner.
        env["PSXPORT_LOG_FILE"] = str(log)
        env["PSXPORT_NATIVE_FRAMES"] = str(frames)
        env["PSXPORT_PRESENT_SHOT_AT"] = ",".join(str(f) for f in shot_frames)
    if replay and not route:
        env["PSXPORT_PAD_REPLAY"] = str(replay)
    if fps60:
        env["PSXPORT_FPS60"] = "1"
        # The slot lines are on the fps60 debug channel.
        channels = env.get("PSXPORT_DEBUG", "")
        env["PSXPORT_DEBUG"] = f"{channels},fps60" if channels else "fps60"
    command = (route_command(route, routed_shot, settings, log) if route else [str(binary)])
    # A file, not a pipe: an undrained pipe stalls the port once full.
    console = scratch / f"{name}.out"
    with console.open("wb") as sink:
        completed = subprocess.run(command, env=env, cwd=repository, stdout=sink,
                                   stderr=subprocess.STDOUT, check=False)
    if completed.returncode != 0:
        print(f"[looks-right] {name}: command exited {completed.returncode} — see {console}")
    text = log.read_text(errors="replace") if log.exists() else ""
    captured = {}
    for frame, candidate in capture_paths.items():
        if not candidate.exists():
            continue
        target = shots / f"present_{frame}.png"
        if candidate != target:
            target.write_bytes(candidate.read_bytes())
            candidate.unlink()
        captured[frame] = Capture.read(target)
    return text, captured, completed.returncode


def crashed(exit_code):
    """Whether a leg died before finishing."""
    return exit_code != 0


def crash_detail(name, exit_code):
    return f"the {name} run exited {exit_code} — it did not finish, so this check cannot pass"


def report(label, ok, detail):
    print(f"[looks-right] {label:<12} {'PASS' if ok else 'FAIL'} — {detail}")
    return ok


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--binary", help="the already-built port executable (agents never run run.sh)")
    parser.add_argument("--repository", help="the port repository the run is rooted at (default: two "
                                             "directories above the binary's bin/, i.e. <repo>/build/bin)")
    parser.add_argument("--frames", type=int, default=400, help="presentation-fence steps to run (PSXPORT_NATIVE_FRAMES)")
    parser.add_argument("--shot-at", default="", help="comma-separated frames to capture (default: the last frame)")
    parser.add_argument("--replay", help="pad replay that reaches gameplay; without one this only sees attract")
    parser.add_argument("--route", help="the title's own command for reaching the state worth judging, "
                                        "with {shot} (required), {settings} and {log} placeholders. It "
                                        "replaces --replay, which describes wherever the game happened "
                                        "to be when the pad was recorded")
    parser.add_argument("--out", default="scratch/looks-right", help="where captures, PNGs and logs land")
    parser.add_argument("--env", action="append", default=[], metavar="K=V", help="extra environment, repeatable")
    parser.add_argument("--skip-fps60", action="store_true", help="title declares no interpolation product")
    args = parser.parse_args(argv)

    if not args.binary:
        print("[looks-right] REFUSED: --binary names the built port; this run asserted NOTHING")
        return REFUSED
    binary = Path(args.binary)
    if not binary.is_file():
        print(f"[looks-right] REFUSED: {binary} is not a built binary; this run asserted NOTHING")
        return REFUSED

    repository = Path(args.repository).resolve() if args.repository else default_repository(binary)
    if not (repository / "scratch").is_dir() and not (repository / "run.sh").is_file():
        print(f"[looks-right] REFUSED: {repository} does not look like a port repository (no run.sh or "
              f"scratch/); pass --repository")
        return REFUSED
    shot_frames = [int(f) for f in args.shot_at.split(",") if f.strip()] or [args.frames - 1]
    extra_env = dict(pair.split("=", 1) for pair in args.env)
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    if args.route:
        try:
            route_command(args.route, "shot.png", "settings.ini", "run.log")
        except ValueError as bad:
            print(f"[looks-right] REFUSED: {bad}; this run asserted NOTHING")
            return REFUSED
        print(f"[looks-right] {binary} — observed route: {args.route}")
    else:
        print(f"[looks-right] {binary} — {args.frames} frame(s), shots at {shot_frames}, "
              f"replay {args.replay or 'none'}")
    standard_log, standard, standard_exit = run_port(binary, out, "aspect-4x3", args.frames, shot_frames, args.replay, 0, False, extra_env, repository, args.route)
    if not standard:
        where = "the route" if args.route else "the 4:3 run"
        print(f"[looks-right] REFUSED: no capture from {where} — see {out}/aspect-4x3.log")
        return REFUSED

    ok = True
    failures = run_failures(standard_log)
    # A short capture fails even with no failure mark in the log.
    missing = [f for f in shot_frames if f not in standard]
    detail = f"{len(standard)}/{len(shot_frames)} shot(s) captured, failure marks: {failures or 'none'}"
    if missing:
        fences = presentation_fences(standard_log)
        detail += f" — never reached frame(s) {missing}"
        if fences is not None:
            detail += f"; the run ended after {fences} of {args.frames} presentation fence(s)"
    if crashed(standard_exit):
        detail += f" — but {crash_detail('4:3', standard_exit)}"
    ok &= report("reaches", not failures and not missing and not crashed(standard_exit), detail)

    wide_log, wide, wide_exit = run_port(binary, out, "aspect-16x9", args.frames, shot_frames, args.replay, 1, False, extra_env, repository, args.route)
    frame = shot_frames[0]
    if crashed(wide_exit):
        ok &= report("widescreen", False, crash_detail("16:9", wide_exit))
    elif frame in wide and frame in standard:
        changed = wide[frame].differs_from(standard[frame])
        ok &= report(
            "widescreen",
            changed,
            f"f{frame} PNG differs from 4:3" + ("" if changed else " — the aspect knob did NOTHING"),
        )
        state, detail = coverage_measure(standard[frame], wide[frame])
        print(f"[looks-right] coverage     {'WIDER' if state == 'wider' else 'NO GAIN'} — {detail}")
    else:
        ok &= report("widescreen", False, "the 16:9 run produced no capture to compare")

    if args.skip_fps60:
        print("[looks-right] fps60        SKIPPED — caller declares no interpolation product for this title")
    else:
        fps_log, _, fps_exit = run_port(binary, out, "fps60", args.frames, shot_frames, args.replay, 0, True, extra_env, repository, args.route)
        state, interpolated, extras = fps60_verdict(fps_log)
        unjudged = fps60_unjudged(state, routed=bool(args.replay or args.route))
        detail = {
            "interpolating": f"{interpolated} interpolated prim(s) over {extras} extra present(s)",
            "duplicate-frame": f"{extras} extra present(s), ALL with tier1=0 — the in-between frame is a DUPLICATE",
            "no-extra-present": ("enabled, but this run had no --replay and no --route, so it saw only boot/attract, "
                                 "where there may be no interpolatable geometry — supply one to judge this"
                                 if unjudged else
                                 "enabled over a gameplay route and no extra present was ever emitted"),
            "not-enabled": "PSXPORT_FPS60=1 was set but the run never reported interpolation on",
            "refused": "the title declares no temporal interpolation product",
        }[state]
        if crashed(fps_exit):
            ok &= report("fps60", False, f"{detail}, but {crash_detail('fps60', fps_exit)}")
        elif unjudged:
            print(f"[looks-right] fps60        UNJUDGED — {detail}")
            ok = False
        else:
            ok &= report("fps60", state == "interpolating", detail)

    print(f"[looks-right] PNGs for a human to judge: {out}/*/present_*.png")
    print("[looks-right] LOOKING AT THEM IS THE REST OF THE CHECK — this tool cannot tell you it looks right.")
    return PASS if ok else FAIL


if __name__ == "__main__":
    sys.exit(main())
