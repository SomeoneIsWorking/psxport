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
                                        #   (warns ⚠ DUAL-OWNERSHIP if authoritatively owned in >=2 files)
  tools/codemap.py --conflicts          # list every guest addr with cross-file authoritative multi-
                                        #   ownership — the duplicate-RE smell (a 2nd native owning a
                                        #   FUN_xxxx some other file already owns; run --addr on each)
  tools/codemap.py --substrate-fallthrough  # native-owned addrs that are a DISPATCH TARGET but NOT
                                        #   override-registered — callers silently hit the emulated
                                        #   substrate (register + MIRROR_VERIFY to native-ize; --all
                                        #   includes soft-attributed owners)
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
# A SECOND class of native carries NO recognized prefix at all: a free function named
# `<description>_<hexsuffix>` (grid_query_47cbc, child_spawn_40410, hitbox_build_3b220, ...) that
# takes `Core*` exactly like an `ov_`/`native_` native and is tagged the same way (a `// FUN_xxxx —`
# comment immediately above, or — see FILE_HEADER_ADDR_RE below — a file-level header tag). DEF_RE's
# prefix requirement made every one of these invisible to `--addr` ("NO native owner found") despite
# being real, tagged, called-by-direct-C++-call natives. Only reached when DEF_RE/METHOD_RE both miss;
# still requires the `impl` (below) to be non-empty, so an untagged helper leaf (`s16`, `leaf1`, ...)
# that happens to take `Core*` first is NOT mistaken for a native (see the is_freefn skip-if-empty guard).
FREEFN_RE = re.compile(r'^\s*(?:static\s+)?(?:inline\s+)?[\w:*&<>]+\s+(\w+)\s*\(\s*Core\s*\*')
ADDR_RE = re.compile(r'0x(8[0-9A-Fa-f]{7})')
FUN_RE  = re.compile(r'FUN_(8[0-9a-fA-F]{7})')
NAMEHEX = re.compile(r'^(?:ov|native|eng)_([0-9A-Fa-f]{6,8})$')
DEP_RE  = re.compile(r'(?:rec_dispatch|rec_super_call|super_call|call_fn|rc[0-4]|rec_coro_redirect)\s*\(\s*c\s*,\s*0x(8[0-9A-Fa-f]{7})')
# A file-level header ("game/player/hitbox.cpp — PC-native ownership of FUN_8003B220.") tags the ONE
# guest address the file exists to own, for files where the tag sits at the top (file/module doc
# comment) rather than immediately above the def (e.g. hitbox.cpp's def is preceded by an unrelated
# one-line comment, with unrelated `#include`s and a tiny helper fn separating it from the real header).
# Deliberately strict — "ownership of FUN_xxxx"/"ownership of 0x..." immediately adjacent, so it does
# NOT fire on multi-native files whose header describes a SUBSYSTEM ("ownership of the engine's
# geometry SUBMIT path") or lists several addresses in prose (release_trigger_motion.h) — those already
# resolve per-method via their own adjacent tags.
FILE_HEADER_ADDR_RE = re.compile(r'ownership of\s+(?:FUN_(8[0-9A-Fa-f]{7})|0x(8[0-9A-Fa-f]{7}))', re.IGNORECASE)


def tag_portion(text):
    """A header line names the owner LEFT of the first separator (—/:/-). Anything to the RIGHT is
    description prose and may reference OTHER addresses (e.g. `// FUN_80107e20 — transition variant …
    1 effect 0x8003e264 …`); those must NOT be counted as owner tags."""
    for sep in ("—", " - ", ":"):
        if sep in text:
            return text.split(sep, 1)[0]
    return text


def comment_above(lines, i):
    """The single comment line immediately above line i (or "" if none) — i.e. the FIRST line of the
    contiguous `//` block right above a def/decl, which is where this tree's tag convention always
    puts the owned address (see scan_decl_tags for why scanning further lines is unsafe)."""
    c = i - 1
    if c >= 0 and lines[c].lstrip().startswith("//"):
        while c - 1 >= 0 and lines[c - 1].lstrip().startswith("//"):
            c -= 1
        return lines[c].strip()
    return ""


