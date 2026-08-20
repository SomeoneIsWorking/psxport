#!/usr/bin/env python3
"""layout_move.py — the psxport `runtime/recomp/` -> `runtime/<subsystem>/` move, as a
RE-RUNNABLE TRANSFORMATION rather than a patch.

Spec: docs/workspace/LAYOUT.md ("Target layout — organize like Dusklight/Aurora"). LAYOUT.md counted
149 files FLAT in runtime/recomp/; it is 168 tracked entries as of 2026-08-06 and still growing —
which is the argument, not a nitpick. this splits them into one directory per subsystem, mirroring how
Dusklight splits `src/dusk/` into `audio/ imgui/ ui/ mods/` rather than one bag of files.

WHY A SCRIPT AND NOT A PATCH. The move must land on a `main` that keeps moving. A diff rots the
moment anybody touches a moved file; a transformation does not. Re-run this against whatever main
has become and it produces the same shape — and it ABORTS (it does not guess) when main has grown
a file this table has never seen.

    tools/layout_move.py --plan       # print the mapping + every file it would touch, write nothing
    tools/layout_move.py --apply      # perform the move (git mv) + rewrite includes, then verify
    tools/layout_move.py --verify     # prove inertness against git HEAD (run BEFORE you commit)
    tools/layout_move.py --selftest   # prove the inertness check can FAIL (the negative control)

THE INERTNESS PROOF (LAYOUT.md: "PROVE CONTENT-IDENTITY MECHANICALLY"). Two checks, each covering
the other's blind spot, both printing their denominators — because a check that can only print "ok"
is not a check:

  A. CONTENT-HASH MULTISET, independent of this script's own logic. For every source file: delete
     the lines the move is ALLOWED to rewrite (quoted `#include` lines, and any line naming a
     `runtime` path), hash what is left, and require the multiset of hashes to be IDENTICAL before
     and after. An edit that rode along inside a 168-file diff — one changed constant, one deleted
     line — breaks a hash. Angle-bracket includes stay inside the hash. `--verify` prints how many
     lines were excluded out of how many, so the blind spot has a SIZE (measured: ~1.3-3%).
  B. PURE-FUNCTION RE-DERIVATION, covering every tracked text file including the ones check A
     ignores. Read the pre-move content out of git, re-run the transformation on it in memory, and
     require byte equality with the working tree. Files OUT of scope are required to be byte-equal
     to `rev` untouched, so a stray edit to a file nobody thought was involved fails too.

  Neither check can pass vacuously: both FAIL when the file SET differs (a file added, lost, or
  unmapped) instead of quietly comparing whatever is left, and `--selftest` proves on a synthetic
  repo that A catches a body edit and B catches an include-line tamper.

NO BEHAVIOUR CHANGE MAY RIDE ALONG. That is not politeness, it is the only reason check A can be
trusted. Anything that is not a path is a separate commit, before or after. The ONE exception is
documented at GTE_SHIM below: a header moved out from under a file in a submodule this repo may not
edit, so the build must hand that one file the one directory it lost.
"""

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile

# ---------------------------------------------------------------------------------------------
# THE MAPPING. One line per file. This is DATA on purpose: re-homing a file is a one-line edit and
# a re-run, not a rewrite of the script. `_ROOT_` means "stays at runtime/ root".
#
# `game_iface.h` / `recomp_iface.h` (and their .cpp storage) stay at runtime/ root because they are
# THE SEAM — burying a seam inside a subsystem directory hides what a game is allowed to touch, and
# `psxport_smoke` exists to police exactly that boundary.
# ---------------------------------------------------------------------------------------------

