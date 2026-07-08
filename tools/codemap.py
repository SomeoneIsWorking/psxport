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

DEF_RE  = re.compile(r'^\s*(?:static\s+)?(?:inline\s+)?[\w:*&<>]+\s+((?:ov_|native_|eng_|beh_)\w+)\s*\(\s*Core\s*\*')
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


def load_behavior_table():
    """Authoritative addr->symbol map for per-object behavior handlers registered in
    BehaviorDispatch::kTable (game/object/behavior_dispatch.cpp) — the pc_skip=true-only native
    shortcut table (see CLAUDE.md engine-overrides + game.h pc_skip). These `beh_*` fns take no
    address in their own name (unlike `ov_<hex>`) and their header comment sits at the TOP of their
    file, not adjacent to the def — so the name/comment heuristic below misses them entirely (this
    is what left the whole `beh_*` family — ~50 owned handlers — reporting 'NO native owner found'
    to `--addr`, even though they're live and gated correctly). The table itself is the ground truth."""
    path = os.path.join(ROOT, "game/object/behavior_dispatch.cpp")
    sym2addrs = {}
    if os.path.exists(path):
        for m in re.finditer(r'\{\s*0x([0-9A-Fa-f]{8})u\s*,\s*(beh_\w+)\s*,', open(path, encoding="utf-8").read()):
            sym2addrs.setdefault(m.group(2), []).append(m.group(1).upper())
    return sym2addrs


OVR = load_override_table()
OVR.update(load_behavior_table())


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
        if m:
            # Reject a forward DECLARATION masquerading as a definition: a prototype ends in `;`
            # before any `{` ever appears on the line (e.g. `uint32_t foo(Core*, uint32_t);  //
            # FUN_xxxx, native (bar.cpp)`). Without this guard the def-line trailing-comment FUN
            # tag makes the brace-balance scanner below treat everything from the prototype to the
            # next unbalanced `}` as the "body" of a phantom native — a real bug this tool hit once
            # `beh_`/`native_`-prefixed forward decls started carrying their own FUN_ tag comments.
            code_part = lines[i].split("//", 1)[0]
            if ";" in code_part and "{" not in code_part:
                m = None
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

        def tag_portion(text):
            """A header line names the owner LEFT of the first separator (—/:/-). Anything to the
            RIGHT is description prose and may reference OTHER addresses (e.g. `// FUN_80107e20 —
            transition variant … 1 effect 0x8003e264 …`); those must NOT be counted as owner tags."""
            for sep in ("—", " - ", ":"):
                if sep in text:
                    return text.split(sep, 1)[0]
            return text
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
            # def-line trailing comment (class-method FUN tag) takes precedence for the owned address.
            # Only scan the tag portion (left of any separator) — addresses in description prose don't own.
            dtag = tag_portion(defcomment)
            for a in [x.upper() for x in FUN_RE.findall(dtag)] + ADDR_RE.findall(dtag):
                if a.upper() not in impl:
                    impl.append(a.upper())
            header = []
            for cl in comment:
                header.append(cl)
                if "—" in cl or " - " in cl or ":" in cl:
                    break
            htext = " ".join(tag_portion(cl) for cl in header)
            for a in [x.upper() for x in FUN_RE.findall(htext)] + ADDR_RE.findall(htext):
                if a.upper() not in impl:
                    impl.append(a.upper())
            if not impl:  # last resort: first address anywhere in the comment block, in TEXTUAL order.
                # A `Foo::bar — … guest 0x80059D28 …` class-method header names its owner in prose;
                # taking the earliest hit avoids picking up a later callee reference (e.g. FUN_8005950C).
                m2 = re.search(r'0x(8[0-9A-Fa-f]{7})|FUN_(8[0-9a-fA-F]{7})', " ".join(comment))
                if m2:
                    impl.append((m2.group(1) or m2.group(2)).upper())

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
        natives.append(dict(sym=sym, file=rel, line=i + 1, impl=impl, deps=deps, desc=desc, body=bodytext,
                             bstart=i, bend=j))
        i = j + 1


