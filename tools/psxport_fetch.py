#!/usr/bin/env python3
"""psxport_fetch.py: make a title repo's `external/psxport` the live framework.

A title runs this from psxport `main` before it has a framework (`uv run --no-project <raw URL> --auto`
in the title root), so it is stdlib only and imports nothing else from psxport. With a sibling `psxport`
checkout it publishes a relative symlink; with none it shallow-clones psxport `main` and its vendor
submodules.
`PSXPORT_FRAMEWORK_URL` names a fork or mirror for that clone.

No git command runs with its cwd at `external/psxport` or through a link this tool did not create
(issue 0142); a staged tree or link is published by one atomic rename.
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile

LINK_REL = os.path.join("external", "psxport")
DEFAULT_URL = "https://github.com/SomeoneIsWorking/psxport.git"
DEFAULT_BRANCH = "main"
# The build's submodules are every `.gitmodules` path under vendor/, init'd non-recursively (beetle has a
# URL-less nested gitlink that makes `--recursive` fail outright), then that nested gitlink.
VENDOR_PREFIX = "vendor/"
NESTED_SUBMODULES = (("vendor/beetle-psx", "deps/libchdr"),)


def git(args, cwd):
    """Run git in a real directory, never through `external/psxport` (issue 0142)."""
    done = subprocess.run(["git", *args], cwd=cwd, capture_output=True, text=True, check=False)
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


def submodules(root):
    """(parent, child) pairs to init in `root`: its declared vendor submodules, then the nested ones."""
    paths, code = git(["config", "-f", ".gitmodules", "--get-regexp", r"^submodule\..*\.path$"], root)
    declared = [line.split(None, 1)[1] for line in paths.splitlines()] if code == 0 else []
    return [(".", path) for path in declared if path.startswith(VENDOR_PREFIX)] + list(NESTED_SUBMODULES)


def init_submodules(root):
    """Check out every build submodule at the commit its parent records, one level at a time with cwd
    in the real checkout `root` (never through a link); Beetle's URL-less nested gitlink rules out --recursive."""
    for parent, sub in submodules(root):
        path = os.path.join(root, parent)
        if not os.path.isdir(path):
            print(f"[psxport] NOTE: this revision has no {parent}, so {sub} was NOT initialised.")
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


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--repo", default=os.getcwd(),
                    help="title repository whose external/psxport must exist (default: the current directory)")
    ap.add_argument("--auto", action="store_true",
                    help="the default and only action: point external/psxport at the live framework, "
                         "cloning psxport main when this machine has no sibling checkout")
    args = ap.parse_args(argv)
    repo = os.path.abspath(args.repo)
    return do_ensure(repo, os.path.join(repo, LINK_REL))


if __name__ == "__main__":
    sys.exit(main())