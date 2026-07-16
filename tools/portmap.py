#!/usr/bin/env python3
# portmap.py — the RE + PORT FRONTIER registry (the real-vs-hack ledger).
#
# WHY THIS EXISTS: a faithful reimplementation is an ORDERED dependency chain of RE steps. The cardinal
# sin (the one that costs months) is faking a step's output before its RE is done — a magic offset, an
# empirical constant, a native overlay standing in for a real mechanism, a "stopgap". Such a hack makes a
# broken port LOOK finished and BLOCKS the real work behind it. docs/port-progress.md is the exhaustive
# boot->gameplay spine; THIS tool is the focused, queryable frontier: for each RE+port step, is it REAL
# (re-verified from the binary/asset) or a HACK (debt that MUST be removed as the real mechanism lands),
# and what is the next RE-ready step. It is the second axis of the tracking stack:
#     codemap.py   = WHERE the code is (guest addr -> native owner)
#     portmap.py   = IS it ported, and REAL not a hack  <-- this tool
#     parity.py    = IS it SBS byte-exact
#     behavior.py  = WHAT it deliberately changes (sanctioned pc_render/skip/enh divergences)
#
# THE LOOP:
#   tools/portmap.py next                 # what's the next RE-ready step? work THAT, not a downstream one.
#   tools/portmap.py hacks                # the debt list — every hack/stopgap that must be removed. Keep it shrinking.
#   ... RE + port a step ...
#   tools/portmap.py set <id> --status verified --owner <file> --notes '...'
#   tools/portmap.py check                # flags hacks + steps whose deps aren't verified (jumped-ahead work)
#
# STATUS values (the core axis):
#   verified          — RE'd from ground truth AND proven on real data (parity 0-diff, or USER-eyeballed
#                       render). The only "done".
#   ported-unverified — code exists, plausibly RE'd, but NOT yet gated (needs parity.py / eyeball).
#   hack              — ⛔ DEBT: an empirical constant / stopgap / overlay standing in for the real
#                       mechanism. MUST cite in `notes` what the real fix is + its death condition.
#   todo              — RE not started.
#   blocked           — waiting on a `deps` step that isn't verified yet.
#
# ENTRY BLOCK FORMAT in docs/port-map.md (one `## ` per step; hand-editable OR via `set`):
#   ## <id — short title>
#   - **scope:** guest addrs / what this step covers
#   - **status:** verified | ported-unverified | hack | todo | blocked
#   - **order:** <int>        (frontier order — lower = more upstream; `next` walks ascending)
#   - **deps:** <id>, <id>    (steps that must be `verified` before this is RE-ready)
#   - **owner:** game/.../file.cpp   (or "-" if unported)
#   - **notes:** for a hack: the REAL fix + its death condition. else: RE refs / what's left.
import os, re, sys, argparse

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DOC = os.path.join(ROOT, "docs", "port-map.md")
FIELDS = ("scope", "status", "order", "deps", "owner", "notes")
STATUSES = ("verified", "ported-unverified", "hack", "todo", "blocked")
HEADER = "# RE + port frontier — is each step REAL (re-verified) or a HACK? (managed by tools/portmap.py)\n"


def parse(text):
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


def _order(f):
    try:
        return int(f.get("order", "999"))
    except ValueError:
        return 999


def render(entries):
    rank = {s: i for i, s in enumerate(STATUSES)}
    entries = sorted(entries, key=lambda e: (_order(e[1]), rank.get(e[1].get("status", "todo"), 9)))
    out = [HEADER,
           "The RE dependency chain. `## ` block per step. Work `portmap.py next`; kill `portmap.py hacks`.",
           "Detail lives in docs/port-progress.md; this is the queryable real-vs-hack frontier.",
           ""]
    counts = {}
    for _, f in entries:
        counts[f.get("status", "todo")] = counts.get(f.get("status", "todo"), 0) + 1
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
        if unit.lower() == args.id.lower():
            f.update(d)
            entries[i] = (unit, f)
            break
    else:
        entries.append((args.id, d))
    save(entries)
    print(f"[portmap] set {args.id}: " + ", ".join(f"{k}={v}" for k, v in d.items()))


def cmd_next(args):
    entries = load()
    verified = {u.lower() for u, f in entries if f.get("status") == "verified"}
    actionable = []
    for u, f in entries:
        if f.get("status") in ("verified",):
            continue
        deps = [d.strip().lower() for d in f.get("deps", "").split(",") if d.strip()]
        ready = all(d in verified for d in deps)
        if ready and f.get("status") in ("todo", "ported-unverified", "hack", "blocked", None):
            actionable.append((u, f))
    actionable.sort(key=lambda e: _order(e[1]))
    if not actionable:
        print("(no RE-ready step — all verified, or every open step is blocked on an unverified dep)")
        return
    print("NEXT RE-ready steps (deps verified), most upstream first:")
    for u, f in actionable[:8]:
        print(f"  [{f.get('status','todo'):17}] order {f.get('order','?'):>3}  {u}")
        if f.get("notes"):
            print(f"       {f['notes']}")


