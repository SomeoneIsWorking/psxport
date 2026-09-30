#!/usr/bin/env python3
"""psxport_sync.py — the framework-side pin tool: report, check, bump, and an explicit link.

WHAT IT IS NOT ANY MORE. Until 2026-09-30 this file also did the bootstrap — "make
`external/psxport` exist", by linking a shared checkout or cloning the pin. That is now
`tools/psxport_fetch.py`, because a port needs it BEFORE it has the framework and this file lives
INSIDE the framework. A port ships `tools/psxport_fetch.py` and nothing else, and runs the pin work
out of the fetched checkout:

    python3 tools/psxport_fetch.py                     # pin external/psxport to psxport.pin's commit
    python3 external/psxport/tools/psxport_sync.py --repo . --check --build build

`external/psxport` resolves to a checkout of the PINNED commit — a detached worktree under the shared
checkout's gitignored `scratch/pins/<sha>/`, never the moving shared checkout — so a framework commit
landing under a consumer cannot change what that consumer builds. `--link` below follows the same rule:
it points at that pinned worktree and refuses when there is none.

`--repo` is the title repository to act on, defaulting to the current directory, so the same text
serves a workspace, a bare clone and a CI runner without editing itself.

WHY A CANONICAL SOURCE EXISTS
-----------------------------
`check_port_pin_tools.py` installs the canonical `fetch` text into every port and fails on drift; that
obligation exists because the copies DID drift, and the drift was not hypothetical:

  - MEASURED 2026-09-27: the ten copies had TEN DISTINCT HASHES and 298–322 lines each.
  - The staleness guard was MISSING FROM SEVEN OF THE TEN, and where it was missing the check answered
    `check OK` on input a guarded copy refused. Same input, opposite answers, and the wrong one is a pass.
  - Only ONE of the ten had a `--build` flag, so the live pin check that actually calls this function
    could not be registered in the other nine.
  - Only ONE had the `do_bump` fix, so the other nine could still record a framework commit the tree was
    never built against — which is the original incident this whole mechanism exists to prevent.

This file is the one the check compares and the one a port runs from its fetched framework. It is
never installed into a port any more (that is the fetch tool's job), so nothing here has to keep working
from an arbitrary `tools/` directory.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys

LINK_REL = os.path.join("external", "psxport")
PIN_NAME = "psxport.pin"
DEFAULT_BUILD = "build/ci"
DEFAULT_URL = "https://github.com/SomeoneIsWorking/psxport.git"


def git(args, cwd):
    done = subprocess.run(["git"] + args, cwd=cwd, capture_output=True, text=True)
    return done.stdout.strip(), done.returncode


def is_framework(path):
    return os.path.isfile(os.path.join(path, "cmake", "psxport.cmake"))


def describe_link(link):
    """(kind, target). What external/psxport currently IS. Never guesses — 'missing' is a real answer."""
    if os.path.islink(link):
        return "symlink", os.path.realpath(link)
    if os.path.isdir(link):
        inner = os.path.join(link, ".git")
        kind = "clone" if os.path.isdir(inner) or os.path.isfile(inner) else "plain-dir"
        return kind, link
    return "missing", link


def read_pin(pin_file):
    """Returns (url, commit) or (None, None). A malformed pin is a refusal, never a silent default."""
    if not os.path.isfile(pin_file):
        return None, None
    url, commit = DEFAULT_URL, None
    # Closed explicitly. A bare `for line in open(...)` leaves the handle to the garbage collector, which
    # is invisible in normal use but shows up as a ResourceWarning the moment a test reads real receipts
    # repeatedly — the selftest does, and it found this.
    with open(pin_file, encoding="utf-8") as handle:
        for line in handle:
            found = re.match(r"(\w+)\s*=\s*(\S+)", line.split("#", 1)[0].strip())
            if found and found.group(1) in ("url", "commit"):
                url, commit = (found.group(2), commit) if found.group(1) == "url" else (url, found.group(2))
    return url, commit


def write_pin(pin_file, url, commit):
    with open(pin_file, "w", encoding="utf-8") as handle:
        handle.write(
            "# psxport framework pin — the commit this game was built and VERIFIED against.\n"
            "# Managed by the framework's tools/psxport_sync.py (--bump to record the framework you are\n"
            "# building against now). This is provenance and the fresh-clone fallback; locally the build\n"
            "# runs off the shared framework clone via the external/psxport symlink, which\n"
            "# tools/psxport_fetch.py establishes. Ports are deliberately not all on framework HEAD — see\n"
            "# that tool's module docstring for why.\n"
            f"url = {url}\n"
            f"commit = {commit}\n"
        )


def head_of(path):
    if not os.path.isdir(path):
        return None
    sha, rc = git(["rev-parse", "HEAD"], path)
    return sha if rc == 0 else None


def dirty(path):
    out, rc = git(["status", "--porcelain"], path)
    return bool(out) if rc == 0 else False


def read_resolved(build):
    """(dir, sha) the selected CMake configure resolved, or None. Written by CMakeLists."""
    receipt = os.path.join(build, "psxport_resolved.txt")
    if not os.path.isfile(receipt):
        return None
    directory = sha = None
    with open(receipt, encoding="utf-8") as handle:
        for line in handle:
            key, _, value = line.partition("=")
            if key.strip() == "dir":
                directory = value.strip()
            elif key.strip() == "commit":
                sha = value.strip()
    return (directory, sha) if sha else None


def shared_candidates(repo):
    """Where a shared checkout may be, in the order psxport_fetch asks: $PSX, the MAIN checkout's
    sibling, then this repository's own.

    DELIBERATE, BOUNDED DUPLICATION. psxport_fetch.py is the copy a port ships and this file must work
    with only the fetched framework on disk, so neither can import the other. The list is nine lines,
    both are covered by tests, and when it changes both change. The two tools have disjoint jobs, which
    is why that is acceptable here and would not be anywhere else.
    """
    out = []
    if os.environ.get("PSX"):
        out.append(os.path.join(os.environ["PSX"], "psxport"))
    common, rc = git(["rev-parse", "--git-common-dir"], repo)
    if rc == 0 and common:
        common = common if os.path.isabs(common) else os.path.join(repo, common)
        out.append(os.path.join(os.path.dirname(os.path.abspath(common)), "..", "psxport"))
    out.append(os.path.join(os.path.abspath(repo), "..", "psxport"))
    seen, unique = set(), []
    for cand in out:
        key = os.path.realpath(cand)
        if key not in seen and key != os.path.realpath(repo):
            seen.add(key)
            unique.append(cand)
    return unique


def point_link(link, target, force):
    """Point `link` at `target` with one atomic rename, and never replace a non-symlink: a real clone or
    directory there is somebody's work, and this tool has never been asked to delete one."""
    if os.path.lexists(link) and not os.path.islink(link) and not force:
        print(f"[psxport] REFUSED: external/psxport is a {describe_link(link)[0]}; a link never replaces "
              f"one. Inspect it, then remove it yourself and re-run with --force if you mean it.")
        return 2
    if os.path.islink(link) and os.path.realpath(link) == os.path.realpath(target):
        return 0
    os.makedirs(os.path.dirname(link), exist_ok=True)
    staging = os.path.join(os.path.dirname(link), f".psxport-link-{os.getpid()}")
    if os.path.lexists(staging):
        os.unlink(staging)
    os.symlink(os.path.relpath(target, os.path.dirname(link)), staging)
    os.rename(staging, link)
    return 0