def ordinary_corpus(files, natives):
    """Full source text of the whole corpus with every indexed native's OWN body blanked out.
    This is where the vast majority of real call sites live: ordinary (non-native-tagged) game
    code — `beh_*.cpp`, `demo.cpp`, `sop.cpp`, HLE adapter shims — invoking a native method via
    `c->game->cd.dc40Sync(...)`, `c->engine.asset.loadDescriptorChunk(...)`, `obj.build(...)`, or a
    bare in-class call. Blanking each native's own body prevents its own signature/self-recursion
    from counting as "called" (a def line like `void Asset::loadDescriptorChunk(...) {` contains
    the callee text `loadDescriptorChunk(` too — that must NOT self-seed liveness)."""
    by_file = {}
    for n in natives:
        by_file.setdefault(n["file"], []).append((n["bstart"], n["bend"]))
    chunks = []
    for f in files:
        rel = os.path.relpath(f, ROOT)
        lines = open(f, encoding="utf-8", errors="replace").read().splitlines()
        for lo, hi in by_file.get(rel, []):
            for k in range(lo, min(hi + 1, len(lines))):
                lines[k] = ""
        chunks.append("\n".join(lines))
    return "\n".join(chunks)


def build(natives, files):
    by_sym = {n["sym"]: n for n in natives}
    sym_set = set(by_sym)
    callers = {s: set() for s in sym_set}

    # --- callee detection: two complementary forms -------------------------------------------
    # (1) free-function / qualified-static syntax: ov_foo(...), native_bar(...), Class::method(...)
    #     (this alone is what the tool originally recognized — it MISSES all instance-call syntax:
    #     `obj.method(...)`, `ptr->method(...)`, and bare in-class `method(...)`, which is how nearly
    #     every OOP native is actually invoked — c->game->cd.dc40Sync(...), c->mRender->mNodeXform.
    #     buildWithOffset(...), c->engine.asset.loadDescriptorChunk(...). That gap is WHY the tool was
    #     reporting 231/237 natives ORPHAN — almost all of them are wired, just via `.`/`->`.)
    qualified_re = re.compile(r'\b(ov_\w+|native_\w+|eng_\w+|[A-Z][A-Za-z0-9_]*::[A-Za-z_]\w*)\b')
    # (2) instance-call syntax for METHOD natives: `.name(`, `->name(`, or bare `name(` all share the
    #     same trailing token — the callee's bare method name immediately before `(`. We can't see the
    #     receiver's static type without a real parser, so to avoid a common method name (e.g. `run`,
    #     `build`) spuriously wiring an unrelated native, only bare names that are UNIQUE across every
    #     indexed method-native are matched this way; ambiguous names fall back to form (1) only
    #     (ClassName::method(...) — still recognized, just not the shorthand instance-call form).
    bare2sym, name_count = {}, {}
    for s in sym_set:
        if "::" in s:
            bare = s.split("::")[-1]
            name_count[bare] = name_count.get(bare, 0) + 1
            bare2sym[bare] = s
    unique_bares = [b for b, c in name_count.items() if c == 1]
    bare_re = re.compile(r'\b(' + "|".join(re.escape(b) for b in unique_bares) + r')\s*\(') \
        if unique_bares else None

    # (3) ambiguous method names (e.g. `init` owned by Font, CutsceneCamera, Pool, ...): the bare
    #     name alone is too common to attribute safely, but the RECEIVER right before `.`/`->` almost
    #     always echoes the class name in some cheap lowercase form — `c->engine.font.init()` for
    #     `Font::init`, `cam.init()` for `CutsceneCamera::init`, `c->engine.demo.stageMain()` for
    #     `Demo::stageMain` vs. `c->engine.stageMain()` for `Engine::stageMain`. Build per-symbol
    #     receiver-hint patterns from the class name's camelCase segments (full name, each segment,
    #     and a short abbreviation of the last segment) and require the receiver to match one of them
    #     immediately before the call — this disambiguates without a real type-checker.
    seg_re = re.compile(r'[A-Z][a-z0-9]*')
    ambiguous_re = {}
    for s in sym_set:
        if "::" not in s:
            continue
        cls, bare = s.split("::", 1)
        if name_count.get(bare, 0) <= 1:
            continue  # handled by the unique bare-name path above
        segs = seg_re.findall(cls) or [cls]
        hints = {cls.lower()}
        hints.update(seg.lower() for seg in segs)
        hints.add(segs[-1][:3].lower())
        hints = {h for h in hints if len(h) >= 3}
        if not hints:
            continue
        pat = r'\b(?:' + "|".join(re.escape(h) for h in hints) + r')\w*\s*(?:\.|->)\s*' + re.escape(bare) + r'\s*\('
        ambiguous_re[s] = re.compile(pat)

    def find_callees(text):
        found = set(cal for cal in qualified_re.findall(text) if cal in sym_set)
        if bare_re:
            found.update(bare2sym[b] for b in bare_re.findall(text))
        for s, rx in ambiguous_re.items():
            if rx.search(text):
                found.add(s)
        return found

    # native-to-native call graph (for transitive reachability + the "C callers" report column)
    for n in natives:
        for cal in find_callees(n["body"]):
            if cal != n["sym"]:
                callers[cal].add(n["sym"])

    # reachability from ROOTS via the native-to-native graph
    live = set()
    stack = [r for r in ROOTS if r in sym_set]
    while stack:
        s = stack.pop()
        if s in live:
            continue
        live.add(s)
        for n in [by_sym[s]] if s in by_sym else []:
            for cal in find_callees(n["body"]):
                if cal not in live:
                    stack.append(cal)

    # additionally: anything invoked from ORDINARY (non-native-tagged) game/engine/runtime code —
    # this is the actual call graph for OOP natives, since most callers are plain game logic
    # (behavior scripts, scene/demo code, HLE adapter shims), not other codemap-indexed natives.
    ordinary = ordinary_corpus(files, natives)
    ordinary_hit = find_callees(ordinary)
    for s in ordinary_hit:
        if s not in live:
            live.add(s)
            stack = [s]
            while stack:
                cur = stack.pop()
                for n in [by_sym[cur]] if cur in by_sym else []:
                    for cal in find_callees(n["body"]):
                        if cal not in live:
                            live.add(cal)
                            stack.append(cal)
    return by_sym, callers, live, ordinary_hit


