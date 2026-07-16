#!/usr/bin/env python3
# parity.py — the project's SBS PARITY REGISTRY (Job #1's status ledger).
#
# WHY THIS EXISTS: the whole port is "pc_faithful must be byte-exact to recomp_path" (CLAUDE.md Job #1),
# verified with the SBS harness. But SBS runs are ad-hoc — a subsystem gets proven 0-diff in one session
# and that proof evaporates. This registry is the durable ledger: for each ported unit, IS it SBS
# byte-exact, HOW do I re-verify it (the exact gate command), and what's the evidence (frames/commit).
# It answers "is it verified correct", the third axis of the tracking stack:
#     codemap.py   = WHERE the code is (guest addr -> native owner)
#     portmap.py   = IS it ported, and REAL not a hack (RE+port frontier)
#     parity.py    = IS it SBS byte-exact  <-- this tool
#     behavior.py  = WHAT it deliberately changes (sanctioned pc_render/skip/enh divergences)
#
# THE LOOP:
#   tools/parity.py                       # session start: what's verified / diverging / untested
#   ... own/change a per-frame subsystem, run its SBS gate ...
#   tools/parity.py set <unit> --status verified --frames 10890 --gate '<cmd>' --evidence <commit>
#   tools/parity.py check                 # pre-push gate: exit 1 if anything is `diverges`
#
# STATUS values:
#   verified  — SBS 0-diff proven; MUST cite `frames` + `gate` + `evidence`.
#   diverges  — a known byte divergence exists (a live Job#1 bug). MUST cite `notes` (what/where) + a
#               findings ref. `check` FAILS while any unit is here — no "known diff" is acceptable.
#   partial   — verified for some scenarios/inputs, not all; `notes` says what's still uncovered.
#   untested  — owned but never SBS-gated. `check` warns.
#   n/a       — content kept as recompiled PSX (not a faithful-port target); excluded from the gate.
#
# ENTRY BLOCK FORMAT in docs/parity-map.md (one `## ` per unit; hand-editable OR via `set`):
#   ## <unit name>
#   - **scope:** guest addrs / subsystem this covers
#   - **status:** verified | diverges | partial | untested | n/a
#   - **frames:** N            (frames the SBS gate ran 0-diff; for `verified`)
#   - **gate:** <exact repro command that re-verifies this unit>
#   - **evidence:** <commit> <date> — short note
#   - **owner:** game/.../file.cpp:line
#   - **notes:** divergence detail / uncovered scenarios / findings ref
import os, re, sys, argparse

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DOC = os.path.join(ROOT, "docs", "parity-map.md")
FIELDS = ("scope", "status", "frames", "gate", "evidence", "owner", "notes")
STATUSES = ("verified", "diverges", "partial", "untested", "n/a")
HEADER = "# SBS parity map — is each ported unit byte-exact to recomp_path? (managed by tools/parity.py)\n"


def parse(text):
    """Yield (unit, {field: text}) blocks."""
    for b in re.split(r"(?m)^## +", text)[1:]:
        title = b.splitlines()[0].strip()
        fields = {}
        for k in FIELDS:
            m = re.search(r"(?im)^\s*[-*]?\s*\*\*%s:\*\*\s*(.*?)\s*$" % k, b)
            if m and m.group(1).strip():
                fields[k] = m.group(1).strip()
        yield title, fields


def load():
    if not os.path.exists(DOC):
        return []
    with open(DOC) as f:
        return list(parse(f.read()))


def render(entries):
    order = {s: i for i, s in enumerate(STATUSES)}
    entries = sorted(entries, key=lambda e: (order.get(e[1].get("status", "untested"), 9), e[0].lower()))
    out = [HEADER,
           "Durable ledger for Job #1 (byte-exact pc_faithful). One `## ` block per ported unit.",
           "`tools/parity.py` = summary · `tools/parity.py <words>` = search · `tools/parity.py check` = gate.",
           ""]
    # summary line
    counts = {}
    for _, f in entries:
        counts[f.get("status", "untested")] = counts.get(f.get("status", "untested"), 0) + 1
    out.append("**Status:** " + " · ".join(f"{counts[s]} {s}" for s in STATUSES if s in counts) + "\n")
    for unit, f in entries:
        out.append(f"## {unit}")
        for k in FIELDS:
            if k in f:
                out.append(f"- **{k}:** {f[k]}")
        out.append("")
    return "\n".join(out).rstrip() + "\n"