def do_link(repo, link, pin_file, force):
    """Point external/psxport at the PINNED checkout of the shared framework.

    It links to a pinned WORKTREE, never to the shared checkout itself: a link to a moving checkout makes
    every build under it silently follow whatever that checkout is at, and a framework commit landing
    under a running consumer is then indistinguishable from the consumer having broken. The worktree is
    created by `tools/psxport_fetch.py`; this only finds and points at one, and refuses when there is
    none, because making one is that tool's whole job.
    """
    _, pin = read_pin(pin_file)
    if not pin:
        print("[psxport] REFUSED: no usable psxport.pin, so there is no commit to link to.")
        return 2
    for cand in shared_candidates(repo):
        if not is_framework(cand):
            continue
        pinned = os.path.join(cand, "scratch", "pins", pin)
        if not os.path.isdir(pinned):
            print(f"[psxport] REFUSED: no pinned worktree for {pin[:12]} under {cand}. Run "
                  f"tools/psxport_fetch.py — creating one is that tool's job, and pointing at a tree "
                  f"that does not exist would break the build in a harder way.")
            return 2
        status = point_link(link, pinned, force)
        if status == 0:
            print(f"[psxport] external/psxport -> {pinned}  (pinned at {pin[:12]}; framework edits are "
                  f"NOT live here)")
        return status
    print("[psxport] no shared framework checkout found. Looked in: "
          + ", ".join(shared_candidates(repo))
          + "\n[psxport] use tools/psxport_fetch.py, which pins this repo to its recorded commit.")
    return 2


