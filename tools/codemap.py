#!/usr/bin/env python3
"""codemap.py — index the PC-native engine's reverse-engineering ownership by GUEST ADDRESS.

WHY THIS EXISTS (read this first): the native port has ~350+ hand-written reimplementations of
specific Tomba!2 functions, scattered across engine/*.cpp and runtime/recomp/*.{c,cpp}, each tied
to a guest MIPS address ONLY by its symbol name (`ov_800753D4`) or a header comment (`// 0x800753D4
— ...`). There is no other index. Worse, the OLD override system that wired address->native was
REMOVED (top-down PC-driven now), so the great majority of these natives are currently ORPHANED:
real, correct code that nothing calls. Without a map you cannot answer the one question that saves
hours — "is FUN_XXXX already owned natively, and where?" — so you re-derive code that already exists
(this tool was written after exactly that happened: native_cb_loadidx duplicated ov_load_texgroup).

WHAT IT DOES: scans the native sources, extracts for each native function:
  - the GUEST ADDRESS(es) it implements (from the `ov_<hex>` name and/or the header comment),
  - its file:line and symbol,
  - the addresses it DEPENDS on (rec_dispatch / call_fn / rc1..rc4 / super_call targets — i.e.
    still-PSX leaves it calls; these are the things that BREAK now overrides are gone),
  - which other native symbols call it directly (C call graph),
  - a reachability verdict: LIVE (transitively C-called from a native_boot dispatch root) vs ORPHAN.

USAGE:
  tools/codemap.py                      # write docs/code-map.md (the committed index)
  tools/codemap.py --addr 800753d4      # look up one address: who implements it + who depends on it
  tools/codemap.py --orphans            # list owned addresses whose native is currently ORPHANED
  tools/codemap.py --stdout             # print the full markdown to stdout instead of writing the file
"""
import os, re, sys, glob

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC_GLOBS = ["engine/**/*.cpp", "engine/**/*.h", "game/**/*.cpp", "game/**/*.h",
             "runtime/recomp/**/*.cpp", "runtime/recomp/**/*.c"]
# Native-dispatch ROOTS: symbols native_boot.cpp calls directly (top-down) to enter native code.
# Everything reachable from these by direct C call is LIVE; the rest is ORPHANED (was override-only).
ROOTS = {"ov_game_stage_main", "ov_start_bin_stage", "native_task0_bootstrap",
         "ov_game_main", "native_boot_run"}

DEF_RE  = re.compile(r'^\s*(?:static\s+)?(?:inline\s+)?void\s+((?:ov_|native_|eng_)\w+)\s*\(\s*Core\s*\*')
# PC-game-structure natives are C++ CLASS METHODS (e.g. `void Camera::lookAt()`), which take no Core*
# param (they hold it as a member). Index those too; the owned guest FUN_/addr is read from a trailing
# `// FUN_xxxx` on the def line or the comment block above (same association logic as free functions).
METHOD_RE = re.compile(r'^\s*(?:static\s+)?(?:inline\s+)?[\w:*&<>]+\s+(\w+::\w+)\s*\(')
ADDR_RE = re.compile(r'0x(8[0-9A-Fa-f]{7})')
FUN_RE  = re.compile(r'FUN_(8[0-9a-fA-F]{7})')
NAMEHEX = re.compile(r'^(?:ov|native|eng)_([0-9A-Fa-f]{6,8})$')
DEP_RE  = re.compile(r'(?:rec_dispatch|rec_super_call|super_call|call_fn|rc[0-4]|rec_coro_redirect)\s*\(\s*c\s*,\s*0x(8[0-9A-Fa-f]{7})')


def load_override_table():
    """Authoritative addr->symbol map recovered from the pre-removal override registrations
    (tools/codemap_overrides.tsv, snapshotted from git faeb436^). Ground truth for any native
    that predates the override removal; new top-down natives fall back to the name/comment heuristic."""
    sym2addrs, path = {}, os.path.join(ROOT, "tools/codemap_overrides.tsv")
    if os.path.exists(path):
        for ln in open(path):
            parts = ln.rstrip("\n").split("\t")
            if len(parts) == 2 and parts[0]:
                sym2addrs.setdefault(parts[1], []).append(parts[0].upper())
    return sym2addrs


OVR = load_override_table()