def save(entries):
    with open(DOC, "w") as f:
        f.write(render(entries))


def cmd_set(args):
    entries = load()
    d = {k: v for k, v in vars(args).items() if k in FIELDS and v is not None}
    if "status" in d and d["status"] not in STATUSES:
        sys.exit(f"bad status {d['status']!r}; expected one of {STATUSES}")
    for i, (unit, f) in enumerate(entries):
        if unit.lower() == args.unit.lower():
            f.update(d)
            entries[i] = (unit, f)
            break
    else:
        entries.append((args.unit, d))
    save(entries)
    print(f"[parity] set {args.unit}: " + ", ".join(f"{k}={v}" for k, v in d.items()))


def cmd_check(args):
    entries = load()
    diverges = [u for u, f in entries if f.get("status") == "diverges"]
    untested = [u for u, f in entries if f.get("status") == "untested"]
    verified_no_evidence = [u for u, f in entries
                            if f.get("status") == "verified" and not (f.get("gate") and f.get("evidence"))]
    for u in untested:
        print(f"  ⚠ untested: {u}")
    for u in verified_no_evidence:
        print(f"  ⚠ verified but missing gate/evidence: {u}")
    for u in diverges:
        print(f"  ✗ DIVERGES (Job#1 bug): {u}")
    if diverges:
        print(f"[parity] FAIL — {len(diverges)} unit(s) diverge; Job #1 is byte-exact-or-nothing.")
        return 1
    print(f"[parity] ok — {len(entries)} units, 0 diverging "
          f"({len(untested)} untested, {len(verified_no_evidence)} verified w/o evidence).")
    return 0


def cmd_list(args):
    for unit, f in sorted(load(), key=lambda e: e[0].lower()):
        if args.status and f.get("status") != args.status:
            continue
        print(f"{f.get('status','?'):9} {unit:40} {f.get('owner','')}")


def cmd_search(words):
    entries = load()
    ws = [w.lower() for w in words]
    hits = [(u, f) for u, f in entries if all(w in (u + " " + " ".join(f.values())).lower() for w in ws)]
    for u, f in hits:
        print(f"## {u}")
        for k in FIELDS:
            if k in f:
                print(f"   {k}: {f[k]}")
        print()
    if not hits:
        print("(no parity entries match; add one with `parity.py set <unit> ...`)")


def main():
    # Bare-word search: `parity.py <words>`. argparse subparsers reject an unknown first positional as an
    # "invalid choice" before any fallback runs, so intercept it here — a first arg that is neither a
    # known subcommand nor a flag means "search". Flags / real subcommands fall through to argparse.
    argv = sys.argv[1:]
    if argv and not argv[0].startswith("-") and argv[0] not in ("set", "check", "list"):
        cmd_search(argv)
        return
    ap = argparse.ArgumentParser(description="SBS parity registry (Job #1 byte-exact ledger).")
    sub = ap.add_subparsers(dest="cmd")
    s = sub.add_parser("set", help="upsert a unit's parity status")
    s.add_argument("unit")
    for k in FIELDS:
        s.add_argument("--" + k)
    s.set_defaults(func=cmd_set)
    c = sub.add_parser("check", help="gate: exit 1 if any unit diverges")
    c.set_defaults(func=lambda a: sys.exit(cmd_check(a)))
    l = sub.add_parser("list", help="one line per unit")
    l.add_argument("status", nargs="?")
    l.set_defaults(func=cmd_list)
    args = ap.parse_args()
    if args.cmd is None:
        # default: read-only summary + gate. A bare read must NEVER rewrite the doc — save() re-emits
        # only the known FIELDS, so writing here would silently drop any hand-added prose/extra field.
        # Only `set` writes (an explicit mutation); ordering/summary normalize on the next set.
        sys.exit(cmd_check(argparse.Namespace()))
    else:
        args.func(args)


if __name__ == "__main__":
    main()