SUBSYS = {
    # -- the seam ------------------------------------------------------------------------------
    "game_iface.h": "_ROOT_",
    "game_iface.cpp": "_ROOT_",
    "recomp_iface.h": "_ROOT_",
    "recomp_iface.cpp": "_ROOT_",

    # -- cpu/ : dispatch, coro, core, MIPS interp — the execution substrate ---------------------
    "core.cpp": "cpu", "core.h": "cpu",
    "game.h": "cpu",                     # the whole-machine aggregate; belongs beside core.h
    "coro.cpp": "cpu", "coro.h": "cpu",
    "dispatch.cpp": "cpu",
    "interp.cpp": "cpu", "interp_diag.h": "cpu",
    "r3000.h": "cpu",
    "mem.cpp": "cpu",                    # guest RAM/scratchpad + hw I/O routing (Core methods)
    "host_turn.cpp": "cpu",
    "boot.cpp": "cpu",                   # load_exe: the PS-EXE loader = program load
    "native_boot.cpp": "cpu",            # the native boot/frame driver
    "native_stub.cpp": "cpu", "native_stub.h": "cpu",
    "scea_asset.h": "cpu",               # baked texture, used only by native_stub
    "scheduler.cpp": "cpu", "scheduler.h": "cpu",
    "pc_scheduler.cpp": "cpu", "pc_scheduler.h": "cpu",
    "overlay_router.cpp": "cpu", "overlay_router.h": "cpu",   # guest CODE overlay routing
    "override_registry.cpp": "cpu", "override_registry.h": "cpu",
    "guest_abi.h": "cpu", "guest_call.h": "cpu",
    "gte_beetle.cpp": "cpu", "gte_state.h": "cpu",            # COP2 is part of the machine
    "hw_bind.cpp": "cpu", "hw_bind.h": "cpu",
    "dma_irq.h": "cpu", "irq_edge.h": "cpu",
    "c_subsys.h": "cpu",                 # C-linkage decls for the still-C leaf subsystems
    "fs_util.cpp": "cpu", "fs_util.h": "cpu",                 # host std::filesystem wrapper
    "game_hooks_opt.cpp": "cpu", "game_hooks_opt.h": "cpu",   # guarded OPTIONAL-hook accessors

    # -- gpu/ : gpu_vk, gpu_native, render_queue, present/video plans, shaders_gpu/ -------------
    "gpu_vk.cpp": "gpu", "gpu_vk.h": "gpu",
    "gpu_vk_device.h": "gpu", "gpu_vk_internal.h": "gpu",
    "gpu_vk_present_mode.h": "gpu", "gpu_vk_present_policy.h": "gpu",
    "gpu_vk_semi_selftest.cpp": "gpu", "gpu_vk_semi_selftest.h": "gpu",
    "gpu_vk_selftest_support.h": "gpu",
    "gpu_vk_texture_phase_selftest.cpp": "gpu", "gpu_vk_texture_phase_selftest.h": "gpu",
    "gpu_vk_shaders.h": "gpu",           # GENERATED (gitignored) — see GEN_ONLY below
    "gpu_native.cpp": "gpu", "gpu_native_internal.h": "gpu",
    "gpu_debug.cpp": "gpu",
    "gpu_perf.cpp": "gpu", "gpu_perf.h": "gpu",
    "vram_xfer.cpp": "gpu", "vram_dirty.h": "gpu",
    "render_queue.cpp": "gpu", "render_queue.h": "gpu",
    "render_diag.h": "gpu", "render_mode.h": "gpu", "render_node.h": "gpu",
    "render_stats.h": "gpu", "render_substrate.h": "gpu",
    "proj_params.cpp": "gpu", "proj_params.h": "gpu",
    "proj_prim.cpp": "gpu", "proj_prim.h": "gpu", "proj_vtx.h": "gpu",
    "ot_attr.cpp": "gpu", "ot_attr.h": "gpu",
    "pgxp.cpp": "gpu", "pgxp.h": "gpu",
    "fps60.cpp": "gpu", "fps60.h": "gpu",
    "pace_plan.h": "gpu", "present_plan.h": "gpu", "video_plan.h": "gpu", "field_rate.h": "gpu",

    # -- audio/ : SPU ---------------------------------------------------------------------------
    "spu_audio.cpp": "audio", "spu_audio.h": "audio",
    "spu_beetle.cpp": "audio", "spu_device.h": "audio", "spu_state.h": "audio",

    # -- media/ : CD, disc, XA, FMV, MDEC -------------------------------------------------------
    "cd.h": "media", "cd_override.cpp": "media",
    "cdc_native.cpp": "media", "cdc_state.h": "media",
    "disc.cpp": "media", "disc.h": "media", "disc_provision.cpp": "media",
    "xa_stream.cpp": "media", "xa_state.h": "media",
    "fmv_decode.cpp": "media", "fmv_decode.h": "media",
    "native_fmv.cpp": "media", "native_fmv.h": "media",
    "mdec_beetle.c": "media", "mdec_device.h": "media", "mdec_state.h": "media",

    # -- hle/ : BIOS/SDK HLE, memcard -----------------------------------------------------------
    "hle.cpp": "hle", "hle.h": "hle",
    "platform_hle.h": "hle",
    "sync_overrides.cpp": "hle",         # the PlatformHle registration table
    "stubs.cpp": "hle",                  # unimplemented GTE/COP0/syscall hooks
    "threads.cpp": "hle",                # BIOS OpenThread/ChangeThread stubs
    "timing.cpp": "hle", "timing.h": "hle",   # libetc VSync counter mirror + frame tick
    "memcard.cpp": "hle", "memcard.h": "hle",
    "pad_input.cpp": "hle", "pad_input.h": "hle",

    # -- harness/ : SBS, dualcore, dualview, verify, native_diff --------------------------------
    "sbs.cpp": "harness", "sbs.h": "harness",
    "sbs_pane_layout.h": "harness", "sbs_present_sdl.cpp": "harness",
    "dualcore.cpp": "harness", "dualcore.h": "harness",
    "dualview_snapshot.cpp": "harness", "dualview_snapshot.h": "harness",
    "verify_harness.cpp": "harness", "verify_harness.h": "harness",
    "native_diff.cpp": "harness", "native_diff.h": "harness",
    "native_gate.cpp": "harness", "native_gate.h": "harness",
    "selftest.cpp": "harness",

    # -- config/ : cfg/config/config_var (the CVar ladder) --------------------------------------
    "cfg.cpp": "config", "cfg.h": "config",
    "config.cpp": "config", "config.h": "config",
    "config_var.h": "config", "config_vars.h": "config",
    "mods.cpp": "config", "mods.h": "config",   # the live toggle/param model behind the overlay

    # -- ui/ : RmlUi overlay + glue -------------------------------------------------------------
    "rmlui_overlay.cpp": "ui", "rmlui_overlay.h": "ui",
    "rmlui_render_gpu.cpp": "ui", "rmlui_render_gpu.h": "ui",
    "rml_text.cpp": "ui", "rml_text.h": "ui",
    "overlay_glue.cpp": "ui", "overlay_glue.h": "ui",

    # -- dbg/ : dbg_server, fntrace, hostprof — developer tooling -------------------------------
    "dbg_server.cpp": "dbg", "dbg_server.h": "dbg",
    "fntrace.cpp": "dbg", "fntrace.h": "dbg",
    "hostprof.cpp": "dbg", "hostprof.h": "dbg",
    "repl.cpp": "dbg", "repl.h": "dbg",
    "snapshot.cpp": "dbg", "snapshot.h": "dbg",
    "watchdog.cpp": "dbg",
}

# Directories under runtime/recomp/ that move wholesale.
DIR_MOVES = {"shaders_gpu": "gpu/shaders_gpu"}

# In SUBSYS for the include rewrite, but NOT on disk in git: generated, gitignored. The build
# regenerates it at its new path (tools/gen_gpu_shaders.py, rewritten below).
GEN_ONLY = {"gpu_vk_shaders.h"}

SRC_EXT = (".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inc")

OLD_DIR = "runtime/recomp"
NEW_DIR = "runtime"

# ---------------------------------------------------------------------------------------------
# Non-source files that carry runtime/recomp paths. Each rule is (old, new, min_hits). A rule that
# fires ZERO times ABORTS the run: a build edit that silently no-ops because somebody reworded the
# file is precisely the failure that would ship a broken tree while every check printed green.
# ---------------------------------------------------------------------------------------------

def _dst(base, sub):
    return f"{NEW_DIR}/{base}" if sub == "_ROOT_" else f"{NEW_DIR}/{sub}/{base}"