def collect_files():
    files = []
    for g in SRC_GLOBS:
        files += glob.glob(os.path.join(ROOT, g), recursive=True)
    return sorted(set(files))


def parse_file(path, natives):
    rel = os.path.relpath(path, ROOT)
    lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
    i = 0
    while i < len(lines):
        m = DEF_RE.match(lines[i])
        is_method = False
        if not m:
            m = METHOD_RE.match(lines[i])
            is_method = True
        if not m:
            i += 1
            continue
        sym = m.group(1)
        # gather the contiguous comment block immediately above the def
        c = i - 1
        comment = []
        while c >= 0 and lines[c].lstrip().startswith("//"):
            comment.append(lines[c].strip()); c -= 1
        comment.reverse()
        # a trailing `// ...` on the def line itself: class methods tag their guest FUN address there
        # (e.g. `void Camera::lookAt() {   // FUN_8006D02C`). Scan it first so it wins the association.
        defcomment = lines[i][lines[i].index("//"):].strip() if "//" in lines[i] else ""
        # gather the body until brace balance returns to 0 (handles one-liners and multiline)
        body, depth, started = [], 0, False
        j = i
        while j < len(lines):
            body.append(lines[j])
            depth += lines[j].count("{") - lines[j].count("}")
            if "{" in lines[j]:
                started = True
            if started and depth <= 0:
                break
            j += 1
        bodytext = "\n".join(body)

        # implemented address(es). The override table is AUTHORITATIVE — when a symbol is in it, use
        # ONLY those addresses (its comment also names helper/dependency addresses we must NOT count as
        # "implemented here"). New top-down natives absent from the table fall back to name-hex + the
        # header-comment address(es) before the first em-dash/colon.
        impl = list(OVR.get(sym, []))
        if not impl:
            nh = NAMEHEX.match(sym)
            if nh:
                impl.append(nh.group(1).upper())
            # def-line trailing comment (class-method FUN tag) takes precedence for the owned address
            for a in ADDR_RE.findall(defcomment) + [x.upper() for x in FUN_RE.findall(defcomment)]:
                if a.upper() not in impl:
                    impl.append(a.upper())
            header = []
            for cl in comment:
                header.append(cl)
                if "—" in cl or " - " in cl or ":" in cl:
                    break
            htext = " ".join(header)
            for a in ADDR_RE.findall(htext) + [x.upper() for x in FUN_RE.findall(htext)]:
                if a.upper() not in impl:
                    impl.append(a.upper())
            if not impl:  # last resort: first address anywhere in the comment block
                for a in ADDR_RE.findall(" ".join(comment)) + [x.upper() for x in FUN_RE.findall(" ".join(comment))]:
                    impl.append(a.upper()); break

        # A class method is only a NATIVE OWNER if it implements a guest address (FUN tag / comment addr).
        # Un-owned helper methods tree-wide must NOT pollute the index.
        if is_method and not impl:
            i = j + 1
            continue
        deps = sorted({d.upper() for d in DEP_RE.findall(bodytext)})
        desc = ""
        if comment:
            # first comment line, stripped of the leading address tokens, as a one-line summary
            desc = re.sub(r'^[/\s]*((0x8[0-9A-Fa-f]{7}|FUN_8[0-9a-fA-F]{7}|/)\s*)+[—:-]?\s*', '', comment[0]).strip()
        natives.append(dict(sym=sym, file=rel, line=i + 1, impl=impl, deps=deps, desc=desc, body=bodytext))
        i = j + 1


def build(natives):
    by_sym = {n["sym"]: n for n in natives}
    # direct C call graph: for each native, which other native symbols its body references
    sym_set = set(by_sym)
    callers = {s: set() for s in sym_set}
    callee_re = re.compile(r'\b(ov_\w+|native_\w+|eng_\w+)\b')
    for n in natives:
        for cal in callee_re.findall(n["body"]):
            if cal in sym_set and cal != n["sym"]:
                callers[cal].add(n["sym"])
    # reachability from ROOTS via direct C calls
    live = set()
    stack = [r for r in ROOTS if r in sym_set]
    # also seed: anything native_boot.cpp references directly
    nb = os.path.join(ROOT, "runtime/recomp/native_boot.cpp")
    if os.path.exists(nb):
        nbtext = open(nb, encoding="utf-8", errors="replace").read()
        for s in sym_set:
            if re.search(r'\b' + re.escape(s) + r'\s*\(', nbtext):
                stack.append(s)
    while stack:
        s = stack.pop()
        if s in live:
            continue
        live.add(s)
        # follow callees of s
        for n in [by_sym[s]] if s in by_sym else []:
            for cal in callee_re.findall(n["body"]):
                if cal in sym_set and cal not in live:
                    stack.append(cal)
    return by_sym, callers, live