def cmd_hacks(args):
    hacks = [(u, f) for u, f in load() if f.get("status") == "hack"]
    hacks.sort(key=lambda e: _order(e[1]))
    if not hacks:
        print("[portmap] 0 hacks — no known standing debt. (Keep it that way.)")
        return
    print(f"⛔ {len(hacks)} HACK(S) — debt to remove (each must name its real fix + death condition):")
    for u, f in hacks:
        print(f"  {u}\n     scope: {f.get('scope','?')}\n     real fix: {f.get('notes','(UNSPECIFIED — fill it in!)')}")


def cmd_check(args):
    entries = load()
    verified = {u.lower() for u, f in entries if f.get("status") == "verified"}
    hacks = [u for u, f in entries if f.get("status") == "hack"]
    jumped = []  # non-todo work whose deps aren't verified — work built ahead of its RE
    for u, f in entries:
        if f.get("status") in ("verified", "ported-unverified", "hack"):
            deps = [d.strip().lower() for d in f.get("deps", "").split(",") if d.strip()]
            missing = [d for d in deps if d not in verified]
            if missing:
                jumped.append((u, missing))
    hack_no_fix = [u for u, f in entries if f.get("status") == "hack" and not f.get("notes")]
    for u, m in jumped:
        print(f"  ⚠ jumped-ahead: {u} — deps not verified: {', '.join(m)}")
    for u in hack_no_fix:
        print(f"  ⚠ hack without a real-fix/death-condition note: {u}")
    for u in hacks:
        print(f"  ⛔ hack (debt): {u}")
    print(f"[portmap] {len(entries)} steps — {len(hacks)} hacks, {len(jumped)} jumped-ahead.")
    return 1 if hack_no_fix else 0


def cmd_list(args):
    for u, f in sorted(load(), key=lambda e: (_order(e[1]), e[0].lower())):
        if args.status and f.get("status") != args.status:
            continue
        print(f"{f.get('status','?'):17} order {f.get('order','?'):>3}  {u:45} {f.get('owner','')}")


def cmd_search(words):
    ws = [w.lower() for w in words]
    hits = [(u, f) for u, f in load() if all(w in (u + " " + " ".join(f.values())).lower() for w in ws)]
    for u, f in hits:
        print(f"## {u}")
        for k in FIELDS:
            if k in f:
                print(f"   {k}: {f[k]}")
        print()
    if not hits:
        print("(no port-map entries match; add one with `portmap.py set <id> ...`)")


def main():
    # Bare-word search: `portmap.py <words>`. argparse subparsers reject an unknown first positional as
    # an "invalid choice" before any fallback runs, so intercept it here — a first arg that is neither a
    # known subcommand nor a flag means "search". Flags / real subcommands fall through to argparse.
    argv = sys.argv[1:]
    if argv and not argv[0].startswith("-") and argv[0] not in ("set", "next", "hacks", "check", "list"):
        cmd_search(argv)
        return
    ap = argparse.ArgumentParser(description="RE+port frontier registry (real-vs-hack ledger).")
    sub = ap.add_subparsers(dest="cmd")
    s = sub.add_parser("set", help="upsert a step")
    s.add_argument("id")
    for k in FIELDS:
        s.add_argument("--" + k)
    s.set_defaults(func=cmd_set)
    sub.add_parser("next", help="next RE-ready step(s)").set_defaults(func=cmd_next)
    sub.add_parser("hacks", help="the debt list").set_defaults(func=cmd_hacks)
    sub.add_parser("check", help="flag hacks + jumped-ahead work").set_defaults(func=lambda a: sys.exit(cmd_check(a)))
    l = sub.add_parser("list", help="one line per step")
    l.add_argument("status", nargs="?")
    l.set_defaults(func=cmd_list)
    args = ap.parse_args()
    if args.cmd is None:
        # default: read-only — next RE-ready step(s). A bare read must NEVER rewrite the doc: save()
        # re-emits only the known FIELDS, so writing here would silently drop hand-added prose. Only
        # `set` writes (an explicit mutation); ordering/summary normalize on the next set.
        cmd_next(argparse.Namespace())
    else:
        args.func(args)


if __name__ == "__main__":
    main()