def _psxport_path_rules():
    """Path-literal rules derived from SUBSYS, longest-first so `runtime/recomp/gpu_vk.h` is
    rewritten before the bare `runtime/recomp` fallback can chew the prefix."""
    rules = []
    for base, sub in sorted(SUBSYS.items(), key=lambda kv: -len(kv[0])):
        rules.append((f"{OLD_DIR}/{base}", _dst(base, sub)))
    for d, dst in DIR_MOVES.items():
        rules.append((f"{OLD_DIR}/{d}", f"{NEW_DIR}/{dst}"))
    return rules


# Build scripts and cmake reach the tree through a VARIABLE root (`RT=runtime/recomp`,
# `set(RT runtime/recomp)`) and then append a bare filename. Rewriting only the root would leave
# `$RT/cfg.cpp` pointing at a file that no longer exists — and a shell script fails at BUILD time,
# far from the move, which is exactly the kind of breakage a "paths only" commit is assumed not to
# have. So the variable forms are rewritten too.
VAR_ROOTS = ("$RT", "${RT}", "$RE", "${RE}")


def _var_path_rules():
    rules = []
    for v in VAR_ROOTS:
        for d, dst in DIR_MOVES.items():
            rules.append((f"{v}/{d}", f"{v}/{dst}"))
        for base, sub in sorted(SUBSYS.items(), key=lambda kv: -len(kv[0])):
            if sub == "_ROOT_":
                continue
            rules.append((f"{v}/{base}", f"{v}/{sub}/{base}"))
    return rules


# Applied LAST: the bare directory, for prose and globs that name no file ("all of
# runtime/recomp/**"). It MUST NOT be a plain string replace. `recomp_iface.h` is a _ROOT_ file, so
# the specific rule turns `runtime/recomp/recomp_iface.h` into `runtime/recomp_iface.h` — and a
# plain `runtime/recomp` -> `runtime` then ate the prefix of THAT and produced `runtime_iface.h`.
# The lookahead confines the tail rule to a real path boundary.
TAIL_RE = re.compile(re.escape(OLD_DIR) + r'(?![A-Za-z0-9_])')

VAR_FILE_EXT = (".sh", ".cmake", ".txt", ".bash")


def rules_for(rel):
    r = list(_psxport_path_rules())
    if rel.endswith(VAR_FILE_EXT) or os.path.basename(rel) == "CMakeLists.txt":
        r += _var_path_rules()
    return r

# REQUIRED substitutions, asserted after the generic rules have run. A rule that ends with FEWER
# than `min_hits` occurrences of the NEW string ABORTS the run: a build edit that silently no-ops
# because somebody reworded the file upstream is exactly the failure that ships a broken tree while
# every check prints green.
#
# The two include strings live inside PYTHON STRING LITERALS in the recompiler's emitter, so the
# source-include rewriter (which only touches real `#include` lines in .c/.h files) cannot see
# them — but every emitted shard would fail to compile without them.

# The ONE edit in this move that is not a path substitution, and it is FORCED by the move rather
# than smuggled in with it. `vendor/beetle-psx/mednafen/psx/gte.c` does `#include "gte_state.h"` —
# a FRAMEWORK header, reached by bare name, from a file that lives in a DIFFERENT REPOSITORY
# (beetle-psx is a submodule). The move cannot rewrite it: editing a submodule is a cross-repo
# change, and this commit may not bump a pin. So the one directory that one vendored file needs is
# given to that ONE SOURCE FILE, not to the target and not to consumers — `runtime/cpu` never
# reaches the framework's own translation units, so no psxport source can go back to resolving
# `#include "core.h"` unqualified through it.
#
# Measured, not assumed: `gte_state.h` is the ONLY framework header any file under
# vendor/beetle-psx/mednafen/ includes. mdec_state.h / mdec_device.h / spu_state.h / spu_device.h
# are reached only from psxport's own mdec_beetle.c / spu_beetle.cpp, which the move rewrites.
GTE_SHIM_ANCHOR = "add_library(psxport STATIC ${PSXPORT_FRAMEWORK_SRC} ${SHADERS_H})"
GTE_SHIM = GTE_SHIM_ANCHOR + """

# The vendored Beetle GTE core includes the framework's `gte_state.h` by bare name, and it lives in
# a SUBMODULE this repo may not edit. runtime/cpu/ is therefore put on the include path of that ONE
# source file — deliberately not on the psxport target (PUBLIC or PRIVATE), so the framework's own
# sources still cannot reach a subsystem header unqualified.
set_source_files_properties(${MED}/psx/gte.c PROPERTIES
  INCLUDE_DIRECTORIES ${PSXPORT_ROOT}/runtime/cpu)"""

PSXPORT_BUILD_FILES = {
    "cmake/psxport.cmake": [("set(RT runtime/recomp)", "set(RT runtime)", 1),
                            (GTE_SHIM_ANCHOR, GTE_SHIM, 1),
                            ("${RT}/gpu/shaders_gpu", "${RT}/gpu/shaders_gpu", 2),
                            ("${RT}/gpu/gpu_vk_shaders.h", "${RT}/gpu/gpu_vk_shaders.h", 1)],
    "tools/gen_gpu_shaders.py": [
        ('SHADER_DIR_REL = Path("runtime/recomp/shaders_gpu")',
         'SHADER_DIR_REL = Path("runtime/gpu/shaders_gpu")', 1),
        ('OUTPUT_REL = Path("runtime/recomp/gpu_vk_shaders.h")',
         'OUTPUT_REL = Path("runtime/gpu/gpu_vk_shaders.h")', 1),
    ],
    "tools/fmv_export/build.sh": [("RE=runtime", "RE=runtime", 1),
                                  ("$RE/media/mdec_beetle.c", "$RE/media/mdec_beetle.c", 1),
                                  ("$RE/media/fmv_decode.cpp", "$RE/media/fmv_decode.cpp", 1),
                                  ("$RE/config/cfg.cpp", "$RE/config/cfg.cpp", 1)],
    "tools/recomp/emit.py": [('#include "core.h"', '#include "cpu/core.h"', 2)],
    "tools/port_gen.py": [('#include "core.h"', '#include "cpu/core.h"', 1)],
}

# Documentation that is a NAVIGATION INSTRUMENT (a stale path sends the next session to a file that
# does not exist). Historical record — docs/info/, docs/issues/, docs/journal.md, docs/kanban/,
# docs/deferred/ — is NEVER rewritten: those record what a past session measured, and editing them
# is falsifying the record. They are counted and listed instead.
HISTORICAL_DOC_DIRS = ("docs/info/", "docs/issues/", "docs/kanban/", "docs/deferred/")
HISTORICAL_DOC_FILES = ("docs/journal.md",)


