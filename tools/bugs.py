#!/usr/bin/env python3
# bugs.py — CLI for the port-bug board (backed by GitHub Issues).
#
# WHY: the port has multiple LIVE bugs (wrong SFX, XA stops, fade stuck black, …). GitHub
# Issues at github.com/SomeoneIsWorking/psxport is the shared source of truth — labels for
# area (bug/render/audio/…), status via `investigating` / `ported-unverified` labels, comments
# for the investigation trail, and it's already in use across the project (26+ existing issues).
# This tool is the thin CLI over `gh issue` that surfaces the psxport bug-workflow (add,
# comment, close, board view) so an agent doesn't hand-craft `gh` invocations each session
# and human-drafts issue markdown from scratch.
#
# WORKFLOW (do this, every session):
#   tools/bugs.py                       # board view: OPEN bugs, most recent first
#   tools/bugs.py <words>               # search across title/body/comments
#   tools/bugs.py show <n>              # full issue + comments
#   tools/bugs.py add                   # new bug: prompts for area, symptom, hypothesis
#   tools/bugs.py investigating <n>     # flip status to `investigating`
#   tools/bugs.py ported <n>            # flip status to `ported-unverified` (post-port)
#   tools/bugs.py comment <n> <text>    # append an investigation-note comment
#   tools/bugs.py close <n> <text>      # comment + close (post user-confirms fix)
#
# The status is expressed via LABELS (github's model) — `investigating` / `ported-unverified`
# ride alongside area labels (bug/render/audio/…). Removing one and adding another is how a
# bug transitions state. Comments carry the investigation trail (what you tried, what was
# ruled out) — findings that emerge get PROMOTED to docs/findings/*.md at close time.
#
# The tool always operates on the origin repo of the CWD (parsed from `git remote`) so it
# stays repo-agnostic if this workflow gets copied to another port. Requires `gh auth`.
import argparse, json, os, re, subprocess, sys, textwrap
from pathlib import Path

STATUS_LABELS = ("investigating", "ported-unverified")   # mutually exclusive; absence = OPEN
AREA_LABELS   = ("render", "render-arch", "audio", "bug")

def _repo() -> str:
    """Read origin URL from git and normalise to owner/repo."""
    url = subprocess.check_output(["git", "remote", "get-url", "origin"], text=True).strip()
    m = re.match(r"^(?:https?://github\.com/|git@github\.com:)([^/]+)/(.+?)(?:\.git)?$", url)
    if not m: sys.exit(f"could not parse repo from origin {url!r}")
    return f"{m.group(1)}/{m.group(2)}"

def _gh(*args, capture=True, check=True, input=None):
    cmd = ["gh", *args]
    if capture:
        res = subprocess.run(cmd, capture_output=True, text=True, input=input)
        if check and res.returncode != 0:
            sys.exit(f"gh failed: {' '.join(cmd)}\n{res.stderr}")
        return res.stdout
    return subprocess.run(cmd, check=check, input=input, text=True).returncode

def _issue_list(repo, needles=None, state="open", limit=50):
    args = ["issue", "list", "-R", repo, "--state", state, "--limit", str(limit),
            "--json", "number,title,labels,state,updatedAt"]
    if needles: args += ["--search", " ".join(needles)]
    out = _gh(*args)
    return json.loads(out)

def _status_of(labels_list):
    names = {l["name"] for l in labels_list}
    for s in STATUS_LABELS:
        if s in names: return s
    return "open"

def _wrap_status(s):
    if not sys.stdout.isatty(): return s
    return {"open":            "\033[31m",   # red
            "investigating":   "\033[33m",   # yellow
            "ported-unverified":"\033[36m"}.get(s, "") + s + "\033[0m"

def _labels_str(labels):
    kept = [l["name"] for l in labels if l["name"] not in STATUS_LABELS]
    return ",".join(kept) if kept else "-"

def cmd_list(args):
    repo = _repo()
    issues = _issue_list(repo, args.needles, state=args.state, limit=args.limit)
    # Filter to bug-labelled if the state is open (the default board view) — enhancement-only
    # issues would drown out the bug signal. `--all` skips this.
    if not args.all:
        issues = [i for i in issues if any(l["name"] == "bug" for l in i["labels"])]
    if not issues:
        print("no matching issues"); return
    counts = {}
    for i in issues:
        st = _status_of(i["labels"])
        counts[st] = counts.get(st, 0) + 1
        print(f"#{i['number']:>3}  [{_wrap_status(st)}]  ({_labels_str(i['labels'])})  {i['title']}")
    summary = " · ".join(f"{_wrap_status(s)}={c}" for s, c in sorted(counts.items()))
    print(f"\n{summary}   ({len(issues)} shown)")

