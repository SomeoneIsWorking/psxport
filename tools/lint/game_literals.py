#!/usr/bin/env python3
"""game_literals.py — the framework-agnosticism ratchet.

WHAT IT GATES.  psxport is a game-agnostic framework; a hardcoded GUEST address in framework code is
a fact about ONE game's executable compiled into the library every port links.  `psxport_smoke`
cannot see this class of leak (a byte-faithful transcription of another game's functions contains no
game SYMBOLS), which is exactly why this lint exists.  Spec: docs/plans/game-seam-redesign.md §7.

HOW IT GATES.  RATCHET, not big-bang.  `tools/lint/game_literals_baseline.txt` records the flagged
(file, value) -> count that exists TODAY.  The gate fails when a count GROWS or a new entry appears,
and also when a count SHRINKS without the baseline being regenerated — so burning a literal down is
a reviewed edit to a tracked file rather than a silent number nobody notices.

THE NEGATIVE IS DESIGNED FIRST.  Every verdict carries its denominator (files scanned, hex literals
seen, candidates classified, flagged, baseline size).  If the scan finds NO files — wrong root, empty
glob, a moved directory — it REFUSES with exit 2 and says it scanned nothing.  A lint that greens
over an empty file list is worse than no lint.

LIVE vs COMMENT.  Only LIVE code (comments and string/char literals stripped) is gated: a comment
naming a guest address is cleanup debt, not an executable lie (§7 mechanic 1).  But the comment/
string occurrences are COUNTED AND REPORTED SEPARATELY, because a raw grep count mixes the two and
sizing future work off the mixed number is how this project has mis-sized work before.

Exit codes: 0 = pass · 1 = gate failure (new/grown/shrunk-unrecorded literal, or a selftest failure)
· 2 = REFUSAL (nothing scanned / unreadable or malformed baseline) — never confused with "clean".
"""

from __future__ import annotations

import argparse
import os
import re
import sys
from collections import defaultdict
from dataclasses import dataclass, field

# ---------------------------------------------------------------------------------------------
# Scope (§7): framework code only.
# ---------------------------------------------------------------------------------------------

SCAN_ROOTS = ("runtime", "tools/recomp")

C_LIKE_EXTS = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".inc", ".frag", ".vert"}
PY_EXTS = {".py"}
SCANNED_EXTS = C_LIKE_EXTS | PY_EXTS

# Directory components never scanned. `generated/` is the recompiled guest itself, `vendor/` is not
# ours, `tests/` deliberately uses synthetic guest addresses to exercise the substrate.
SKIP_DIR_COMPONENTS = {"generated", "vendor", "tests", "build", "scratch", ".git"}

# A test file is out of scope wherever it lives, for the same reason tests/ is: its guest addresses
# are fixtures. The count of files excluded this way is REPORTED, so the exemption is visible.
TEST_FILE_RE = re.compile(r"(^test_.*|.*_test)\.[^.]+$")

# The inline exception. Must be followed by a reason that states the CONSOLE fact.
CONSOLE_MARKER = "psx-console:"

# ---------------------------------------------------------------------------------------------
# Classification (§7 mechanic 2). Ranges are inclusive.
# ---------------------------------------------------------------------------------------------

ALLOW_EXACT = {
    0x80000000, 0xA0000000,  # KSEG0 / KSEG1 segment bases
    0x1FFFFFFF, 0x001FFFFF, 0x001FFFFC,  # segment mask, RAM mask, word-aligned RAM mask
    0x00200000, 0x80200000,  # 2 MiB RAM size / top of KSEG0 RAM
    0xFFFFFFFF, 0xFF000000, 0xDEAD0000,  # enumerated sentinels — a pattern would allow too much
}

