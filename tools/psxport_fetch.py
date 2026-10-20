#!/usr/bin/env python3
"""psxport_fetch.py: make a title repo's `external/psxport` the live framework.

A title runs this from psxport `main` before it has a framework (`uv run --no-project <raw URL> --auto`
in the title root), so it is stdlib only and imports nothing else from psxport. With a sibling `psxport`
checkout it publishes a relative symlink; with none it shallow-clones psxport `main` and its vendor
submodules.
`PSXPORT_FRAMEWORK_URL` names a fork or mirror for that clone.

No git command runs with its cwd at `external/psxport` or through a link this tool did not create
(issue 0142); a staged tree or link is published by one atomic rename.

`--lightrec` materialises the Lightrec revision pinned in this checkout's
`cmake/lightrec_dependency.cmake` as a detached worktree.
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import re

LINK_REL = os.path.join("external", "psxport")
DEFAULT_URL = "https://github.com/SomeoneIsWorking/psxport.git"
DEFAULT_BRANCH = "main"
# psxport's own vendor submodules, init'd non-recursively (beetle has a URL-less nested gitlink that
# makes `--recursive` fail outright), then that nested gitlink, as (parent, child) pairs.
SUBMODULES = ((".", "vendor/beetle-psx"), (".", "vendor/lucent"), (".", "vendor/rmlui"),
              (".", "vendor/bug-report"), (".", "vendor/setup-ui"), ("vendor/beetle-psx", "deps/libchdr"))
LIGHTREC_REVISION = re.compile(r'set\s*\(\s*PSXPORT_LIGHTREC_REVISION\s+"([0-9a-f]{40})"')


def git(args, cwd):
    """Run git in a real directory, never through `external/psxport` (issue 0142)."""
    done = subprocess.run(["git"] + args, cwd=cwd, capture_output=True, text=True)
    return done.stdout.strip(), done.returncode


def is_framework(path):
    """A psxport checkout, judged by the two files a build needs."""
    return (os.path.isfile(os.path.join(path, "cmake", "psxport.cmake"))
            and os.path.isfile(os.path.join(path, "CMakeLists.txt")))


def describe_link(link):
    """symlink | clone | dir | missing."""
    if os.path.islink(link):
        return "symlink"
    if os.path.isdir(link):
        inner = os.path.join(link, ".git")
        return "clone" if os.path.isdir(inner) or os.path.isfile(inner) else "dir"
    return "missing"


def live_candidates(repo):
    """Where the live framework checkout may be: `$PSX/psxport`, then the sibling of this repository's
    MAIN checkout (found through `git rev-parse --git-common-dir`, so a linked worktree resolves the
    same answer its main checkout does), then the plain sibling. Deduplicated on the resolved path."""
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


def point_link(link, target):
    """Point `link` at `target` with ONE atomic rename. Never replaces a non-symlink: a real clone or
    directory there is somebody's work, and this tool has never been asked to delete one."""
    if os.path.lexists(link) and not os.path.islink(link):
        print(f"[psxport] REFUSED: external/psxport is a {describe_link(link)}; a link never replaces "
              f"one. Inspect it, then remove it yourself and re-run.")
        return 2
    if os.path.islink(link) and os.path.realpath(link) == os.path.realpath(target):
        print(f"[psxport] external/psxport already -> {target}")
        return 0
    os.makedirs(os.path.dirname(link), exist_ok=True)
    staging = os.path.join(os.path.dirname(link), f".psxport-link-{os.getpid()}")
    if os.path.lexists(staging):
        os.unlink(staging)
    os.symlink(os.path.relpath(target, os.path.dirname(link)), staging)
    os.rename(staging, link)             # replaces a symlink atomically; never a directory
    print(f"[psxport] external/psxport -> {os.readlink(link)}  (live framework: edits are immediate)")
    return 0


def source_url():
    """Where the no-sibling clone comes from: `PSXPORT_FRAMEWORK_URL` when set (a fork or an internal
    mirror), else the public repository."""
    return os.environ.get("PSXPORT_FRAMEWORK_URL") or DEFAULT_URL


def do_clone(link):
    """No live checkout on this machine: shallow-clone psxport `main` into a SIBLING of `link` — one
    filesystem, so publishing is an atomic rename — and run every git command there."""
    url, branch = source_url(), DEFAULT_BRANCH
    os.makedirs(os.path.dirname(link), exist_ok=True)
    staging = tempfile.mkdtemp(prefix=".psxport-clone-", dir=os.path.dirname(link))
    published = False
    try:
        print(f"[psxport] no sibling psxport checkout; cloning {url} at {branch}")
        if git(["clone", "--depth", "1", "--branch", branch, url, staging],
               os.path.dirname(link))[1] != 0:
            print("[psxport] REFUSED: clone failed.")
            return 2
        init_submodules(staging)
        if os.path.lexists(link):
            if os.path.islink(link):
                os.unlink(link)           # this tool's own earlier link, replaced by a real checkout
            else:
                print(f"[psxport] REFUSED: external/psxport appeared while the clone was in progress "
                      f"and is now a {describe_link(link)}. Nothing was moved; the clone was discarded.")
                return 2
        try:
            os.rename(staging, link)
            published = True
        except OSError as error:           # another clone landed in the same window
            print(f"[psxport] REFUSED: external/psxport was taken while the clone was in progress "
                  f"({error.strerror or error}). The staged clone was discarded.")
        if not published:
            return 2
        print(f"[psxport] external/psxport cloned from {branch}")
        return 0
    finally:
        if not published:
            shutil.rmtree(staging, ignore_errors=True)