def do_report(link, pin_file, build):
    kind, target = describe_link(link)
    _, pin = read_pin(pin_file)
    sha = head_of(target) if kind in ("symlink", "clone") else None
    print(f"[psxport] external/psxport : {kind}" + (f" -> {target}" if kind == "symlink" else ""))
    print(f"[psxport] framework HEAD   : {sha or '(none)'}"
          + ("  +dirty" if sha and dirty(target) else ""))
    print(f"[psxport] recorded pin     : {pin or '(no psxport.pin)'}")
    built = read_resolved(build)
    if built:
        print(f"[psxport] last build used  : {built[1]}  (from {built[0]})")
    if sha and pin:
        if sha == pin:
            print("[psxport] IN SYNC — the checkout you build from is the commit this repo records.")
        else:
            ahead, _ = git(["rev-list", "--count", f"{pin}..{sha}"], target)
            print(f"[psxport] DRIFT — the checkout is {ahead or '?'} commit(s) off the recorded pin. "
                  f"That is normal WHILE doing framework work; record it before you land game code "
                  f"that needs it:  python3 external/psxport/tools/psxport_sync.py --bump")
    return 0


def check_build_pin(link, pin_file, build):
    """Refuse unless this exact CMake build's framework receipt matches the recorded pin."""
    _, pin = read_pin(pin_file)
    if not pin:
        print("[psxport] REFUSED: no psxport.pin — this check asserted NOTHING.")
        return 2
    built = read_resolved(build)
    if not built:
        print(f"[psxport] REFUSED: no usable psxport_resolved.txt in {build}; "
              f"nothing can be compared with pin {pin[:8]}.")
        return 2
    bdir, bsha = built
    # THE STALENESS GUARD, and it is the whole point of this check. `bsha` is what CMake recorded at
    # CONFIGURE time, so a plain `cmake --build` never refreshes it. Without comparing it against the
    # framework's CURRENT head, a tree rebuilt against newer framework code still reports the OLD commit,
    # matches its pin, and passes -- so a fresh clone would build a different framework than the one just
    # tested, which is the single failure the pin exists to prevent.
    #
    # MEASURED 2026-09-27: this guard was present in 3 of 10 copies of the tool and absent from the
    # other 7, and the difference is observable. With `psxport_resolved.txt` naming a repo's own recorded
    # pin while the shared framework sits eight commits later, a guarded copy refuses --
    #   "check FAILED -- framework .../psxport is dirty or changed since configure (configured 436c3762,
    #    current ba48b103)" -- and an unguarded one on the same input answers "check OK -- built against
    # e0485d33, which is the recorded pin." Same input, opposite answers, and the wrong one is a pass.
    current = head_of(bdir)
    if current != bsha or dirty(bdir):
        print(f"[psxport] check FAILED — framework {bdir} is dirty or changed since configure "
              f"(configured {bsha}, current {current}). Rebuild from a reconfigure before trusting this "
              f"tree's pin, or bump the pin to what you actually built and tested.")
        return 1
    if bsha == pin:
        print(f"[psxport] check OK — {build} was built against {bsha[:8]}, which is the recorded pin.")
        return 0
    print(f"[psxport] check FAILED — you built against {bsha[:8]} (from {bdir}) but this repo records "
          f"{pin[:8]}.")
    print(f"[psxport]   A fresh clone would build a DIFFERENT framework than you just tested. That is "
          f"how this tree once recorded a pin whose GameHooks lacked a field the game used.")
    print("[psxport]   Fix: reconfigure and verify this build against the recorded pin, "
          "or bump the pin only after verifying a different published framework commit.")
    return 1