def is_historical(rel):
    return rel.startswith(HISTORICAL_DOC_DIRS) or rel in HISTORICAL_DOC_FILES


# ---------------------------------------------------------------------------------------------
# git helpers
# ---------------------------------------------------------------------------------------------

def git(repo, *args, check=True):
    r = subprocess.run(["git", "-C", repo, *args], capture_output=True, text=True)
    if check and r.returncode != 0:
        raise RuntimeError(f"git {' '.join(args)} in {repo} failed:\n{r.stderr}")
    return r.stdout


def git_ls(repo):
    return [p for p in git(repo, "ls-files", "-z").split("\0") if p]


def git_show(repo, rev, path):
    r = subprocess.run(["git", "-C", repo, "show", f"{rev}:{path}"],
                       capture_output=True)
    if r.returncode != 0:
        return None
    return r.stdout


# ---------------------------------------------------------------------------------------------
# the transformation
# ---------------------------------------------------------------------------------------------

INC_RE = re.compile(r'^([ \t]*#[ \t]*include[ \t]*")([^"/]+)(")', re.M)


def include_map():
    """basename -> the path an OUTSIDER writes for it. `_ROOT_` entries keep their bare name
    (the seam looks identical from every tree, which is the point of keeping it at the root)."""
    m = {}
    for base, sub in SUBSYS.items():
        m[base] = base if sub == "_ROOT_" else f"{sub}/{base}"
    return m


def rewrite_includes(text, own_subsys, imap):
    """Qualify every include of a framework header with its subsystem.

    Same-subsystem includes stay BARE — `#include "foo.h"` already resolves relative to the
    including file's own directory, so a bare include unambiguously means "my own subsystem" and
    needs no extra entry on the include path. That is Dusklight's convention: `src/dusk/ui/*.cpp`
    include siblings bare (`#include "component.hpp"`) and cross-package qualified
    (`#include "dusk/achievements.h"`).  [taken from ~/repo/dusklight/src/dusk/ui/]

    Only bare basenames are matched (`[^"/]+`), so re-running this on an already-moved tree is a
    no-op: the rewrite is idempotent by construction.
    """
    def sub(m):
        base = m.group(2)
        tgt = imap.get(base)
        if tgt is None:
            return m.group(0)
        if own_subsys is not None and SUBSYS.get(base) == own_subsys:
            return m.group(0)          # sibling: leave bare
        return m.group(1) + tgt + m.group(3)
    return INC_RE.sub(sub, text)


def apply_path_rules(text, rules):
    """Longest-first literal path substitution; returns (text, {rule: hits})."""
    hits = {}
    for old, new in rules:
        n = text.count(old)
        if n and old != new:
            text = text.replace(old, new)
        hits[old] = n
    return text, hits


def new_path_for(rel):
    """runtime/recomp/<x> -> its post-move path. None if `rel` is not under the moved dir."""
    if not rel.startswith(OLD_DIR + "/"):
        return None
    tail = rel[len(OLD_DIR) + 1:]
    if "/" in tail:
        top = tail.split("/", 1)[0]
        if top in DIR_MOVES:
            return f"{NEW_DIR}/{DIR_MOVES[top]}/{tail.split('/', 1)[1]}"
        return None
    sub = SUBSYS.get(tail)
    if sub is None:
        return None
    return f"{NEW_DIR}/{tail}" if sub == "_ROOT_" else f"{NEW_DIR}/{sub}/{tail}"


def old_path_for(rel):
    """Inverse of new_path_for, for --verify."""
    if not rel.startswith(NEW_DIR + "/"):
        return None
    tail = rel[len(NEW_DIR) + 1:]
    for d, dst in DIR_MOVES.items():
        if tail.startswith(dst + "/"):
            return f"{OLD_DIR}/{d}/{tail[len(dst) + 1:]}"
    base = os.path.basename(tail)
    sub = SUBSYS.get(base)
    if sub is None:
        return None
    expect = base if sub == "_ROOT_" else f"{sub}/{base}"
    return f"{OLD_DIR}/{base}" if tail == expect else None


def subsys_of_worktree_path(rel):
    """Which subsystem does a post-move psxport file live in? None for anything outside runtime/."""
    if not rel.startswith(NEW_DIR + "/"):
        return None
    tail = rel[len(NEW_DIR) + 1:]
    return tail.split("/", 1)[0] if "/" in tail else "_ROOT_"


# ---------------------------------------------------------------------------------------------
# the inertness proof
# ---------------------------------------------------------------------------------------------

STRIP_RE = re.compile(
    rb'^(?:'
    rb'[ \t]*#[ \t]*include[ \t]*"[^"]*"'   # a quoted #include: the move rewrites these
    rb'|.*\bruntime\b'                      # any line naming the tree the move renames
    rb').*$\r?\n?', re.M)


def strip_hash(blob):
    """Hash a file with every line the move is ALLOWED to rewrite DELETED, and report how many
    lines that was.

    Two kinds of line qualify, both deleted rather than normalised, so what remains is exactly the
    bytes that must be byte-identical:
      1. quoted `#include "..."` lines — the include rewrite;
      2. any line containing the token `runtime` — the self-referential file-header comments
         (`// runtime/recomp/boot.cpp — the MAIN.EXE loader`) and prose references that the move
         rewrites, in both their `runtime/recomp/x.cpp -> runtime/cpu/x.cpp` and their bare
         `runtime/recomp -> runtime` forms. The rule has to be this blunt to be SYMMETRIC: a
         narrower pattern matched the before-line and missed the after-line, which showed up as 12
         phantom "content changed" failures the first time this ran.

    Angle-bracket includes stay INSIDE the hash — the move never touches them.

    THE BLIND SPOT, sized rather than hand-waved: `--verify` prints how many lines of how many were
    excluded. Check B (pure re-derivation from git) covers every excluded line byte-exactly, which
    is why both checks run and why the selftest tampers with an include line specifically."""
    kept = STRIP_RE.sub(b"", blob)
    n_all = blob.count(b"\n") + (0 if blob.endswith(b"\n") or not blob else 1)
    n_kept = kept.count(b"\n") + (0 if kept.endswith(b"\n") or not kept else 1)
    return hashlib.sha256(kept).hexdigest(), n_all, n_all - n_kept