ALLOW_RANGES = (
    (0x1F801000, 0x1F802FFF, "hardware I/O registers"),
    (0x1F800000, 0x1F800000, "scratchpad region base"),
    (0x1F800400, 0x1F800400, "scratchpad region end"),
    (0x1FC00000, 0x1FC7FFFF, "BIOS ROM (physical)"),
    (0xBFC00000, 0xBFC7FFFF, "BIOS ROM (KSEG1)"),
    (0x00000000, 0x0000FFFF, "kernel/BIOS RAM (vectors, A0/B0/C0 tables, HLE work area)"),
    (0x80000000, 0x8000FFFF, "kernel/BIOS RAM in KSEG0"),
)

FLAG_RANGES = (
    (0x80010000, 0x801FFFFF, "guest RAM, KSEG0 — a game's code/data address"),
    (0xA0010000, 0xA01FFFFF, "guest RAM, KSEG1 — a game's code/data address"),
    (0x1F800001, 0x1F8003FF, "scratchpad INTERIOR — a game's allocation of scratchpad"),
)


def classify(v: int) -> tuple[bool, str]:
    """-> (flagged, reason). Anything neither allowed nor in a flag range is UNCLASSIFIED (not
    flagged): unsegmented physical addresses and instruction encodings live there, and flagging them
    would drown the baseline in size/stride false positives (§7)."""
    if v in ALLOW_EXACT:
        return False, "console constant (exact allow set)"
    for lo, hi, why in ALLOW_RANGES:
        if lo <= v <= hi:
            return False, f"console constant ({why})"
    for lo, hi, why in FLAG_RANGES:
        if lo <= v <= hi:
            return True, why
    return False, "unclassified (outside every guest range)"


HEX_RE = re.compile(r"0[xX]([0-9A-Fa-f]+)[uUlL]*")


@dataclass
class Hit:
    path: str
    line: int
    value: int
    reason: str
    live: bool


@dataclass
class Scan:
    files: int = 0
    files_excluded_test: int = 0
    files_excluded_generated: int = 0   # gitignored build artifacts written into the source tree
    git_available: bool = True          # False => the ignored set is unknown; the negative says so
    hex_literals: int = 0          # every 0x... token, any width
    candidates: int = 0            # the 5-8 hex-digit subset that gets classified
    console_suppressed: int = 0    # candidates a `psx-console:` marker exempted
    live_hits: list[Hit] = field(default_factory=list)
    comment_hits: list[Hit] = field(default_factory=list)
    roots_scanned: list[str] = field(default_factory=list)
    roots_missing: list[str] = field(default_factory=list)


# ---------------------------------------------------------------------------------------------
# Comment / string stripping. Returns two same-length-line-indexed texts: live and non-live.
# ---------------------------------------------------------------------------------------------

def split_c_like(src: str) -> tuple[str, str]:
    """Blank out comments and string/char literals to produce LIVE text; the inverse for NON-LIVE.
    Newlines are preserved in both so line numbers stay exact."""
    live: list[str] = []
    dead: list[str] = []
    i, n = 0, len(src)

    def emit(ch: str, is_live: bool) -> None:
        if ch == "\n":
            live.append("\n")
            dead.append("\n")
        elif is_live:
            live.append(ch)
            dead.append(" ")
        else:
            live.append(" ")
            dead.append(ch)

    while i < n:
        c = src[i]
        nxt = src[i + 1] if i + 1 < n else ""
        if c == "/" and nxt == "/":
            while i < n and src[i] != "\n":
                emit(src[i], False)
                i += 1
            continue
        if c == "/" and nxt == "*":
            while i < n and not (src[i] == "*" and i + 1 < n and src[i + 1] == "/"):
                emit(src[i], False)
                i += 1
            for _ in range(2):
                if i < n:
                    emit(src[i], False)
                    i += 1
            continue
        if c in "\"'":
            # A raw string literal R"delim( ... )delim" has no escapes; handle it explicitly so a
            # backslash inside one cannot desynchronise the scanner.
            if c == '"' and i >= 1 and src[i - 1] in "Rr":
                j = src.find("(", i)
                if j != -1:
                    delim = src[i + 1:j]
                    end = src.find(")" + delim + '"', j)
                    stop = n if end == -1 else end + len(delim) + 2
                    while i < stop:
                        emit(src[i], False)
                        i += 1
                    continue
            quote = c
            emit(c, False)
            i += 1
            while i < n and src[i] != quote:
                if src[i] == "\\" and i + 1 < n:
                    emit(src[i], False)
                    i += 1
                    if i < n:
                        emit(src[i], False)
                        i += 1
                    continue
                if src[i] == "\n":       # unterminated literal — do not swallow the rest of the file
                    break
                emit(src[i], False)
                i += 1
            if i < n and src[i] == quote:
                emit(src[i], False)
                i += 1
            continue
        emit(c, True)
        i += 1
    return "".join(live), "".join(dead)