def file_header_addr(lines):
    """The leading contiguous comment/blank block at the top of the file (the file's own doc-comment,
    stopping at the first real code line), searched for the single-address 'ownership of FUN_xxxx'
    tag. Returns None for the common case (no such phrase, or a multi-native/subsystem file whose
    header doesn't name one specific address that way)."""
    block = []
    for ln in lines:
        s = ln.strip()
        if s.startswith("//") or s == "":
            block.append(ln)
        else:
            break
    m = FILE_HEADER_ADDR_RE.search("\n".join(block))
    return (m.group(1) or m.group(2)).upper() if m else None


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


def collect_files():
    files = []
    for g in SRC_GLOBS:
        files += glob.glob(os.path.join(ROOT, g), recursive=True)
    return sorted(set(files))


CLASS_OPEN_RE = re.compile(r'^\s*class\s+(\w+)\b[^;{]*\{')
DECL_RE = re.compile(r'^\s*(?:static\s+)?(?:virtual\s+)?[\w:*&<>]+\s+(\w+)\s*\([^;{]*\)\s*(?:const)?\s*(?:override)?\s*;')


def scan_decl_tags(files):
    """A second, ORTHOGONAL source of address ownership beyond load_override_table/
    load_behavior_table: a class's method is declared (not defined) in its header with the guest FUN_
    tag on/above the DECLARATION (e.g. Trig::rsin — game/math/trig.h tags `rsin`/`rcos`/`ratan2`/
    `angleCmp` next to their in-class declarations), while the out-of-line DEFINITION in the .cpp has
    no adjacent tag at all (trig.cpp's method bodies open with zero comment above them — the tag lives
    only in the header, nowhere near the def the main parse_file() scanner inspects). Declarations
    themselves are skipped by parse_file's forward-decl guard (correctly — there's no body to scan for
    callees/deps), so without this pass those methods report 'NO native owner found' despite being
    tagged, real, and reached by ordinary direct C++ calls. Declarations inside a class body have no
    `ClassName::` qualifier (`int32_t rsin(...) const;`), so we track the enclosing `class Foo {` to
    reconstruct the qualified symbol the .cpp definition (found by METHOD_RE) will use."""
    tags = {}
    for path in files:
        if not path.endswith(".h") and not path.endswith(".hpp"):
            continue
        lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
        class_stack, depth = [], 0
        for i, line in enumerate(lines):
            m = CLASS_OPEN_RE.match(line)
            if m:
                class_stack.append((m.group(1), depth))
            depth += line.count("{") - line.count("}")
            while class_stack and depth <= class_stack[-1][1]:
                class_stack.pop()
            if not class_stack:
                continue
            dm = DECL_RE.match(line)
            if not dm:
                continue
            defcomment = line[line.index("//"):].strip() if "//" in line else ""
            # First-line-only, untruncated scan (deliberately NOT tag_portion's "left of separator"
            # rule, and NOT a multi-line header scan): this codebase's declaration-tag convention is
            # `methodName(args): guest FUN_xxxx. <description mentioning OTHER addresses>` — the colon
            # comes BEFORE the real address, so tag_portion's split-at-colon truncates it away, and
            # scanning a 2nd/3rd comment line for a fallback picks up a dependency address from the
            # description instead (this exact shape mis-owned Engine::objMatrixCompose with its own
            # `deps` — 0x80085480/80084110/80084470/80051128 — because line 1 (holding the true
            # 0x800518FC) got truncated at ':' and line 2 (a pure dependency list) was scanned next).
            # Every real declaration tag in this tree names its own address somewhere on the line
            # immediately above the declaration (or trailing on the declaration line itself) — never
            # needs a 2nd line — so take the FIRST match on defcomment/first-comment-line only.
            first_line = defcomment or (comment_above(lines, i))
            if not first_line:
                continue
            m3 = re.search(r'FUN_(8[0-9a-fA-F]{7})|0x(8[0-9A-Fa-f]{7})', first_line)
            if not m3:
                continue
            addrs = [(m3.group(1) or m3.group(2)).upper()]
            sym = f"{class_stack[-1][0]}::{dm.group(1)}"
            for a in addrs:
                tags.setdefault(sym, [])
                if a not in tags[sym]:
                    tags[sym].append(a)
    return tags