class Report:
    def __init__(self):
        self.lines = []
        self.failed = False

    def say(self, s=""):
        self.lines.append(s)
        print(s)

    def fail(self, s):
        self.failed = True
        self.say("FAIL: " + s)


# ---------------------------------------------------------------------------------------------
# tree discovery
# ---------------------------------------------------------------------------------------------

def find_games(psxport_root, explicit):
    """Locate the game trees whose game/ includes must be rewritten in lockstep.

    Returns (found, searched_root, skipped). A caller that prints only `found` is lying: the
    number that matters is how many candidates were LOOKED AT, so both are returned."""
    if explicit:
        return [os.path.abspath(p) for p in explicit], None, []
    # <workspace>/<game>/external/psxport  ->  workspace is three levels up
    ws = os.path.abspath(os.path.join(psxport_root, "..", "..", ".."))
    found, skipped = [], []
    if not os.path.isdir(ws):
        return [], ws, []
    for name in sorted(os.listdir(ws)):
        cand = os.path.join(ws, name)
        if not os.path.isdir(cand):
            continue
        if os.path.isdir(os.path.join(cand, "external", "psxport")) and \
           os.path.isdir(os.path.join(cand, "game")):
            found.append(cand)
        elif os.path.isdir(os.path.join(cand, ".git")):
            skipped.append(cand)
    return found, ws, skipped


# ---------------------------------------------------------------------------------------------
# plan / apply
# ---------------------------------------------------------------------------------------------

def build_plan(psxport_root, rep):
    """Classify every tracked file under runtime/recomp. ABORTS on anything unmapped."""
    tracked = git_ls(psxport_root)
    under = [p for p in tracked if p.startswith(OLD_DIR + "/")]
    moves, unmapped = [], []
    for p in under:
        np = new_path_for(p)
        if np is None:
            unmapped.append(p)
        else:
            moves.append((p, np))
    if unmapped:
        rep.fail(f"{len(unmapped)} tracked file(s) under {OLD_DIR}/ are NOT in the mapping table. "
                 f"main has grown files this transformation has never seen — add one line each to "
                 f"SUBSYS in tools/layout_move.py and re-run. Refusing to move a partial tree.")
        for p in unmapped:
            rep.say("    unmapped: " + p)
        raise SystemExit(2)
    return tracked, moves


def collision_check(psxport_root, games, rep):
    """A game tree with its own header named like a framework header would have its include
    silently re-pointed at the framework. Re-checked at APPLY time, every time, because main and
    the games both keep moving."""
    imap = include_map()
    bad = 0
    for g in games:
        own = set()
        gdir = os.path.join(g, "game")
        for dp, _dn, fn in os.walk(gdir):
            for f in fn:
                if f.endswith((".h", ".hpp", ".hh", ".hxx", ".inc")):
                    own.add(f)
        clash = sorted(own & set(imap))
        rep.say(f"  collision check {os.path.basename(g)}/game: {len(own)} local headers, "
                f"{len(clash)} clash with framework basenames")
        for c in clash:
            rep.fail(f"    {os.path.basename(g)}/game has its own '{c}', which is ALSO a framework "
                     f"header. Rewriting `#include \"{c}\"` there would re-point it. Rename one.")
            bad += 1
    return bad == 0


def in_scope(rel, do_docs=True):
    """The ONE definition of what the transformation may touch. --apply and --verify both call it,
    so a file cannot be rewritten by one and expected untouched by the other — a scope split is
    what turned two stale CLAUDE.md references into "NOT a pure re-derivation" the first time this
    ran, and a scope split in the other direction would hide a real edit."""
    if rel.startswith("vendor/") or rel.startswith("generated/"):
        return False
    if is_historical(rel):
        return False            # the record of what a past session measured is never rewritten
    if rel.endswith(".md") and not do_docs:
        return False
    return True


def transform(rel, text, imap, psxport):
    """The whole per-file transformation, in ONE place, so --apply and --verify cannot drift.
    Sources get their includes qualified AND their path-citing comments updated; everything else
    gets path literals only."""
    out = text
    if rel.endswith(SRC_EXT):
        own = subsys_of_worktree_path(rel) if psxport else None
        out = rewrite_includes(out, own, imap)
    out, _ = apply_path_rules(out, rules_for(rel))
    out = TAIL_RE.sub(NEW_DIR, out)
    if psxport:
        for old, new, _need in PSXPORT_BUILD_FILES.get(rel, []):
            # `new not in out` keeps this IDEMPOTENT. It matters: the gte.c shim's replacement
            # CONTAINS its own anchor, so a second run would nest it inside itself.
            if old != new and new not in out:
                out = out.replace(old, new)
    return out


def rewrite_tree(root, rels, imap, psxport, dry, rep, label):
    """Apply `transform` to every file. Returns the list of files it changed."""
    changed = []
    for rel in rels:
        path = os.path.join(root, rel)
        if not os.path.isfile(path) or os.path.islink(path):
            continue
        try:
            text = open(path, "rb").read().decode("utf-8")
        except (UnicodeDecodeError, OSError):
            continue
        out = transform(rel, text, imap, psxport)
        if out != text:
            changed.append(rel)
            if not dry:
                open(path, "wb").write(out.encode("utf-8"))
    rep.say(f"  {label}: {len(changed)} of {len(rels)} files rewritten")
    return changed