def split_python(src: str) -> tuple[str, str]:
    live: list[str] = []
    dead: list[str] = []
    i, n = 0, len(src)

    def emit(ch: str, is_live: bool) -> None:
        if ch == "\n":
            live.append("\n")
            dead.append("\n")
        elif is_live:
            live.append(ch)
            dead.append(" ")
        else:
            live.append(" ")
            dead.append(ch)

    while i < n:
        c = src[i]
        if c == "#":
            while i < n and src[i] != "\n":
                emit(src[i], False)
                i += 1
            continue
        if c in "\"'":
            triple = src[i:i + 3] if src[i:i + 3] in ('"""', "'''") else None
            if triple:
                for _ in range(3):
                    emit(src[i], False)
                    i += 1
                while i < n and src[i:i + 3] != triple:
                    if src[i] == "\\" and i + 1 < n:
                        emit(src[i], False)
                        i += 1
                    emit(src[i], False)
                    i += 1
                for _ in range(3):
                    if i < n:
                        emit(src[i], False)
                        i += 1
                continue
            quote = c
            emit(c, False)
            i += 1
            while i < n and src[i] != quote:
                if src[i] == "\\" and i + 1 < n:
                    emit(src[i], False)
                    i += 1
                    if i < n:
                        emit(src[i], False)
                        i += 1
                    continue
                if src[i] == "\n":
                    break
                emit(src[i], False)
                i += 1
            if i < n and src[i] == quote:
                emit(src[i], False)
                i += 1
            continue
        emit(c, True)
        i += 1
    return "".join(live), "".join(dead)


def console_marked(raw_line: str) -> bool:
    """True when the source line carries `psx-console: <reason>` with a non-empty reason."""
    k = raw_line.find(CONSOLE_MARKER)
    if k < 0:
        return False
    return bool(raw_line[k + len(CONSOLE_MARKER):].strip())


def scan_text(path: str, src: str, is_python: bool, scan: Scan) -> None:
    live_txt, dead_txt = (split_python(src) if is_python else split_c_like(src))
    raw_lines = src.splitlines()

    for txt, is_live in ((live_txt, True), (dead_txt, False)):
        for m in HEX_RE.finditer(txt):
            # Leading zeros are stripped BEFORE the width test. `0x0801FE070` is a legal C literal
            # equal to a guest address, and measuring the raw digit count let it through as "9 digits,
            # not 32-bit". Found by sabotaging this tool's own selftest; the evasion was real.
            digits = m.group(1).lstrip("0") or "0"
            if is_live:
                scan.hex_literals += 1
            if not (5 <= len(digits) <= 8):
                continue          # nothing shorter can be a guest address; 9+ is not 32-bit
            if is_live:
                scan.candidates += 1
            v = int(digits, 16)
            flagged, reason = classify(v)
            if not flagged:
                continue
            line = txt.count("\n", 0, m.start()) + 1
            raw = raw_lines[line - 1] if line - 1 < len(raw_lines) else ""
            if console_marked(raw):
                if is_live:
                    scan.console_suppressed += 1
                continue
            hit = Hit(path, line, v, reason, is_live)
            (scan.live_hits if is_live else scan.comment_hits).append(hit)