def do_bump(link, pin_file, build):
    """Record the framework commit THIS BUILD resolved -- not the framework's current HEAD.

    WHY THIS IS NOT `head_of(target)`. It used to be. Recording HEAD means the pin names whatever the
    framework is at when you run the bump, which is unrelated to what this tree was compiled and tested
    against, and it makes the documented order `reconfigure -> build -> test -> --bump` a convention that
    nothing enforces: `--bump` alone, with no build at all, would record a commit this tree has never
    seen. That is not hypothetical. This repo shipped built against psxport `25dd7826` while recording
    `a1c53d7c`, so a bare clone named a framework whose `GameHooks` lacked a field the game used, and
    nothing noticed because a submodule working tree and its recorded gitlink drift silently. The pin
    existed to make that failure loud, and the bump was the one step that could re-create it.

    So a bump reads the same receipt, applies the same staleness guard, and selects the same build as
    `--check`, which makes the two agree by construction rather than by two people remembering an order.
    """
    kind, target = describe_link(link)
    built = read_resolved(build)
    if not built:
        print(f"[psxport] REFUSED: no usable psxport_resolved.txt in {build}; nothing was configured "
              f"there, so there is nothing to record. Reconfigure and build FIRST, then bump -- a pin "
              f"records a verification, and no build is no verification.")
        return 2
    bdir, bsha = built
    # The build must have resolved the tree this repo LINKS, or the receipt describes a framework this
    # port is not actually consuming.
    if kind in ("symlink", "clone") and os.path.realpath(bdir) != os.path.realpath(target):
        print(f"[psxport] REFUSED: {build} was configured against {bdir}, but external/psxport "
              f"points at {target}. Bumping would record a framework this port does not consume.")
        return 1
    # The same guard --check applies: a receipt that is already stale is not a verification.
    current = head_of(bdir)
    if current != bsha or dirty(bdir):
        print(f"[psxport] REFUSED: {build}'s receipt is stale -- framework {bdir} is dirty or "
              f"changed since configure (configured {bsha}, current {current}). Reconfigure, rebuild "
              f"and retest, then bump.")
        return 1
    url, old = read_pin(pin_file)
    remote_has, rc = git(["branch", "-r", "--contains", bsha], bdir)
    if rc != 0 or not remote_has.strip():
        print(f"[psxport] REFUSED: {bsha[:8]} is not on any remote branch. Recording it would leave a "
              f"pin that a fresh clone cannot fetch -- which is exactly how this repo shipped a tree "
              f"that did not build standalone. Push the framework first.")
        return 1
    write_pin(pin_file, url or DEFAULT_URL, bsha)
    print(f"[psxport] pin {(old or '(none)')[:8]} -> {bsha[:8]}")
    print(f"[psxport]   recorded from {build}'s receipt -- the commit that build resolved -- not "
          f"from the framework's current HEAD.")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--repo", default=os.getcwd(),
                    help="title repository to act on (default: the current directory)")
    ap.add_argument("--link", action="store_true",
                    help="point external/psxport at the PINNED worktree of a shared checkout")
    ap.add_argument("--bump", action="store_true", help="record the framework you are building against")
    ap.add_argument("--check", action="store_true", help="fail if the built framework is not the pin")
    ap.add_argument("--force", action="store_true", help="allow --link to replace a real clone")
    ap.add_argument("--build", default=None,
                    help="CMake build directory whose framework receipt to inspect (default: <repo>/"
                         + DEFAULT_BUILD + ")")
    args = ap.parse_args(argv)
    repo = os.path.abspath(args.repo)
    link, pin_file = os.path.join(repo, LINK_REL), os.path.join(repo, PIN_NAME)
    build = args.build or os.path.join(repo, DEFAULT_BUILD)
    if args.link:
        return do_link(repo, link, pin_file, args.force)
    if args.bump:
        return do_bump(link, pin_file, build)
    if args.check:
        return check_build_pin(link, pin_file, build)
    return do_report(link, pin_file, build)


if __name__ == "__main__":
    sys.exit(main())