def do_apply(psxport_root, games, dry, rep, do_docs=True):
    imap = include_map()

    dirty = git(psxport_root, "status", "--porcelain", "--", OLD_DIR, NEW_DIR).strip()
    if dirty and not dry:
        rep.fail("psxport runtime/ is DIRTY. The move must land on a clean tree, or the inertness "
                 "proof is measuring somebody else's edit:\n" + dirty)
        raise SystemExit(2)

    tracked, moves = build_plan(psxport_root, rep)
    rep.say(f"psxport: {len(moves)} tracked files under {OLD_DIR}/ -> {NEW_DIR}/<subsystem>/")
    per = {}
    for _o, n in moves:
        per.setdefault(n.split("/")[1] if "/" in n[len(NEW_DIR) + 1:] else "_ROOT_", []).append(n)
    for k in sorted(per):
        rep.say(f"    {NEW_DIR}/{k:<10} {len(per[k]):>3} files")

    if not collision_check(psxport_root, games, rep):
        raise SystemExit(2)

    if dry:
        rep.say("\n--plan: nothing written.")
        return moves

    # 1. git mv, so `git log --follow` and blame survive. A delete+add would throw away most of
    #    the framework's history in one commit.
    for sub in sorted({n.rsplit("/", 1)[0] for _o, n in moves}):
        os.makedirs(os.path.join(psxport_root, sub), exist_ok=True)
    for o, n in moves:
        git(psxport_root, "mv", o, n)
    rep.say(f"  git mv: {len(moves)} files moved")

    # 1b. the generated, gitignored header that used to be emitted into runtime/recomp/. It is
    #     rebuilt at its new path by tools/gen_gpu_shaders.py; the stale copy is debris that would
    #     otherwise keep runtime/recomp/ alive as an empty directory.
    for base in GEN_ONLY:
        stale = os.path.join(psxport_root, OLD_DIR, base)
        if os.path.exists(stale):
            os.remove(stale)
            rep.say(f"  removed stale generated {OLD_DIR}/{base} (rebuilt at "
                    f"{NEW_DIR}/{SUBSYS[base]}/{base})")

    # 1c. git tracks files, not directories, so `git mv` leaves the emptied runtime/recomp/ (and its
    #     shaders_gpu/) on disk. An empty directory with the OLD name is a trap for the next `ls`.
    old_abs = os.path.join(psxport_root, OLD_DIR)
    for dp, _dn, _fn in os.walk(old_abs, topdown=False):
        # `_dn` is stale after the children are removed, so test the directory itself.
        if os.path.isdir(dp) and not os.listdir(dp):
            os.rmdir(dp)
    if not os.path.isdir(old_abs):
        rep.say(f"  removed the emptied {OLD_DIR}/ directory tree")

    # 2. rewrite the framework: sources, build files, tools, tests, nav docs.
    rels = [r for r in git_ls(psxport_root) if in_scope(r, do_docs)]
    rewrite_tree(psxport_root, rels, imap, True, dry, rep, "psxport")

    # 3. assert every REQUIRED build-file string is present AFTERWARDS. Checking the result rather
    #    than the substitution means a rule that became a no-op upstream still trips it.
    n_assert = 0
    for rel, reqs in PSXPORT_BUILD_FILES.items():
        p = os.path.join(psxport_root, rel)
        if not os.path.isfile(p):
            rep.fail(f"{rel}: listed in PSXPORT_BUILD_FILES but absent from the tree — the build "
                     f"file was renamed or deleted upstream; update the rule table.")
            continue
        txt = open(p, encoding="utf-8", errors="replace").read()
        for _old, new, need in reqs:
            n_assert += 1
            if txt.count(new) < need:
                rep.fail(f"{rel}: expected >= {need} occurrence(s) of {new!r} after the move, "
                         f"found {txt.count(new)}. The build file was reworded upstream; fix the "
                         f"rule rather than shipping a no-op.")
    rep.say(f"  build-file assertions: {n_assert} required strings checked across "
            f"{len(PSXPORT_BUILD_FILES)} files")
    if rep.failed:
        raise SystemExit(2)

    # 4. rewrite the game trees. SAME scope rule as the framework — a game's CLAUDE.md and its
    #    codemap are navigation instruments too, and a stale path in one sends the next session to
    #    a file that no longer exists.
    for g in games:
        grels = [r for r in git_ls(g) if in_scope(r, do_docs)]
        rewrite_tree(g, grels, imap, False, dry, rep, os.path.basename(g))

    # 5. the substrate each game already has on disk was emitted by the OLD emit.py and still says
    #    `#include "core.h"`. generated/ is SACROSANCT — this reports it and names the fix rather
    #    than editing it, and a silent omission here surfaces later as a baffling
    #    `generated/rec_decls.h:3: fatal error: core.h: No such file or directory`.
    stale_generated(games, rep)

    # 6. the denominator: what still says runtime/recomp anywhere.
    leftovers(psxport_root, games, rep)
    return moves


def stale_generated(games, rep):
    rep.say("\nSTALE `generated/` SUBSTRATE (emitted by the pre-move emit.py):")
    if not games:
        rep.say("  no game trees were given, so NOTHING was inspected — this is not a clean bill "
                "of health, it is an empty search.")
        return
    for g in games:
        gen = os.path.join(g, "generated")
        if not os.path.isdir(gen):
            rep.say(f"  {os.path.basename(g)}: no generated/ on disk — nothing to restamp")
            continue
        n = tot = 0
        for dp, _dn, fn in os.walk(gen):
            for f in fn:
                if not f.endswith((".c", ".h", ".cpp")):
                    continue
                tot += 1
                try:
                    if '#include "core.h"' in open(os.path.join(dp, f), encoding="utf-8",
                                                   errors="replace").read():
                        n += 1
                except OSError:
                    pass
        verdict = "MUST be re-emitted" if n else "already clean"
        rep.say(f"  {os.path.basename(g)}: {n} of {tot} generated files carry the old bare "
                f"`#include \"core.h\"` — {verdict}")
        if n:
            rep.say(f"      fix: cd {g} && ./run.sh    "
                    f"(ensure_recomp.py hashes emit.py, so the edit above already invalidates the "
                    f"stamp; PSXPORT_FORCE_RECOMP=1 forces it). Do NOT hand-edit generated/.")