def cmd_show(args):
    print(_gh("issue", "view", str(args.number), "-R", _repo(), "--comments"))

def cmd_add(args):
    repo = _repo()
    body = args.body
    if not body and not sys.stdin.isatty():
        body = sys.stdin.read()
    if not body:
        sys.exit("body required (use --body or pipe on stdin)")
    label_args = ["--label", "bug"]
    for lab in args.labels or []:
        label_args += ["--label", lab]
    out = _gh("issue", "create", "-R", repo, "--title", args.title, "--body", body, *label_args)
    # `gh issue create` prints the URL of the new issue.
    print(out.strip())

def _remove_status_labels(repo, number):
    for s in STATUS_LABELS:
        _gh("issue", "edit", str(number), "-R", repo, "--remove-label", s, check=False)

def cmd_set_status(args):
    repo = _repo()
    _remove_status_labels(repo, args.number)
    if args.status != "open":
        _gh("issue", "edit", str(args.number), "-R", repo, "--add-label", args.status)
    print(f"#{args.number}: {_wrap_status(args.status)}")

def cmd_comment(args):
    repo = _repo()
    text = args.text or (sys.stdin.read() if not sys.stdin.isatty() else None)
    if not text: sys.exit("comment text required")
    _gh("issue", "comment", str(args.number), "-R", repo, "--body", text)
    print(f"#{args.number}: commented")

def cmd_close(args):
    repo = _repo()
    if args.text:
        _gh("issue", "comment", str(args.number), "-R", repo, "--body", args.text)
    _gh("issue", "close", str(args.number), "-R", repo)
    print(f"#{args.number}: closed")

def cmd_reopen(args):
    repo = _repo()
    _gh("issue", "reopen", str(args.number), "-R", repo)
    if args.text:
        _gh("issue", "comment", str(args.number), "-R", repo, "--body", args.text)
    print(f"#{args.number}: reopened")

def build_parser():
    p = argparse.ArgumentParser(description="CLI over `gh issue` for the psxport bug board.")
    sub = p.add_subparsers(dest="cmd")

    lp = sub.add_parser("list", help="board view (default)")
    lp.add_argument("needles", nargs="*", help="search terms (AND across title/body)")
    lp.add_argument("--state", default="open", choices=["open","closed","all"])
    lp.add_argument("--limit", type=int, default=50)
    lp.add_argument("--all", action="store_true", help="include non-bug-labelled issues")
    lp.set_defaults(fn=cmd_list)

    sp = sub.add_parser("show", help="show issue + comments")
    sp.add_argument("number", type=int)
    sp.set_defaults(fn=cmd_show)

    ap = sub.add_parser("add", help="create a new bug issue")
    ap.add_argument("title")
    ap.add_argument("--body", help="body markdown (or pipe on stdin)")
    ap.add_argument("--label", action="append", dest="labels", metavar="LABEL",
                    help="additional label (repeat for many). `bug` is always added.")
    ap.set_defaults(fn=cmd_add)

    for status in STATUS_LABELS + ("open",):
        stp = sub.add_parser(status, help=f"set status to `{status}`")
        stp.add_argument("number", type=int)
        stp.set_defaults(fn=cmd_set_status, status=status)

    cp = sub.add_parser("comment", help="append a comment (investigation note)")
    cp.add_argument("number", type=int)
    cp.add_argument("text", nargs="?", help="comment text (or pipe on stdin)")
    cp.set_defaults(fn=cmd_comment)

    xp = sub.add_parser("close", help="close (optionally with a farewell comment)")
    xp.add_argument("number", type=int)
    xp.add_argument("text", nargs="?", help="closing comment")
    xp.set_defaults(fn=cmd_close)

    rp = sub.add_parser("reopen", help="reopen a closed issue")
    rp.add_argument("number", type=int)
    rp.add_argument("text", nargs="?", help="reopening comment")
    rp.set_defaults(fn=cmd_reopen)

    # positional-only default: `tools/bugs.py foo bar` = search
    return p

def main(argv):
    parser = build_parser()
    # Legacy shim: `bugs.py <words>` (no subcommand) = `list <words>`.
    if argv and argv[0] not in {"list","show","add","comment","close","reopen","-h","--help"} \
             and argv[0] not in STATUS_LABELS and argv[0] != "open":
        argv = ["list", *argv]
    args = parser.parse_args(argv)
    if not args.cmd: args = parser.parse_args(["list"])
    args.fn(args)

if __name__ == "__main__":
    main(sys.argv[1:])
