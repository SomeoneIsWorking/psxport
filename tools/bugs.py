#!/usr/bin/env python3
# bugs.py — CLI for the port-bug tracker at docs/known-bugs.md.
#
# WHY THIS EXISTS: the port has multiple LIVE bugs (missing renders, wrong SFX, un-owned
# fadeout paths…). Without a single greppable list, every session forgets which symptoms
# were reported and re-discovers them from user reports. This tool is the equivalent of
# tools/findings.py for the "known but not yet root-caused / fixed" tier — findings.py
# is the DISTILLED tier (root cause + fix), this is the LIVE tier (symptom + hypothesis).
#
# WORKFLOW (do this, every session):
#   tools/bugs.py                     # list all open bugs (session start)
#   tools/bugs.py <words>             # search: "have we seen this symptom before?"
#   # after landing a plausible fix:
#   #   edit docs/known-bugs.md, flip status to PORTED-BUT-UNVERIFIED
#   # after user confirms the fix:
#   #   move the write-up to docs/findings/<subsystem>.md (see tools/findings.py header)
#   #   set the entry here to FIXED and link the finding
#
# BLOCK FORMAT (each bug is one `## ` block in docs/known-bugs.md):
#   ## BUG-<n>: <short title>
#   - **status:** OPEN | INVESTIGATING | PORTED-BUT-UNVERIFIED | FIXED
#   - **area:** render | audio | physics | scene | boot | tooling | …
#   - **symptom:** grep-friendly one-line description (words the user would say)
#   - **hypothesis:** current best guess + suggested next step
#   - **refs:** commits, journal entries, files, findings links
#
# The parser is intentionally simple — any `**field:**` line matches, indentation/order
# don't matter. Keep the block terse; long RE goes in a docs/findings/*.md write-up.
import os, re, sys, glob

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUGS_MD = os.path.join(ROOT, "docs", "known-bugs.md")
FIELDS = ("status", "area", "symptom", "hypothesis", "refs")

STATUS_COLORS = {
    "OPEN":                   "\033[31m",  # red
    "INVESTIGATING":          "\033[33m",  # yellow
    "PORTED-BUT-UNVERIFIED":  "\033[36m",  # cyan
    "FIXED":                  "\033[32m",  # green
}
RESET = "\033[0m"

def isatty(): return sys.stdout.isatty()

def parse(path):
    """Yield (id_title, {field: value}) for each `## ` block in the file."""
    if not os.path.exists(path):
        return
    with open(path) as f:
        text = f.read()
    blocks = re.split(r"(?m)^## +", text)
    for b in blocks[1:]:
        lines = b.splitlines()
        title = lines[0].strip()
        fields = {}
        for k in FIELDS:
            m = re.search(r"(?im)^\s*[-*]?\s*\*\*%s:\*\*\s*(.+?)\s*$" % k, b)
            if m: fields[k] = m.group(1).strip()
        yield title, fields

def wrap_status(s):
    if not isatty(): return s
    return STATUS_COLORS.get(s, "") + s + RESET

def matches(fields, needles):
    if not needles: return True
    hay = " ".join([fields.get(k, "") for k in FIELDS]).lower()
    return all(n.lower() in hay for n in needles)

def main(argv):
    if argv and argv[0] in ("-h", "--help", "help"):
        print(__doc__.strip("\n"))
        return
    needles = argv
    bugs = list(parse(BUGS_MD))
    if not bugs:
        print(f"no bugs at {os.path.relpath(BUGS_MD, ROOT)} (or file missing)")
        return
    counts = {}
    hits = []
    for title, fields in bugs:
        if not fields: continue   # section header, not a bug block
        if matches(fields, needles):
            hits.append((title, fields))
            counts[fields.get("status", "?")] = counts.get(fields.get("status", "?"), 0) + 1
    if not hits:
        print(f"no bugs matching: {' '.join(needles)}")
        return
    for title, fields in hits:
        st = fields.get("status", "?")
        area = fields.get("area", "?")
        print(f"[{wrap_status(st)}] ({area}) {title}")
        for k in ("symptom", "hypothesis", "refs"):
            v = fields.get(k)
            if v: print(f"    {k}: {v}")
        print()
    tot = f" ({sum(counts.values())} of {len(bugs)} match)" if needles else ""
    summary = " · ".join(f"{wrap_status(s)}={c}" for s, c in sorted(counts.items()))
    print(f"summary: {summary}{tot}")

if __name__ == "__main__":
    main(sys.argv[1:])