def leftovers(psxport_root, games, rep):
    rep.say("\nRESIDUAL `runtime/recomp` MENTIONS (the denominator — an empty list here would be "
            "the interesting answer, so it is always printed with its counts):")
    for root, name in [(psxport_root, "psxport")] + [(g, os.path.basename(g)) for g in games]:
        code, hist, other = [], [], []
        for rel in git_ls(root):
            if rel.startswith("vendor/"):
                continue
            p = os.path.join(root, rel)
            if not os.path.isfile(p) or os.path.islink(p):
                continue
            try:
                t = open(p, encoding="utf-8").read()
            except (UnicodeDecodeError, OSError):
                continue
            # Boundary-aware, for the same reason TAIL_RE is: `runtime/recomp_iface.h` CONTAINS the
            # substring `runtime/recomp` and a naive `in` test reported it as a leftover.
            if not TAIL_RE.search(t):
                continue
            if is_historical(rel):
                hist.append(rel)
            elif rel.endswith(SRC_EXT) or rel.endswith((".cmake", ".sh", ".py")) or \
                    rel in ("CMakeLists.txt", ".gitignore"):
                code.append(rel)
            else:
                other.append(rel)
        rep.say(f"  {name}: code/build {len(code)}, prose {len(other)}, "
                f"HISTORICAL (never rewritten, by design) {len(hist)}")
        for r in code:
            rep.fail(f"    {name}: {r} still mentions {OLD_DIR}/ in a CODE/BUILD file")
        for r in other[:20]:
            rep.say(f"    prose (left, harmless): {name}: {r}")


# ---------------------------------------------------------------------------------------------
# verify
# ---------------------------------------------------------------------------------------------

def verify_repo(root, rev, imap, psxport, rep, name, do_docs=True):
    """Check A (rewritable-line-stripped hash multiset) + check B (pure-function re-derivation)."""
    before_files = [p for p in git(root, "ls-tree", "-r", "--name-only", rev).splitlines() if p]
    before_set = set(before_files)
    now = [p for p in git_ls(root) if not p.startswith("vendor/")]

    # --- file-set accounting: nothing lost, nothing conjured -----------------------------------
    if psxport:
        expect_after = set()
        for p in before_files:
            if p.startswith("vendor/"):
                continue
            expect_after.add(new_path_for(p) or p)
        have = set(now)
        lost = sorted(expect_after - have)
        extra = sorted(have - expect_after)
        rep.say(f"  [{name}] file set: {len(expect_after)} expected after the move, "
                f"{len(have)} present, {len(lost)} missing, {len(extra)} unexpected")
        for p in lost[:20]:
            rep.fail(f"    [{name}] MISSING after move: {p}")
        for p in extra[:20]:
            rep.fail(f"    [{name}] UNEXPECTED (not produced by the transformation): {p}")

    # --- check A: rewritable-line-stripped hash multiset ---------------------------------------
    from collections import Counter
    hb, ha = [], []
    pairs = []
    tot_lines = tot_stripped = 0
    for rel in now:
        if not rel.endswith(SRC_EXT):
            continue
        old = old_path_for(rel) if psxport else None
        src = old if (old and old in before_set) else rel
        blob = git_show(root, rev, src)
        if blob is None:
            rep.fail(f"    [{name}] {rel}: no pre-move content at {rev}:{src} — a NEW source file "
                     f"rode along inside the move commit")
            continue
        cur = open(os.path.join(root, rel), "rb").read()
        h0, n0, s0 = strip_hash(blob)
        h1, n1, s1 = strip_hash(cur)
        hb.append(h0)
        ha.append(h1)
        tot_lines += n1
        tot_stripped += s1
        pairs.append((src, rel, h0, h1))
    mism = [(o, n) for o, n, a, b in pairs if a != b]
    same_multiset = Counter(hb) == Counter(ha)
    pct = (100.0 * tot_stripped / tot_lines) if tot_lines else 0.0
    rep.say(f"  [{name}] check A  content hashes: {len(pairs)} source files compared, "
            f"{len(set(hb))} distinct hashes before / {len(set(ha))} after, "
            f"multiset identical: {same_multiset}, per-file mismatches: {len(mism)}")
    rep.say(f"  [{name}] check A  blind spot SIZE: {tot_stripped} of {tot_lines} lines "
            f"({pct:.2f}%) were excluded as rewritable (#include or a `runtime` path); the other "
            f"{tot_lines - tot_stripped} lines are hashed byte-exactly")
    if not same_multiset:
        rep.fail(f"    [{name}] hash MULTISET differs — content changed inside the move")
    for o, n in mism[:25]:
        rep.fail(f"    [{name}] content changed: {o} -> {n}")

    # --- check B: pure-function re-derivation --------------------------------------------------
    # Covers EVERY tracked text file, in scope or out. Out of scope means the transformation had no
    # licence to touch it at all, so the expectation is byte-identity with `rev` — which is how a
    # stray edit to a file nobody thought was involved gets caught.
    n_ok = n_bad = n_untouched = 0
    for rel in now:
        old = (old_path_for(rel) if psxport else None) or rel
        if old not in before_set:
            continue
        blob = git_show(root, rev, old)
        if blob is None:
            continue
        try:
            text = blob.decode("utf-8")
        except UnicodeDecodeError:
            continue
        scoped = in_scope(rel, do_docs)
        expect = transform(rel, text, imap, psxport) if scoped else text
        try:
            cur = open(os.path.join(root, rel), encoding="utf-8").read()
        except (UnicodeDecodeError, OSError, IsADirectoryError):
            continue
        if cur == expect:
            n_ok += 1
            if not scoped:
                n_untouched += 1
        else:
            n_bad += 1
            if n_bad <= 15:
                rep.fail(f"    [{name}] NOT a pure re-derivation: {rel} differs from "
                         f"{'the transformation of' if scoped else 'the UNTOUCHED'} {rev}:{old}")
    rep.say(f"  [{name}] check B  pure re-derivation: {n_ok} files byte-identical to the "
            f"transform of {rev} ({n_untouched} of them out-of-scope and required to be "
            f"UNCHANGED), {n_bad} divergent")
    rep.say(f"  [{name}] BLIND SPOTS: check A ignores #include lines and any line naming a "
            f"`runtime` path (check B covers those byte-exactly). NEITHER check sees: a file "
            f"untracked in both trees, anything under vendor/, or a non-UTF-8 file.")


def do_verify(psxport_root, games, rev, rep):
    imap = include_map()
    rep.say(f"INERTNESS PROOF against {rev}")
    verify_repo(psxport_root, rev, imap, True, rep, "psxport")
    for g in games:
        verify_repo(g, "HEAD", imap, False, rep, os.path.basename(g))


# ---------------------------------------------------------------------------------------------
# selftest — prove the inertness check can produce the OTHER answer
# ---------------------------------------------------------------------------------------------