def load_engine_overrides():
    """Authoritative addr->symbol map recovered from LIVE EngineOverrides registrations in the
    CURRENT sources: `ov.register_(0xADDR, "Class::method", fn)` (runtime/recomp/engine_overrides.h).
    The quoted second argument IS the owning symbol — the same qualified name METHOD_RE captures at
    the DEFINITION — so a native wired ONLY via register_ (no `// FUN_xxxx` tag on its def line, and
    absent from the faeb436^ snapshot tsv that load_override_table reads) is still attributed to its
    guest address. Without this pass such a native reports 'NO native owner found' AND, worse, stays
    invisible as a SECOND owner of an address some other file already claims — which is exactly how
    cube_text_ledger.cpp's CubeTextLedger::activateSlot silently duplicated scene_events.cpp's
    SceneEvents::armBody on FUN_80040B48 (the --conflicts dual-ownership detector depends on this)."""
    sym2addrs = {}
    reg = re.compile(r'\.register_\s*\(\s*0x([0-9A-Fa-f]{8})u?\s*,\s*"([^"]+)"')
    for path in collect_files():
        try:
            txt = open(path, encoding="utf-8", errors="replace").read()
        except OSError:
            continue
        for m in reg.finditer(txt):
            sym2addrs.setdefault(m.group(2), []).append(m.group(1).upper())
    return sym2addrs


def load_registered_addrs():
    """Set of guest addresses CURRENTLY wired to a native override at runtime: EngineOverrides
    `register_(0xADDR, ...)`, any `*set_override(...)` family (shard_set_override / engine_set_override_* /
    rec_set_override / ov_sop_set_override), and BehaviorDispatch::kTable {0xADDR, beh_*} entries. The
    faeb436^ tsv snapshot is historical (pre-removal) and is NOT counted — it does not reflect current
    runtime wiring. Used by --substrate-fallthrough to separate a dispatched-and-wired address from one
    that still falls through to substrate."""
    addrs = set()
    reg = re.compile(r'(?:\.register_\w*|[A-Za-z_]*set_override)\s*\(\s*(?:c\s*,\s*)?0x([0-9A-Fa-f]{8})')
    for path in collect_files():
        try:
            txt = open(path, encoding="utf-8", errors="replace").read()
        except OSError:
            continue
        for m in reg.finditer(txt):
            addrs.add(m.group(1).upper())
    for a_list in load_behavior_table().values():
        addrs.update(a.upper() for a in a_list)
    return addrs


# Dispatch idioms that route through the override table (→ substrate when the target is UNREGISTERED).
# PRECISE capture of the TARGET address only (not any 0x8 on the line): the target is the first address
# argument. Two arg shapes: `idiom(c, 0xADDR...)` (rec_dispatch/guest_leaf/…) and `callObj/call(c, arg,
# …, 0xADDR)` (address is a later arg). guest_fn is DELIBERATELY EXCLUDED — it is an explicit "run the
# substrate leaf" call (a native that intends the emulated body), not a fallthrough.
DISPATCH_TARGET_RES = [
    re.compile(r'\b(?:rec_dispatch|rec_super_call|super_call|guest_leaf|guest_dispatch|call_fn|rc[0-4])\s*\(\s*c\s*,\s*0x([0-9A-Fa-f]{8})'),
    re.compile(r'\b(?:callObj\d|call\d)\s*\(\s*c\s*,[^;{}]*?0x([0-9A-Fa-f]{8})'),
]


