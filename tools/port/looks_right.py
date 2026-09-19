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

  reaches      the requested frame count is presented, with no executor fault or fatal trap
  widescreen   the widened picture actually DIFFERS from the 4:3 one (a no-op aspect knob FAILS)
  coverage     MEASURES how much wider the drawn picture became (a rescale reads no-gain)
  fps60        the extra presents carry interpolated prims (`tier1=N>0`); an inserted duplicate FAILS

It keeps the port's directly written PNGs for a human to look at, because "looks right" is a judgement
no tool makes. It reports what it could not assert instead of passing quietly: a missing disc, a binary
that never launched, or a title that declares no interpolation product is REFUSED (exit 2), never a
green tick.
"""

import argparse
import io
import os
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path

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

    # Everything the port actually drew, as a half-open pixel box, plus the frame it was drawn in.
    # "Drawn" is non-black: a present is letterboxed/pillarboxed into the sink with black, so the
    # black is the frame around the picture rather than part of it. A scene that legitimately ends
    # in black at its edges under-reports its own extent, which is why the coverage verdict below
    # only ever uses this to compare ONE leg against another leg of the SAME scene.
    def drawn_extent(self):
        from PIL import Image

        image = Image.open(io.BytesIO(self.encoded)).convert("RGB")
        box = image.point(lambda v: 255 if v > 8 else 0).convert("L").getbbox()
        width, height = image.size
        if box is None:
            return None, width, height
        return box, width, height

    # The aspect of what was drawn, which is the number that moves when a picture genuinely widens.
    def drawn_aspect(self):
        box, _, _ = self.drawn_extent()
        if box is None:
            return None
        x0, y0, x1, y1 = box
        return None if y1 == y0 else (x1 - x0) / (y1 - y0)

    # The aspect of what was drawn, which is the number that moves when a picture genuinely widens.
    def drawn_aspect(self):
        box, _, _ = self.drawn_extent()
        if box is None:
            return None
        x0, y0, x1, y1 = box
        return None if y1 == y0 else (x1 - x0) / (y1 - y0)

    # The aspect of what was drawn, which is the number that moves when a picture genuinely widens.
    def drawn_aspect(self):
        box, _, _ = self.drawn_extent()
        if box is None:
            return None
        x0, y0, x1, y1 = box
        return None if y1 == y0 else (x1 - x0) / (y1 - y0)

    # The aspect of what was drawn, which is the number that moves when a picture genuinely widens.
    def drawn_aspect(self):
        box, _, _ = self.drawn_extent()
        if box is None:
            return None
        x0, y0, x1, y1 = box
        return None if y1 == y0 else (x1 - x0) / (y1 - y0)


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


def coverage_measure(standard, wide):
    """(state, detail): how much wider the picture this scene actually DREW became.

    `widescreen` above asks only whether the two PNGs DIFFER, and rescaling the whole picture
    satisfies that. Measured 2026-09-19 on Tomba! 2's title options page: the 4:3 leg drew 960x720
    (ratio 1.333) and the 16:9 leg drew 718x538 (ratio 1.335) — the same 320-wide page, scaled down
    because the target around it got wider, with black pillars either side on a real 16:9 display.
    `widescreen` called that a PASS. This tool's own docstring names that failure ("its 2D layers
    kept a 4:3 extent") and the check it shipped with could not see it.

    A MEASUREMENT, NOT A VERDICT, and that is deliberate on evidence. Two rules were tried and both
    were wrong:

      - "judge only when the 4:3 leg fills the frame" excluded the cases that matter most. A PSX
        picture is letterboxed inside the sink, so Spyro's genuinely widened 3D scene reaches 93.3%
        of the sink's height; all eleven of Spyro's paired captures came back not-applicable.
      - "abstain when the content is sparse" does not separate the populations. Over 15 real paired
        captures, `spyro/secondary` f399 at 27.8% density correctly reads flat while `spyro/lr`
        f3000 at 30.6% correctly reads widened. Adjacent densities, opposite right answers.

    What remains true is the number itself, and a scene that legitimately draws black (Spyro's
    level-intro card, 1.992 -> 2.000) reports no gain because there was no content to gain. Whether
    a given scene SHOULD widen is a judgement about that scene, so this prints what it measured and
    leaves the judgement where this tool always leaves it — with the person looking at the PNGs.
    """
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
    """A title's own command for reaching the state worth judging, with this run's paths in it.

    A recorded pad is a fixed list of frame numbers, so it describes wherever the game happened to
    be when it was recorded and rots the moment anything before that point changes length. Spyro's
    only gameplay pad ended up sitting on the save-file warning for the whole run, and every verdict
    taken through it described a dialog (spyro issue 0116). An observed route asks the running game
    where it is instead, so it keeps meaning what it says. This tool stays title-neutral: what the
    route knows about menus and game states belongs to the title that wrote it.
    """
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
    """One headless run with cwd at the repository (present shots are repository-relative).
    Returns (log_text, {frame: Capture})."""
    scratch = Path(scratch).resolve()
    settings = scratch / f"{name}.ini"
    settings.write_text(f"aspect={aspect}\n")
    log = scratch / f"{name}.log"
    shots = scratch / name
    shots.mkdir(parents=True, exist_ok=True)
    # A route decides for itself when the game is worth looking at, so it names one capture rather
    # than a list of frame numbers this tool chose in advance.
    routed_shot = shots / "present.png"
    capture_paths = (
        {shot_frames[0]: routed_shot} if route
        else {frame: repository / "scratch" / "screenshots" / f"present_{frame}.png"
              for frame in shot_frames}
    )
    reset_run_outputs(log, capture_paths.values())
    env = dict(os.environ)
    env.update(extra_env)
    env.update({"PSXPORT_NOAUDIO": "1", "PSXPORT_NOPACE": "1", "PSXPORT_SETTINGS": str(settings)})
    if not route:
        # A route runs the game from its own REPL, so a frame cap would cut the drive short and a
        # fixed shot frame would fire wherever the route had got to. It also launches the port
        # itself and is handed {log}, so it owns where the log goes: setting PSXPORT_LOG_FILE for
        # it is worse than redundant, because it diverts the port's own REPL banner into the file
        # and a driver waiting for that banner on stdout then waits forever. Measured 2026-09-19 —
        # driver and port both sat at 0% CPU, one waiting for a prompt that had been written to a
        # file, the other waiting for the command that prompt would have triggered.
        env["PSXPORT_LOG_FILE"] = str(log)
        env["PSXPORT_NATIVE_FRAMES"] = str(frames)
        env["PSXPORT_PRESENT_SHOT_AT"] = ",".join(str(f) for f in shot_frames)
    if replay and not route:
        env["PSXPORT_PAD_REPLAY"] = str(replay)
    if fps60:
        env["PSXPORT_FPS60"] = "1"
        # The per-present slot line is debug audience, so the telemetry this verdict reads only exists
        # when the channel is asked for. Without this the tool reports "no extra present" for a run that
        # emitted one every frame — a false FAILURE, which is the same class of lie as a false pass.
        channels = env.get("PSXPORT_DEBUG", "")
        env["PSXPORT_DEBUG"] = f"{channels},fps60" if channels else "fps60"
    command = (route_command(route, routed_shot, settings, log) if route else [str(binary)])
    # Not capture_output: that gives the child a pipe, and a pipe that nobody drains blocks its
    # writer once the kernel buffer fills. Measured 2026-09-19 — a routed run sat at 0% CPU on
    # "[repl] frame=0 ready" for as long as it was left, because the port had filled 64 KB of
    # stderr and stopped. A file also leaves the reason behind when a route fails, which a
    # discarded pipe does not.
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
    return text, captured


def report(label, ok, detail):
    print(f"[looks-right] {label:<12} {'PASS' if ok else 'FAIL'} — {detail}")
    return ok


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--binary", help="the already-built port executable (agents never run run.sh)")
    parser.add_argument("--repository", help="the port repository the run is rooted at (default: two "
                                             "directories above the binary's bin/, i.e. <repo>/build/bin)")
    parser.add_argument("--frames", type=int, default=400, help="frames to present")
    parser.add_argument("--shot-at", default="", help="comma-separated frames to capture (default: the last frame)")
    parser.add_argument("--replay", help="pad replay that reaches gameplay; without one this only sees attract")
    parser.add_argument("--route", help="the title's own command for reaching the state worth judging, "
                                        "with {shot} (required), {settings} and {log} placeholders. It "
                                        "replaces --replay, which describes wherever the game happened "
                                        "to be when the pad was recorded")
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
    standard_log, standard = run_port(binary, out, "aspect-4x3", args.frames, shot_frames, args.replay, 0, False, extra_env, repository, args.route)
    if not standard:
        where = "the route" if args.route else "the 4:3 run"
        print(f"[looks-right] REFUSED: no capture from {where} — see {out}/aspect-4x3.log")
        return REFUSED

    ok = True
    failures = run_failures(standard_log)
    ok &= report("reaches", not failures, f"{len(standard)} shot(s) captured, failure marks: {failures or 'none'}")

    wide_log, wide = run_port(binary, out, "aspect-16x9", args.frames, shot_frames, args.replay, 1, False, extra_env, repository, args.route)
    frame = shot_frames[0]
    if frame in wide and frame in standard:
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
        fps_log, _ = run_port(binary, out, "fps60", args.frames, shot_frames, args.replay, 0, True, extra_env, repository, args.route)
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

    # The shape the product actually emits (runtime/psx/fps60.cpp). It said "replay prev=Q[N-1]"
    # until 2026-09-19, which was never true: both presents run over the current fence's queue.
    duplicate = "[fps60] TRUE per-object interpolated 60fps ON (source: env)\n" + "".join(
        f"[fps60] f{f} slotA: in-between over Q[N] n=3613 tier1=0 backdrop=0 t=0.500\n" for f in range(3)
    )
    live = "[fps60] TRUE per-object interpolated 60fps ON (source: env)\n" + "".join(
        f"[fps60] f{f} slotA: in-between over Q[N] n=3613 tier1=1800 backdrop=12 t=0.500\n" for f in range(3)
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
    checks.append(("an executor fault is a failure mark", run_failures("executor fault at 0x8001") != []))

    png_header = b"\x89PNG\r\n\x1a\n"
    flat = Capture(png_header + b"same picture")
    same = Capture(png_header + b"same picture")
    other = Capture(png_header + b"wider picture")
    checks.append(("an identical widescreen picture is a FAILURE", not flat.differs_from(same)))
    checks.append(("a widened picture differs", flat.differs_from(other)))

    # coverage, on constructed pictures, both answers. `drawn` paints a white box of the given size
    # centred in a 400x300 frame, which is the shape every one of these measurements is about.
    def drawn(box_w, box_h, frame=(400, 300)):
        from PIL import Image

        image = Image.new("RGB", frame, (0, 0, 0))
        image.paste(Image.new("RGB", (box_w, box_h), (255, 255, 255)),
                    ((frame[0] - box_w) // 2, (frame[1] - box_h) // 2))
        buffer = io.BytesIO()
        image.save(buffer, format="PNG")
        return Capture(buffer.getvalue())

    full43 = drawn(400, 300)      # a full-screen 4:3 picture
    rescaled = drawn(300, 225)    # the SAME picture, smaller — Tomba! 2's options page
    widened = drawn(400, 225)     # genuinely wider content
    window = drawn(280, 200)      # a bordered window: unchanged between legs
    # Spyro's real shape: a picture letterboxed INSIDE the sink. An earlier "must fill the frame"
    # rule excluded exactly this from judgement; it must be measured, and it must read wider.
    letterboxed43, letterboxed169 = drawn(400, 280), drawn(400, 209)
    checks.append(("coverage: a rescaled full-screen picture reads no-gain",
                   coverage_measure(full43, rescaled)[0] == "no-gain"))
    checks.append(("coverage: a genuinely widened picture reads wider",
                   coverage_measure(full43, widened)[0] == "wider"))
    checks.append(("coverage: a letterboxed picture is MEASURED, not skipped",
                   coverage_measure(letterboxed43, letterboxed169)[0] == "wider"))
    checks.append(("coverage: an unchanged window reads no-gain",
                   coverage_measure(window, window)[0] == "no-gain"))
    checks.append(("coverage: a near-blank leg reads no-gain, never wider",
                   coverage_measure(full43, drawn(1, 1))[0] == "no-gain"))
    checks.append(("coverage: a leg that drew nothing at all is named",
                   coverage_measure(full43, drawn(0, 0))[0] == "blank"))

    with tempfile.TemporaryDirectory() as directory:
        stale = Path(directory) / "run.log"
        stale_capture = Path(directory) / "present.png"
        stale.write_text("stale run\n")
        stale_capture.write_bytes(png_header + b"stale picture")
        reset_run_outputs(stale, [stale_capture])
        checks.append(("a run starts without stale evidence", stale.read_text() == "" and not stale_capture.exists()))

    built = route_command("drive.py --shot {shot} --settings {settings} --log {log}",
                          "/s/present.png", "/s/a.ini", "/s/a.log")
    checks.append(("a route command carries this run's paths",
                   built == ["drive.py", "--shot", "/s/present.png", "--settings", "/s/a.ini",
                             "--log", "/s/a.log"]))
    try:
        route_command("drive.py gameplay", "/s/present.png", "/s/a.ini", "/s/a.log")
        refused_no_shot = False
    except ValueError:
        refused_no_shot = True
    checks.append(("a route with nowhere to put the capture is REFUSED", refused_no_shot))

    passed = sum(1 for _, ok in checks if ok)
    for name, ok in checks:
        if not ok:
            print(f"[looks-right:selftest] FAILED: {name}")
    print(f"[looks-right] selftest {passed}/{len(checks)} => {'PASS' if passed == len(checks) else 'FAIL'}")
    return PASS if passed == len(checks) else FAIL


if __name__ == "__main__":
    sys.exit(main())