SELFTEST_FILES = {
    "runtime/recomp/core.h": '#pragma once\n#include "cfg.h"\n#include <cstdint>\nint kMagic = 7;\n',
    "runtime/recomp/gpu_vk.cpp": '#include "core.h"\n#include "cfg.h"\nint draw(){return kMagic;}\n',
    "runtime/recomp/cfg.cpp": '#include "cfg.h"\nint cfg(){return 1;}\n',
    "runtime/recomp/cfg.h": '#pragma once\n',
    "cmake/psxport.cmake": 'set(RT runtime/recomp)\nset(S ${R}/runtime/recomp/gpu_vk.cpp)\n',
}


def do_selftest(rep):
    """Three runs on a synthetic repo: clean must PASS, a body edit must FAIL, an include-line
    tamper must FAIL. A checker that has never been seen red proves nothing about what it covers."""
    tmp = tempfile.mkdtemp(prefix="layout_move_selftest_")
    try:
        for rel, body in SELFTEST_FILES.items():
            p = os.path.join(tmp, rel)
            os.makedirs(os.path.dirname(p), exist_ok=True)
            open(p, "w").write(body)
        git(tmp, "init", "-q")
        git(tmp, "config", "user.email", "selftest@local")
        git(tmp, "config", "user.name", "selftest")
        git(tmp, "add", "-A")
        git(tmp, "commit", "-q", "-m", "before")

        imap = include_map()

        def run_case(label, tamper):
            sub = subprocess.run([sys.executable, __file__, "--_selftest_apply", tmp],
                                 capture_output=True, text=True)
            if sub.returncode != 0:
                rep.fail(f"selftest[{label}]: transform failed:\n{sub.stdout}\n{sub.stderr}")
                return None
            if tamper:
                tamper(tmp)
            r = Report()
            verify_repo(tmp, "HEAD", imap, True, r, f"selftest:{label}")
            git(tmp, "reset", "-q", "--hard", "HEAD")
            git(tmp, "clean", "-qfd")
            return r.failed

        clean = run_case("clean", None)

        def body_edit(root):
            p = os.path.join(root, "runtime/cpu/core.h")
            open(p, "a").write("int rode_along = 1;\n")
        body = run_case("body-edit", body_edit)

        def inc_edit(root):
            p = os.path.join(root, "runtime/gpu/gpu_vk.cpp")
            t = open(p).read().replace('#include "cpu/core.h"', '#include "cpu/core.h"\n#include "evil.h"')
            open(p, "w").write(t)
        inc = run_case("include-tamper", inc_edit)

        rep.say("")
        rep.say(f"SELFTEST  clean run failed? {clean}   (must be False)")
        rep.say(f"SELFTEST  body edit failed? {body}    (must be True — check A must catch it)")
        rep.say(f"SELFTEST  include tamper failed? {inc} (must be True — check B must catch it)")
        ok = (clean is False) and (body is True) and (inc is True)
        if not ok:
            rep.fail("SELFTEST DID NOT PRODUCE BOTH ANSWERS — the inertness proof cannot be trusted.")
        else:
            rep.say("SELFTEST OK: the inertness proof produces PASS on a clean move and FAIL on "
                    "both a body edit and an include-line tamper.")
        return ok
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def _selftest_apply(root):
    """Minimal apply used by --selftest (no game trees, no build-file assertions)."""
    rep = Report()
    imap = include_map()
    _tracked, moves = build_plan(root, rep)
    for sub in sorted({n.rsplit("/", 1)[0] for _o, n in moves}):
        os.makedirs(os.path.join(root, sub), exist_ok=True)
    for o, n in moves:
        git(root, "mv", o, n)
    rels = [r for r in git_ls(root)]
    rewrite_tree(root, rels, imap, True, False, rep, "selftest")
    p = os.path.join(root, "cmake/psxport.cmake")
    if os.path.isfile(p):
        t = open(p).read().replace("set(RT runtime/recomp)", "set(RT runtime)")
        open(p, "w").write(t)
    return 0


# ---------------------------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--plan", action="store_true")
    ap.add_argument("--apply", action="store_true")
    ap.add_argument("--verify", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--before-ref", default="HEAD",
                    help="the PRE-MOVE revision for --verify (default HEAD: run it before you commit)")
    ap.add_argument("--game", action="append", default=[],
                    help="explicit game-repo root (repeatable). Default: auto-discover siblings.")
    ap.add_argument("--no-docs", action="store_true",
                    help="skip navigation docs (historical docs/info, docs/issues, docs/journal.md, "
                         "docs/kanban are NEVER touched either way)")
    ap.add_argument("--_selftest_apply", metavar="ROOT")
    a = ap.parse_args()

    if a._selftest_apply:
        return _selftest_apply(a._selftest_apply)

    rep = Report()
    if a.selftest:
        return 0 if do_selftest(rep) else 1

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    if not os.path.isdir(os.path.join(root, "runtime")):
        print(f"not a psxport checkout: {root}", file=sys.stderr)
        return 2

    games, ws, skipped = find_games(root, a.game)
    rep.say(f"psxport root : {root}")
    rep.say(f"workspace    : {ws}")
    rep.say(f"game trees   : {len(games)} found" +
            (f" ({', '.join(os.path.basename(g) for g in games)})" if games else ""))
    if not games:
        rep.say("  NOTE: ZERO game trees matched `<ws>/*/external/psxport` + `<ws>/*/game`. "
                "The game-side include rewrite will therefore do NOTHING — pass --game explicitly "
                "if that is wrong. This is printed loudly because a silent zero here ships a "
                "framework whose consumers no longer compile.")
    if skipped:
        rep.say(f"  (searched {len(games) + len(skipped)} candidate repos; skipped: "
                f"{', '.join(os.path.basename(s) for s in skipped)})")
    rep.say("")

    if a.plan:
        do_apply(root, games, True, rep)
    elif a.apply:
        do_apply(root, games, False, rep, do_docs=not a.no_docs)
        rep.say("")
        do_verify(root, games, a.before_ref, rep)
    elif a.verify:
        do_verify(root, games, a.before_ref, rep)
    else:
        ap.print_help()
        return 0

    rep.say("")
    rep.say("RESULT: " + ("FAILED — see the FAIL lines above." if rep.failed else "OK"))
    return 1 if rep.failed else 0


if __name__ == "__main__":
    sys.exit(main())