def scan_dispatched_addrs():
    """Set of guest addresses reached as the TARGET of a dispatch idiom with a literal address. Dynamic
    dispatches (rec_dispatch(c, handler) with a variable) carry no literal and are correctly ignored."""
    addrs = set()
    for path in collect_files():
        try:
            txt = open(path, encoding="utf-8", errors="replace").read()
        except OSError:
            continue
        for rx in DISPATCH_TARGET_RES:
            for m in rx.finditer(txt):
                addrs.add(m.group(1).upper())
    return addrs


OVR = load_override_table()
OVR.update(load_behavior_table())
OVR.update(scan_decl_tags(collect_files()))
OVR.update(load_engine_overrides())


def parse_file(path, natives):
    rel = os.path.relpath(path, ROOT)
    lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
    pending_freefn = []  # candidates with no per-def tag; may be claimed by the file-header fallback
    i = 0
    while i < len(lines):
        m = DEF_RE.match(lines[i])
        is_method = False
        is_freefn = False
        if not m:
            m = METHOD_RE.match(lines[i])
            is_method = True
        if not m:
            m = FREEFN_RE.match(lines[i])
            is_freefn = True
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
        # AUTHORITATIVE attribution = a real ownership source (override tsv / behavior table / decl-tag /
        # live EngineOverrides register_) or a name-for-address (ov_<hex>/gen_func_<hex>). SOFT = the
        # comment/header-prose fallbacks below, which also fire on a fn that merely MENTIONS an address
        # it traces or reads (a diagnostic tracer, a data-list head) — those must NOT count toward the
        # --conflicts dual-ownership signal, or it drowns in false positives.
        impl_auth = bool(impl)
        if not impl:
            nh = NAMEHEX.match(sym)
            if nh:
                impl.append(nh.group(1).upper())
                impl_auth = True
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
                # For a name-agnostic free fn (is_freefn), scanning the WHOLE block is unsafe: unlike the
                # disciplined "Class::method — native ownership of FUN_xxxx" header every tagged method
                # uses, a free fn's preceding comment is often a multi-paragraph DESIGN NOTE that mentions
                # several unrelated addresses before ever naming its own (e.g. gte_op's comment discusses
                # `gte_math.cpp ov_mat_mul = FUN_80084110` — another function's address — while gte_op
                # itself owns none; scanning the full block wrongly credited gte_op with 0x80084110,
                # already correctly owned by Math::matMul). Every real free-fn tag observed in this tree
                # names its address on the comment block's FIRST line, so restrict to that line only.
                scan_text = comment[0] if (is_freefn and comment) else " ".join(comment)
                m2 = re.search(r'0x(8[0-9A-Fa-f]{7})|FUN_(8[0-9a-fA-F]{7})', scan_text)
                if m2:
                    impl.append((m2.group(1) or m2.group(2)).upper())

        # A class method / name-agnostic free fn is only a NATIVE OWNER if it implements a guest address
        # (FUN tag / comment addr). Un-owned helper methods/leaf fns tree-wide must NOT pollute the index —
        # EXCEPT: a name-agnostic free fn (is_freefn) with no address of its OWN gets one more chance below,
        # via the file-header fallback, before being dropped (see pending_freefn).
        if (is_method or is_freefn) and not impl:
            if is_freefn:
                deps = sorted({d.upper() for d in DEP_RE.findall(bodytext)})
                desc = re.sub(r'^[/\s]*((0x8[0-9A-Fa-f]{7}|FUN_8[0-9a-fA-F]{7}|/)\s*)+[—:-]?\s*', '',
                               comment[0]).strip() if comment else ""
                pending_freefn.append(dict(sym=sym, file=rel, line=i + 1, deps=deps, desc=desc, body=bodytext,
                                            bstart=i, bend=j, is_freefn=True))
            i = j + 1
            continue
        deps = sorted({d.upper() for d in DEP_RE.findall(bodytext)})
        desc = ""
        if comment:
            # first comment line, stripped of the leading address tokens, as a one-line summary
            desc = re.sub(r'^[/\s]*((0x8[0-9A-Fa-f]{7}|FUN_8[0-9a-fA-F]{7}|/)\s*)+[—:-]?\s*', '', comment[0]).strip()
        natives.append(dict(sym=sym, file=rel, line=i + 1, impl=impl, deps=deps, desc=desc, body=bodytext,
                             bstart=i, bend=j, is_freefn=is_freefn, authoritative=impl_auth))
        i = j + 1

    # File-header fallback (Fix for hitbox.cpp-style files): a file's own leading doc-comment names
    # ONE guest address it exists to own ("... ownership of FUN_8003B220."), but the tag sits far from
    # the def (separated by #includes / a tiny unrelated helper fn), so no per-def scan above found it.
    # Only fires when (a) the file actually makes that "ownership of FUN_xxxx" claim, and (b) nothing
    # already indexed in this file claims that address — then attributes it to the LARGEST untagged
    # free-fn candidate in the file (the primary implementation; incidental one-line helpers like `s16`
    # are never the biggest body in a file dedicated to one leaf).
    if pending_freefn:
        addr = file_header_addr(lines)
        if addr and not any(addr in n["impl"] for n in natives if n["file"] == rel):
            best = max(pending_freefn, key=lambda n: n["bend"] - n["bstart"])
            best["impl"] = [addr]
            best["authoritative"] = True   # explicit "ownership of FUN_xxxx" file-header claim
            natives.append(best)


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
    # NOTE: `beh_\w+` is included here even though DEF_RE recognizes it as a def prefix and the
    # call-detection loop below never required a trailing `(` — this is deliberate: `beh_*` handlers
    # are wired as FUNCTION-POINTER VALUES in a registration table (BehaviorDispatch::kTable in
    # game/object/behavior_dispatch.cpp — `{ 0xADDR, beh_foo, "foo" }`), called later via `b.fn(c)`
    # indirection, never via literal `beh_foo(...)` call syntax anywhere in the tree. Before this was
    # added, EVERY kTable-registered beh_* was invisible to find_callees() (qualified_re didn't match
    # the `beh_` prefix at all) and reported ORPHAN despite being live — 65 of 66 ORPHAN rows in one
    # audit were this exact false positive. The same "bare identifier used as a value, not a call" shape
    # also covers other pointer-table registrations (EngineOverrides::register_, PlatformHle::register_)
    # for symbols that already match one of these prefixes or the Class::method form.
    qualified_re = re.compile(r'\b(ov_\w+|native_\w+|eng_\w+|beh_\w+|[A-Z][A-Za-z0-9_]*::[A-Za-z_]\w*)\b')
    # (2) instance-call syntax for METHOD natives: `.name(`, `->name(`, or bare `name(` all share the
    #     same trailing token — the callee's bare method name immediately before `(`. We can't see the
    #     receiver's static type without a real parser, so to avoid a common method name (e.g. `run`,
    #     `build`) spuriously wiring an unrelated native, only bare names that are UNIQUE across every
    #     indexed method-native are matched this way; ambiguous names fall back to form (1) only
    #     (ClassName::method(...) — still recognized, just not the shorthand instance-call form).
    bare2sym, name_count = {}, {}
    # Name-agnostic free-fn natives (FREEFN_RE — grid_query_47cbc, hitbox_build_3b220, ...) have no
    # `::` and no recognized prefix either, so qualified_re above never sees a call to them (`foo(c)`
    # is just a bare identifier). They ARE always called by that exact bare name (there's no receiver
    # syntax for a free function) — fold them into the same bare-name table as unique method names, one
    # bucket keyed by "the whole symbol is its own bare name" instead of "the part after `::`".
    for s in sym_set:
        if "::" in s:
            bare = s.split("::")[-1]
        else:
            bare = s
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