def ignored_paths(repo: str, roots: tuple[str, ...]) -> tuple[set[str], bool]:
    """Repo-relative paths git IGNORES under `roots` -> (paths, git_was_available).

    Why this exists: `runtime/recomp/gpu_vk_shaders.h` is generated by the build into the source tree
    and is gitignored. Scanning it made this tool's own denominator depend on whether you had built
    (202 files / 25216 literals before a build, 203 / 38095 after) — an unstable denominator makes
    every negative it prints unverifiable, and a flag inside a generated file names a line no human
    can edit. Scope is SOURCE, so the ignored set is subtracted."""
    try:
        import subprocess
        r = subprocess.run(
            ["git", "-C", repo, "ls-files", "--others", "--ignored", "--exclude-standard", "-z",
             "--", *roots],
            capture_output=True, text=True, timeout=60)
        if r.returncode != 0:
            return set(), False
        return {p for p in r.stdout.split("\0") if p}, True
    except Exception:
        return set(), False


def scan_tree(repo: str, roots: tuple[str, ...]) -> Scan:
    scan = Scan()
    ignored, scan.git_available = ignored_paths(repo, roots)
    for root in roots:
        abs_root = os.path.join(repo, root)
        if not os.path.isdir(abs_root):
            scan.roots_missing.append(root)
            continue
        scan.roots_scanned.append(root)
        for dirpath, dirnames, filenames in os.walk(abs_root):
            dirnames[:] = [d for d in sorted(dirnames) if d not in SKIP_DIR_COMPONENTS]
            for fn in sorted(filenames):
                ext = os.path.splitext(fn)[1]
                if ext not in SCANNED_EXTS:
                    continue
                if TEST_FILE_RE.match(fn):
                    scan.files_excluded_test += 1
                    continue
                abspath = os.path.join(dirpath, fn)
                rel = os.path.relpath(abspath, repo)
                if rel in ignored:
                    scan.files_excluded_generated += 1
                    continue
                try:
                    with open(abspath, "r", encoding="utf-8", errors="replace") as fh:
                        src = fh.read()
                except OSError as e:
                    print(f"REFUSE: cannot read {rel}: {e}", file=sys.stderr)
                    sys.exit(2)
                scan.files += 1
                scan_text(rel, src, ext in PY_EXTS, scan)
    return scan


# ---------------------------------------------------------------------------------------------
# Baseline
# ---------------------------------------------------------------------------------------------

BASELINE_REL = "tools/lint/game_literals_baseline.txt"


def counts_of(hits: list[Hit]) -> dict[tuple[str, int], int]:
    out: dict[tuple[str, int], int] = defaultdict(int)
    for h in hits:
        out[(h.path, h.value)] += 1
    return dict(out)


def lines_of(hits: list[Hit]) -> dict[tuple[str, int], list[int]]:
    out: dict[tuple[str, int], list[int]] = defaultdict(list)
    for h in hits:
        out[(h.path, h.value)].append(h.line)
    return out


def parse_baseline(path: str) -> dict[tuple[str, int], int]:
    """Malformed or unreadable baseline is a REFUSAL (exit 2), never a pass."""
    if not os.path.exists(path):
        print(f"REFUSE: baseline {path} does not exist. This tool cannot tell a clean tree from an\n"
              f"        unrecorded one. Create it with --write-baseline (reviewed) before gating.",
              file=sys.stderr)
        sys.exit(2)
    out: dict[tuple[str, int], int] = {}
    with open(path, "r", encoding="utf-8") as fh:
        for lineno, raw in enumerate(fh, 1):
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            parts = line.split(":")
            if len(parts) != 3:
                print(f"REFUSE: {path}:{lineno}: expected <path>:<value>:<count>, got {line!r}",
                      file=sys.stderr)
                sys.exit(2)
            try:
                key = (parts[0], int(parts[1], 16))
                cnt = int(parts[2], 10)
            except ValueError:
                print(f"REFUSE: {path}:{lineno}: unparseable value/count in {line!r}", file=sys.stderr)
                sys.exit(2)
            if key in out:
                print(f"REFUSE: {path}:{lineno}: duplicate entry {parts[0]}:{parts[1]}", file=sys.stderr)
                sys.exit(2)
            out[key] = cnt
    return out