def addr_index(natives):
    idx = {}
    for n in natives:
        for a in n["impl"]:
            idx.setdefault(a.upper(), []).append(n)
    return idx


def main():
    natives = []
    for f in collect_files():
        parse_file(f, natives)
    by_sym, callers, live = build(natives)
    idx = addr_index(natives)

    args = sys.argv[1:]
    if "--addr" in args:
        a = args[args.index("--addr") + 1].upper().replace("0X", "")
        owners = idx.get(a, [])
        if not owners:
            print(f"0x{a}: NO native owner found.")
        for n in owners:
            st = "LIVE" if n["sym"] in live else "ORPHAN"
            print(f"0x{a}: {n['sym']}  [{st}]  {n['file']}:{n['line']}")
            if n["desc"]: print(f"    desc: {n['desc']}")
            if n["deps"]: print(f"    depends on (still-PSX leaves): {', '.join('0x'+d for d in n['deps'])}")
            cs = sorted(callers.get(n["sym"], []))
            print(f"    C callers: {', '.join(cs) if cs else '(none — only the removed override table)'}")
        # who depends on this address?
        dep_users = [n for n in natives if a in n["deps"]]
        if dep_users:
            print(f"  depended-on by: {', '.join(sorted(set(n['sym'] for n in dep_users)))}")
        return

    if "--orphans" in args:
        rows = [(a, n) for a, ns in idx.items() for n in ns if n["sym"] not in live]
        for a, n in sorted(rows, key=lambda r: (r[0], r[1]["sym"])):
            print(f"0x{a}  {n['sym']:32s} {n['file']}:{n['line']}")
        print(f"\n{len(rows)} owned addresses currently ORPHANED (native exists, nothing C-calls it).")
        return

    # default: emit the markdown index
    out = []
    out.append("# Code map — guest address → PC-native owner\n")
    out.append("> GENERATED by `tools/codemap.py` — do not edit by hand; rerun the tool.\n")
    out.append("Before reimplementing any `FUN_xxxx`, look it up here (or `tools/codemap.py --addr <hex>`).")
    out.append("A native may exist already. **LIVE** = reachable by direct C call from a native_boot")
    out.append("dispatch root (actually runs). **ORPHAN** = native exists but only the REMOVED override")
    out.append("table used to reach it — it is dead code until a native parent calls it directly.\n")
    n_live = sum(1 for n in natives if n["sym"] in live)
    out.append(f"Totals: {len(natives)} native fns, {len(idx)} owned addresses, "
               f"{n_live} LIVE / {len(natives)-n_live} ORPHAN.\n")
    out.append("| addr | status | symbol | file:line | depends-on (still-PSX) | summary |")
    out.append("|------|--------|--------|-----------|------------------------|---------|")
    for a in sorted(idx):
        for n in idx[a]:
            st = "LIVE" if n["sym"] in live else "ORPHAN"
            deps = " ".join("0x" + d for d in n["deps"][:6]) + (" …" if len(n["deps"]) > 6 else "")
            desc = (n["desc"][:70] + "…") if len(n["desc"]) > 70 else n["desc"]
            desc = desc.replace("|", "\\|")
            out.append(f"| 0x{a} | {st} | `{n['sym']}` | {n['file']}:{n['line']} | {deps} | {desc} |")
    text = "\n".join(out) + "\n"
    if "--stdout" in args:
        sys.stdout.write(text)
    else:
        dest = os.path.join(ROOT, "docs/code-map.md")
        open(dest, "w").write(text)
        print(f"wrote {os.path.relpath(dest, ROOT)}: {len(natives)} natives, {len(idx)} addresses, "
              f"{n_live} LIVE / {len(natives)-n_live} ORPHAN")


if __name__ == "__main__":
    main()