def reconcile_freefn_claims(natives):
    """A name-agnostic free fn (FREEFN_RE) is often a THIN DISPATCH WRAPPER whose own doc-comment
    names the address it CALLS, not one it implements (e.g. beh_scene_ui_trigger.cpp's
    `render_and_return` — "dispatch the per-object render-state update FUN_800517F8 (owned)" — is a
    2-line wrapper around the ALREADY-owned GraphicsBind::renderUpdate; several `beh_*` behavior files
    each carry their own such wrapper around the same shared Engine::animTick/walkStart/etc.). Because
    parse_file resolves one file at a time, it can't see whether some OTHER file properly (via
    DEF_RE/METHOD_RE + a real per-def tag) already owns the same address — so this cross-file pass
    runs once, after every file is parsed, and drops any freefn's claim on an address that a non-freefn
    native already owns. Addresses shared among two non-freefn natives are left untouched (that's the
    deliberate pc_skip fork pattern — doSkip()/doFaithful() legitimately both implement one address)."""
    non_freefn_addrs = {a.upper() for n in natives if not n.get("is_freefn") for a in n["impl"]}
    kept = []
    for n in natives:
        if n.get("is_freefn") and any(a.upper() in non_freefn_addrs for a in n["impl"]):
            continue
        kept.append(n)
    return kept