def render_baseline(scan: Scan) -> str:
    live = counts_of(scan.live_hits)
    lines = lines_of(scan.live_hits)
    comment = counts_of(scan.comment_hits)
    per_file_live: dict[str, int] = defaultdict(int)
    per_file_comment: dict[str, int] = defaultdict(int)
    for (p, _), c in live.items():
        per_file_live[p] += c
    for (p, _), c in comment.items():
        per_file_comment[p] += c

    out: list[str] = []
    out.append("# psxport game-literal RATCHET BASELINE — generated by tools/lint/game_literals.py")
    out.append("#")
    out.append("# Each gated line is `<relative path>:<guest value>:<count of LIVE occurrences>`.")
    out.append("# The key is (file, value) and NOT a line number, so unrelated edits do not churn it.")
    out.append("# The trailing `# L…` is indicative context for retiring the entry — not part of the key.")
    out.append("#")
    out.append("# THE GATE: a count may only go DOWN, and going down requires regenerating this file")
    out.append("# (`--write-baseline`) in the same reviewed change. Growth is impossible without an edit here.")
    out.append("#")
    out.append(f"# DENOMINATOR at generation: {scan.files} files scanned across roots "
               f"{', '.join(scan.roots_scanned)}; {scan.hex_literals} hex literals in live code; "
               f"{scan.candidates} were 5-8 digit address candidates; "
               f"{len(scan.live_hits)} live flagged in {len(per_file_live)} files; "
               f"{len(scan.comment_hits)} more in comments/strings (NOT gated, listed at the bottom); "
               f"{scan.console_suppressed} exempted by an inline `psx-console:` marker; "
               f"{scan.files_excluded_test} test files and "
               f"{scan.files_excluded_generated} gitignored build artifacts excluded"
               + ("." if scan.git_available else
                  " — WARNING: git was unavailable, so generated files may be INCLUDED and this "
                  "denominator is not reproducible."))
    out.append("#")
    out.append("# Top holders by LIVE count (comment/string count in parentheses):")
    for p, c in sorted(per_file_live.items(), key=lambda kv: (-kv[1], kv[0]))[:15]:
        out.append(f"#   {c:5d} ({per_file_comment.get(p, 0):4d})  {p}")
    out.append("")
    for (p, v) in sorted(live.keys()):
        ls = sorted(lines[(p, v)])
        shown = ",".join(f"L{n}" for n in ls[:3]) + (",…" if len(ls) > 3 else "")
        out.append(f"{p}:0x{v:08X}:{live[(p, v)]}  # {shown}")
    out.append("")
    out.append("# ---------------------------------------------------------------------------------")
    out.append("# NOT GATED — the same values in comments and string literals. Cleanup debt, recorded")
    out.append("# here so nobody sizes the seam work off a grep count that mixes the two classes.")
    for p, c in sorted(per_file_comment.items(), key=lambda kv: (-kv[1], kv[0])):
        out.append(f"#   {c:5d}  {p}")
    return "\n".join(out) + "\n"


# ---------------------------------------------------------------------------------------------
# Selftest — both classes, and it must fail if the detector is sabotaged.
# ---------------------------------------------------------------------------------------------

POSITIVE_SAMPLES = [
    ("guest KSEG0 code address", "c", "  rc2(c, 0x80051FA4, 0);\n"),
    ("guest KSEG0 data address", "c", "  int v = mem_r32(0x801FE070);\n"),
    ("guest KSEG1 alias", "c", "  uint32_t p = 0xA0108F60u;\n"),
    ("scratchpad INTERIOR (a game's layout of a console region)", "c",
     "  static const uint32_t kCurTaskPtr = 0x1F800138u;\n"),
    ("guest address in python framework tooling", "py", "TARGET = 0x8002A338\n"),
    ("guest address whose psx-console marker has NO reason", "c",
     "  rc2(c, 0x80051FA4, 0); // psx-console:\n"),
    # Regression guard for a real evasion the sabotage pass found: a zero-padded literal is the same
    # 32-bit address, and the width test used to read it as "9 digits, not an address".
    ("zero-padded guest address (0x0801FE070 == 0x801FE070)", "c",
     "  int v = mem_r32(0x0801FE070);\n"),
]