def addr_index(natives):
    idx = {}
    for n in natives:
        for a in n["impl"]:
            idx.setdefault(a.upper(), []).append(n)
    return idx


def main():
    natives = []
    files = collect_files()
    for f in files:
        parse_file(f, natives)
    by_sym, callers, live, ordinary_hit = build(natives, files)
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
            extra = " + ordinary game/engine code" if n["sym"] in ordinary_hit else ""
            print(f"    C callers: {(', '.join(cs) if cs else '(none)') + extra}")
        # who depends on this address?
        dep_users = [n for n in natives if a in n["deps"]]
        if dep_users:
            print(f"  depended-on by: {', '.join(sorted(set(n['sym'] for n in dep_users)))}")
        return

    if "--orphans" in args:
        rows = [(a, n) for a, ns in idx.items() for n in ns if n["sym"] not in live]
        for a, n in sorted(rows, key=lambda r: (r[0], r[1]["sym"])):
            print(f"0x{a}  {n['sym']:32s} {n['file']}:{n['line']}")
        print(f"\n{len(rows)} owned addresses currently ORPHANED "
              f"(native exists, nothing calls it — not even via `.`/`->`/bare method syntax).")
        return

    # default: emit the markdown index
    out = []
    out.append("# Code map — guest address → PC-native owner\n")
    out.append("> GENERATED by `tools/codemap.py` — do not edit by hand; rerun the tool.\n")
    out.append("Before reimplementing any `FUN_xxxx`, look it up here (or `tools/codemap.py --addr <hex>`).")
    out.append("A native may exist already. **LIVE** = reachable by a real call from either a native_boot")
    out.append("dispatch root or ordinary (non-native-tagged) game/engine code — free-function syntax")
    out.append("(`ov_foo(...)`), qualified static syntax (`Class::method(...)`), or C++ instance-call")
    out.append("syntax (`obj.method(...)`, `ptr->method(...)`, bare in-class `method(...)`). **ORPHAN** =")
    out.append("native exists but no call site of any of those forms was found anywhere in the tree — it")
    out.append("is genuinely dead code until something calls it.\n")
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