def main():
    natives = []
    files = collect_files()
    for f in files:
        parse_file(f, natives)
    natives = reconcile_freefn_claims(natives)
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
        cf = sorted(set(n["file"] for n in owners if n.get("authoritative")))
        if len(cf) >= 2:
            print(f"  ⚠ DUAL-OWNERSHIP: 0x{a} is authoritatively implemented in {len(cf)} DIFFERENT files "
                  f"({', '.join(cf)}). Two natives claiming one guest address is a DUPLICATION bug "
                  f"unless it is a deliberate same-class pc_skip doSkip()/doFaithful() fork — "
                  f"consolidate to one owner (delegate one body to the other).")
        return

    if "--conflicts" in args:
        # Only AUTHORITATIVE owners (real registration / name-for-address / explicit ownership claim)
        # count — soft comment-scan attributions (a tracer/data-head that merely mentions the address)
        # are excluded so this is a low-false-positive signal.
        rows = []
        for a, ns in idx.items():
            auth = [n for n in ns if n.get("authoritative")]
            cf = sorted(set(n["file"] for n in auth))
            if len(cf) >= 2:
                rows.append((a, cf, sorted(set(n["sym"] for n in auth))))
        for a, cf, syms in sorted(rows):
            print(f"0x{a}: {len(cf)} files — {', '.join(syms)}")
            for f in cf:
                print(f"    {f}")
        print(f"\n{len(rows)} guest address(es) with CROSS-FILE authoritative multi-ownership "
              f"(duplication smell unless a deliberate pc_skip fork — those live in ONE file). "
              f"Run `--addr <hex>` on each.")
        return

    if "--substrate-fallthrough" in args:
        # An address that HAS a native owner AND is the TARGET of a dispatch idiom (rec_dispatch/
        # guest_leaf/…) but is NOT override-registered → those callers silently run the EMULATED body
        # while any direct-native callers run the port. That split is how FUN_800518FC hid (fixed
        # 2026-07-15). Register + MIRROR_VERIFY to native-ize. Boot/stage handlers reached only by a
        # direct native_boot call (never a dispatch target) do NOT appear — the precise target capture
        # excludes them. Still a candidate list: a few may be intentionally direct-call-only. `authOnly`
        # (default) restricts to authoritative owners; pass `--all` to include soft-attributed owners.
        auth_only = "--all" not in args
        registered = load_registered_addrs()
        dispatched = scan_dispatched_addrs()
        rows = []
        for a in sorted(set(idx) & dispatched - registered):
            ns = [n for n in idx[a] if n.get("authoritative")] if auth_only else idx[a]
            if ns:
                rows.append((a, ns))
        for a, ns in rows:
            syms = ", ".join(sorted(set(n["sym"] for n in ns)))
            print(f"0x{a}: native owner {syms} — dispatch target, NOT override-registered → callers hit SUBSTRATE")
            for n in ns:
                print(f"    {n['file']}:{n['line']}")
        print(f"\n{len(rows)} native-owned address(es) dispatched but not override-registered — dispatch/"
              f"guest_leaf callers fall through to the emulated substrate. Register + MIRROR_VERIFY to "
              f"native-ize (review each; a few may be intentionally direct-call-only).")
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