NEGATIVE_SAMPLES = [
    ("KSEG0 segment base", "c", "  uint32_t k = 0x80000000u;\n"),
    ("segment mask", "c", "  a &= 0x1FFFFFFF;\n"),
    ("RAM mask", "c", "  a &= 0x001FFFFF;\n"),
    ("hardware I/O register (GPU GP1)", "c", "  w32(0x1F801814, cmd);\n"),
    ("scratchpad region BOUNDS", "c", "  if (a >= 0x1F800000 && a < 0x1F800400) return true;\n"),
    ("BIOS ROM base", "c", "  if (a >= 0xBFC00000) return bios(a);\n"),
    ("kernel/BIOS RAM work area", "c", "  uint32_t t = 0x0000A0A0;\n"),
    ("sentinel", "c", "  return 0xDEAD0000;\n"),
    ("guest address inside a // comment", "c", "  // was 0x80051FA4 before the move\n"),
    ("guest address inside a block comment", "c", "  /* Tomba's 0x801FE070 task word */\n"),
    ("guest address inside a string literal", "c", "  log(\"entry 0x801062E4\");\n"),
    ("guest address in a python docstring", "py", '"""Spider-Man 0x8002A338 is the case."""\n'),
    ("guest address with a REASONED psx-console marker", "c",
     "  w32(0x801FE070, 0); // psx-console: not real, only here to prove the marker works\n"),
    ("4-digit hex — structurally not an address", "c", "  a += 0x0070;\n"),
    ("64-bit-width hex — not a 32-bit address", "c", "  uint64_t m = 0x80051FA4DEAD;\n"),
    ("MIPS instruction encoding", "c", "  emit(0x27BD8000);\n"),
]