def init_submodules(root):
    """Initialise every declared submodule that is not checked out yet, one level at a time with cwd in
    the real checkout `root` (never through a link). One already checked out is left at its commit."""
    for parent, sub in SUBMODULES:
        path = os.path.join(root, parent)
        if not os.path.isdir(path):
            print(f"[psxport] NOTE: this revision has no {parent}, so {sub} was NOT initialised.")
            continue
        if os.path.exists(os.path.join(path, sub, ".git")):
            continue
        git(["submodule", "update", "--init", sub], path)


def do_ensure(repo, link):
    """Make `external/psxport` the live framework: the sibling checkout when this machine has one, else
    a shallow clone of psxport `main`."""
    for cand in live_candidates(repo):
        if is_framework(cand):
            return point_link(link, cand)
    if os.path.islink(link) and is_framework(link):
        # A link to a live framework checkout that discovery could not name from here — a linked
        # worktree, or a checkout the workspace moved. It is the only framework this port has, so it
        # is left alone rather than replaced by a clone.
        print(f"[psxport] external/psxport is a symlink to {os.path.realpath(link)}; left alone.")
        return 0
    if describe_link(link) == "clone" and is_framework(link):
        # An earlier run's clone (CI resolves the framework, then the verifier asks again).
        print("[psxport] external/psxport is a clone; left alone.")
        return 0
    return do_clone(link)


def pinned_lightrec_revision(repo):
    """The Lightrec revision psxport pins, read from the module that consumes it — ONE source for the
    pin, so this tool and cmake/lightrec_dependency.cmake cannot disagree about it."""
    source = os.path.join(repo, "cmake", "lightrec_dependency.cmake")
    if not os.path.isfile(source):
        return None
    with open(source, encoding="utf-8") as handle:
        found = LIGHTREC_REVISION.search(handle.read())
    return found.group(1) if found else None


def do_lightrec(repo):
    """Ensure the pinned Lightrec revision exists as a detached worktree under the shared Lightrec
    checkout, and print its path. `cmake/lightrec_dependency.cmake` probes `scratch/pins/<revision>/`
    before the plain checkout and refuses a checkout that is not at the pinned revision, so a Lightrec
    commit landing under a running consumer cannot change what that consumer builds."""
    sha = pinned_lightrec_revision(repo)
    if not sha:
        print("[psxport] REFUSED: no PSXPORT_LIGHTREC_REVISION in cmake/lightrec_dependency.cmake, so "
              "there is no Lightrec commit to pin to.")
        return 2
    parent = os.path.dirname(os.path.abspath(repo))
    roots = []
    if os.environ.get("SHARED_DIR"):
        roots.append(os.path.join(os.environ["SHARED_DIR"], "lightrec"))
    roots += [os.path.join(parent, "shared", "lightrec"),
              os.path.join(os.path.dirname(parent), "shared", "lightrec")]
    for candidate in roots:
        if not os.path.isfile(os.path.join(candidate, "CMakeLists.txt")):
            continue
        target = os.path.join(candidate, "scratch", "pins", sha)
        if not os.path.isdir(target):
            os.makedirs(os.path.dirname(target), exist_ok=True)
            # `git -C` names the real checkout; cwd stays here, never inside the published worktree.
            if git(["-C", candidate, "worktree", "add", "--detach", target, sha], repo)[1] != 0:
                print(f"[psxport] REFUSED: could not create the pinned Lightrec worktree at {target}. "
                      f"It was NOT touched.")
                return 2
            print(f"[psxport] Lightrec worktree created at {sha[:12]}")
        print(f"[psxport] Lightrec worktree at {sha[:12]} -> {target}")
        return 0
    print("[psxport] REFUSED: no shared Lightrec checkout found. Looked in: " + ", ".join(roots))
    return 2


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--repo", default=os.getcwd(),
                    help="title repository whose external/psxport must exist (default: the current directory)")
    ap.add_argument("--auto", action="store_true",
                    help="the default and only action: point external/psxport at the live framework, "
                         "cloning psxport main when this machine has no sibling checkout")
    ap.add_argument("--lightrec", action="store_true",
                    help="instead, ensure the pinned Lightrec worktree and print its path")
    args = ap.parse_args(argv)
    if args.lightrec:
        return do_lightrec(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    repo = os.path.abspath(args.repo)
    return do_ensure(repo, os.path.join(repo, LINK_REL))


if __name__ == "__main__":
    sys.exit(main())