def selftest() -> int:
    fails: list[str] = []
    ran = 0
    for label, kind, src in POSITIVE_SAMPLES:
        ran += 1
        s = Scan()
        scan_text(f"<positive:{label}>", src, kind == "py", s)
        if len(s.live_hits) != 1:
            fails.append(f"POSITIVE MISSED [{label}]: expected 1 live flag, got {len(s.live_hits)} "
                         f"— source: {src.strip()!r}")
    for label, kind, src in NEGATIVE_SAMPLES:
        ran += 1
        s = Scan()
        scan_text(f"<negative:{label}>", src, kind == "py", s)
        if s.live_hits:
            fails.append(f"FALSE POSITIVE [{label}]: {len(s.live_hits)} live flag(s) "
                         f"— source: {src.strip()!r}")

    # A structural assertion, not a sample: the classifier must actually DISCRIMINATE. If someone
    # neuters classify() into "never flag" or "always flag", the sample loops above catch it — this
    # catches the subtler sabotage of collapsing the boundary between adjacent ranges.
    ran += 1
    boundary = [
        (0x8000FFFF, False), (0x80010000, True), (0x801FFFFF, True), (0x80200000, False),
        (0x1F800000, False), (0x1F800001, True), (0x1F8003FF, True), (0x1F800400, False),
        (0xA000FFFF, False), (0xA0010000, True), (0xA01FFFFF, True), (0xA0200000, False),
    ]
    bad = [f"0x{v:08X} expected flagged={want} got {classify(v)[0]}"
           for v, want in boundary if classify(v)[0] != want]
    if bad:
        fails.append("BOUNDARY: " + "; ".join(bad))

    # And the comment stripper must be a stripper, not a no-op: identical source, one live and one
    # commented, must land in different buckets.
    ran += 1
    s_live, s_cmt = Scan(), Scan()
    scan_text("<split-live>", "x = 0x801FE070;\n", False, s_live)
    scan_text("<split-comment>", "// x = 0x801FE070;\n", False, s_cmt)
    if not (len(s_live.live_hits) == 1 and not s_live.comment_hits
            and not s_cmt.live_hits and len(s_cmt.comment_hits) == 1):
        fails.append(f"SPLIT: live/comment separation is not working "
                     f"(live={len(s_live.live_hits)}/{len(s_live.comment_hits)}, "
                     f"comment={len(s_cmt.live_hits)}/{len(s_cmt.comment_hits)})")

    # The DENOMINATOR is gated too, not just the verdict. Sabotaging the 5-8-digit width rule turned
    # out to change no verdict at all (every flag range needs >=7 significant digits and no 9-digit
    # value lands in one), so a verdict-only selftest certified a broken width rule as fine. What the
    # width rule really governs is the candidate COUNT the negative reports — so assert that.
    ran += 1
    s_den = Scan()
    scan_text("<denominator>", "  a=0x0070; b=0x801FE070; c=0x1F801814; d=0x80051FA4DEAD;\n", False, s_den)
    if not (s_den.hex_literals == 4 and s_den.candidates == 2):
        fails.append(f"DENOMINATOR: expected 4 hex literals / 2 address candidates from the mixed-width "
                     f"sample, got {s_den.hex_literals} / {s_den.candidates} — the 5-8-digit candidate "
                     f"rule is wrong, so every 'scanned N' figure this tool prints is wrong.")

    # Precedence, stated so it cannot be mistaken for a hole: ALLOW is evaluated BEFORE FLAG, so
    # widening a FLAG range down into console territory is INERT — it cannot change a verdict. What
    # can change a verdict is widening an ALLOW range up into guest territory, and the boundary sweep
    # above catches that at 0x80010000 / 0x1F800001 / 0xA0010000.

    print(f"selftest: {ran} checks — {len(POSITIVE_SAMPLES)} must-flag samples, "
          f"{len(NEGATIVE_SAMPLES)} must-pass samples, 1 range-boundary sweep "
          f"({len(boundary)} boundary values), 1 live/comment split check, 1 denominator check.")
    if fails:
        print(f"selftest: FAIL — {len(fails)} of {ran} checks failed:")
        for f in fails:
            print(f"  {f}")
        return 1
    print("selftest: PASS — the detector flags every positive class and no negative class.")
    return 0


# ---------------------------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------------------------

def default_repo() -> str:
    return os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--repo", default=default_repo(), help="psxport repo root (default: this file's ../..)")
    ap.add_argument("--roots", nargs="*", default=list(SCAN_ROOTS), help="scan roots, repo-relative")
    ap.add_argument("--baseline", default=None, help=f"baseline file (default: <repo>/{BASELINE_REL})")
    ap.add_argument("--write-baseline", action="store_true", help="regenerate the baseline")
    ap.add_argument("--grow", action="store_true",
                    help="permit --write-baseline to record MORE literals than the current baseline")
    ap.add_argument("--selftest", action="store_true", help="run the detector's own both-class selftest")
    args = ap.parse_args(argv)

    if args.selftest:
        return selftest()

    baseline_path = args.baseline or os.path.join(args.repo, BASELINE_REL)
    scan = scan_tree(args.repo, tuple(args.roots))

    # The refusal, designed before the pass: a scan of nothing is not a clean tree.
    if scan.files == 0:
        print(f"REFUSE: scanned NOTHING — 0 files under repo {args.repo!r} roots "
              f"{args.roots} (missing roots: {scan.roots_missing or 'none'}; "
              f"{scan.files_excluded_test} test files excluded; extensions {sorted(SCANNED_EXTS)}). "
              f"This is NOT a pass: the tool cannot see the framework.", file=sys.stderr)
        return 2
    if scan.roots_missing:
        print(f"REFUSE: scan root(s) {scan.roots_missing} do not exist under {args.repo!r}. "
              f"A partial scan cannot certify the framework.", file=sys.stderr)
        return 2

    live = counts_of(scan.live_hits)
    lines = lines_of(scan.live_hits)

    if args.write_baseline:
        old_total = 0
        if os.path.exists(baseline_path):
            old_total = sum(parse_baseline(baseline_path).values())
        new_total = sum(live.values())
        if old_total and new_total > old_total and not args.grow:
            print(f"REFUSE to write a LARGER baseline: {new_total} live literals vs {old_total} "
                  f"recorded. The ratchet only turns one way. Remove the new literals, or pass "
                  f"--grow deliberately (ctest and the hook never do).", file=sys.stderr)
            return 2
        os.makedirs(os.path.dirname(baseline_path), exist_ok=True)
        with open(baseline_path, "w", encoding="utf-8") as fh:
            fh.write(render_baseline(scan))
        print(f"wrote {baseline_path}: {len(live)} entries / {new_total} live literals "
              f"(was {old_total}) · scanned {scan.files} files, {scan.candidates} address candidates, "
              f"{len(scan.comment_hits)} comment/string occurrences recorded but not gated")
        return 0

    base = parse_baseline(baseline_path)

    new_entries, grown, shrunk = [], [], []
    for key, cnt in sorted(live.items()):
        b = base.get(key)
        if b is None:
            new_entries.append((key, cnt))
        elif cnt > b:
            grown.append((key, cnt, b))
    for key, b in sorted(base.items()):
        cnt = live.get(key, 0)
        if cnt < b:
            shrunk.append((key, cnt, b))

    total_live = sum(live.values())
    print(f"game_literals: scanned {scan.files} files under {', '.join(scan.roots_scanned)} "
          f"({scan.files_excluded_test} test files, {scan.files_excluded_generated} gitignored build "
          f"artifacts excluded{'' if scan.git_available else '; GIT UNAVAILABLE — generated files may be INCLUDED'})"
          f" / {scan.hex_literals} hex literals in "
          f"live code / {scan.candidates} address candidates classified / {total_live} flagged live "
          f"in {len({p for p, _ in live})} files / baseline covers {sum(base.values())} in "
          f"{len(base)} entries / {len(scan.comment_hits)} more in comments+strings (not gated) / "
          f"{scan.console_suppressed} exempted by `psx-console:`")

    if not (new_entries or grown or shrunk):
        print("game_literals: PASS — no new guest literal in framework code, no count grew, "
              "and the baseline matches the tree exactly.")
        return 0

    print("game_literals: FAIL")
    for (p, v), cnt in new_entries:
        for ln in sorted(lines[(p, v)]):
            print(f"  {p}:{ln}: NEW guest literal 0x{v:08X} — {classify(v)[1]}")
        print(f"    -> {p}: 0x{v:08X} x{cnt} is not in the baseline.")
    for (p, v), cnt, b in grown:
        for ln in sorted(lines[(p, v)]):
            print(f"  {p}:{ln}: guest literal 0x{v:08X} — {classify(v)[1]}")
        print(f"    -> {p}: 0x{v:08X} count GREW {b} -> {cnt}.")
    for (p, v), cnt, b in shrunk:
        print(f"  {p}: 0x{v:08X} count SHRANK {b} -> {cnt} — good, but the baseline must record it.")
    print("\n  Two legal remedies, and only two:")
    print("   1. Move the fact to where it belongs: a GameConfig scalar the framework reads via")
    print("      `cfg->`, game code behind an abstract class, or a diagnostic ROW the game registers")
    print("      at init (docs/plans/game-seam-redesign.md §2 decides which, by three questions).")
    print("   2. If it is genuinely a CONSOLE constant this classifier cannot know, annotate the line")
    print("      `// psx-console: <the console fact>` — the reason must state the fact, not that you")
    print("      need the number here.")
    print(f"   For a deliberate BURN-DOWN (a count went down), regenerate the ratchet:")
    print(f"      python3 tools/lint/game_literals.py --write-baseline")
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
