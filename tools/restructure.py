#!/usr/bin/env python3
"""restructure.py — re-sort the arbitrary native_path*.cpp grab-bags into subsystem files.

The ~176 natives in engine/native_path{,_a1,_a2,_a3,_b1..b5}.cpp were batch-extracted into files
named by extraction batch ("a1","b3"), not by subsystem. They are all `static` and currently
ORPHANED (the override table that reached them was removed), and each is self-contained (it only
calls others via rec_dispatch(<addr>) / call_fn — never a direct C call), so moving them cannot
change runtime behaviour; the only risk is a build break, which `build_port.sh all` catches.

This tool reads each grab-bag, classifies every function by its implemented guest address (address
ranges map to subsystems because the original linker groups each translation unit contiguously) with
a few keyword refinements, and rewrites them into subsystem-named files. Helpers (call_fn / zfill_words
/ clip_word / seed_array) are emitted into whichever target needs them. The grab-bags are emptied.

  tools/restructure.py --plan     # print the proposed file -> functions assignment, write nothing
  tools/restructure.py --apply    # perform the move (then update build_port.sh/run.sh SRC lists)
"""
import os, re, sys, collections

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GRABBAGS = ["native_path.cpp"] + [f"native_path_{b}.cpp" for b in
            ("a1", "a2", "a3", "b1", "b2", "b3", "b4", "b5")]
DEF_RE = re.compile(r'^\s*(?:static\s+)?(?:inline\s+)?(?:void|int|uint32_t|int32_t)\s+(\w+)\s*\(')
NAMEHEX = re.compile(r'^ov_([0-9A-Fa-f]{6,8})$')

# Subsystem classification. Each rule: (target_file, predicate(addr:int, sym, desc)->bool).
# Order matters — first match wins. addr is the implemented guest address (int) or 0 if unknown.
def in_rng(a, lo, hi): return lo <= a < hi

RULES = [
    # libc / runtime helpers (memset/memcpy/bzero/str*/rand/isqrt/atan2/pseudo-rng)
    ("engine/clib.cpp", lambda a, s, d: a in (
        0x8009A340, 0x8009A3E0, 0x8009A420, 0x8009A450, 0x8009A480, 0x8009A540, 0x8009A640,
        0x80079528, 0x80077FB0, 0x80085690, 0x800976C8)),
    # GTE matrix/vector math
    ("engine/gte.cpp", lambda a, s, d: a in (0x80084250, 0x80084470, 0x800847B0)),
    # peripherals: pad / memcard / SIO / BCD-time / rumble  (PSX platform -> runtime/recomp)
    ("runtime/recomp/peripheral_misc.cpp", lambda a, s, d: a in (
        0x8008A00C, 0x8008A110, 0x80089BAC, 0x80089E1C, 0x80089F68, 0x80089B98, 0x8008AC34,
        0x8008B040, 0x8008B19C, 0x8008BBC8, 0x8008BEAC, 0x80003A4C, 0x8005082C, 0x800508A8)),
    # libgpu: primitive/tpage/draw-mode words, sprite/quad prims, GPU packet build/finalize,
    # GPU heap alloc, viewport/clip descriptors, cel-upload completion flags
    ("engine/gpu_lib.cpp", lambda a, s, d: (
        in_rng(a, 0x80082000, 0x80084000) or a in (
            0x80083DE0, 0x80083B30, 0x80083BF0, 0x8007E9C8, 0x800834A0, 0x80085900,
            0x8009C9D0, 0x8009CAEC, 0x80050738, 0x80097760, 0x800977C0, 0x80097A90,
            0x80099370, 0x80099450, 0x80099478, 0x800974FC, 0x8009A1D0, 0x80097678))),
    # sound / voice: channel/voice/pan/key-on, sound-bank request, voice tables
    ("engine/sound_voice.cpp", lambda a, s, d: (
        in_rng(a, 0x80090000, 0x80096400) or a in (0x800782F0, 0x80097540, 0x80097E10, 0x80097E40))),
    # object-pool / control-block init (the 0x8007xxxx zero/seed cluster) + scheduler-frame helpers
    ("engine/object_init.cpp", lambda a, s, d: (
        in_rng(a, 0x8007A000, 0x8007C000) or a in (
            0x8004FB20, 0x800796DC, 0x8007982C, 0x800798F8, 0x80051794, 0x800752B4,
            0x80075E04, 0x80051F80))),
]
FALLBACK = "engine/native_misc.cpp"   # honest catch-all for genuinely-miscellaneous global pokes

# helper source (emitted into any target file that uses the helper)
HELPERS = {
    "call_fn": "static inline void call_fn(Core* c, uint32_t fn) { rec_dispatch(c, fn); }\n",
    "zfill_words": None,    # captured verbatim from native_path.cpp at parse time
    "clip_word": None,      # from native_path_a2.cpp
    "seed_array": None,     # from native_path_b3.cpp
}
FILE_HDR = {
    "engine/clib.cpp": "// engine/clib.cpp — PC-native libc-style helpers the engine RE references "
        "(memset/memcpy/bzero/strcmp/strncmp/strlen, the LCG rand, integer sqrt, atan2).",
    "engine/gte.cpp": "// engine/gte.cpp — PC-native GTE matrix/vector transforms.",
    "runtime/recomp/peripheral_misc.cpp": "// peripheral_misc.cpp — small PSX-platform peripheral "
        "natives (pad/SIO command push, memcard retry wrappers, BCD<->frame time, rumble).",
    "engine/gpu_lib.cpp": "// engine/gpu_lib.cpp — PC-native libgpu-style primitive builders "
        "(draw-mode/tpage/sprite words, GPU packet build/finalize, GPU heap alloc, cel-upload flags).",
    "engine/sound_voice.cpp": "// engine/sound_voice.cpp — PC-native sound/voice helpers "
        "(channel/voice attribute & pan/volume tables, key-on/off, sound-bank requests).",
    "engine/object_init.cpp": "// engine/object_init.cpp — PC-native object-pool / control-block "
        "init (the boot-time zero/seed cluster) and scheduler-frame helpers.",
    FALLBACK: "// engine/native_misc.cpp — miscellaneous PC-native global/state pokes that do not "
        "form a larger subsystem (getter/setters, scratchpad scatter, small table inits).",
}


def parse_grabbag(path):
    """Yield (sym, addr_int, text, uses_set) for each top-level function; also capture named helpers."""
    rel = os.path.relpath(path, ROOT)
    lines = open(path, encoding="utf-8", errors="replace").read().splitlines(keepends=True)
    items, helpers = [], {}
    i = 0
    while i < len(lines):
        m = DEF_RE.match(lines[i])
        if not m:
            i += 1
            continue
        sym = m.group(1)
        # comment block immediately above
        c = i - 1
        cm = []
        while c >= 0 and lines[c].lstrip().startswith("//"):
            cm.append(lines[c]); c -= 1
        cm.reverse()
        # body until brace balance closes
        depth, started, j = 0, False, i
        while j < len(lines):
            depth += lines[j].count("{") - lines[j].count("}")
            if "{" in lines[j]:
                started = True
            if started and depth <= 0:
                break
            j += 1
        text = "".join(cm) + "".join(lines[i:j + 1])
        uses = set(re.findall(r'\b(call_fn|zfill_words|clip_word|seed_array)\b', "".join(lines[i:j + 1])))
        if sym in HELPERS:
            helpers[sym] = "".join(lines[i:j + 1])
        else:
            nh = NAMEHEX.match(sym)
            addr = int(nh.group(1), 16) if nh else 0
            items.append(dict(sym=sym, addr=addr, text=text, uses=uses, src=rel,
                              desc=" ".join(x.strip("/ \n") for x in cm)[:80]))
        i = j + 1
    return items, helpers


def classify(it):
    for tgt, pred in RULES:
        if pred(it["addr"], it["sym"], it["desc"]):
            return tgt
    return FALLBACK


def main():
    apply = "--apply" in sys.argv
    items, helpers = [], {}
    for g in GRABBAGS:
        p = os.path.join(ROOT, "engine", g)
        if os.path.exists(p):
            its, hs = parse_grabbag(p)
            items += its
            helpers.update(hs)
    HELPERS.update({k: v for k, v in helpers.items() if v})

    groups = collections.defaultdict(list)
    for it in items:
        groups[classify(it)].append(it)

    if not apply:
        for tgt in sorted(groups):
            g = sorted(groups[tgt], key=lambda x: (x["addr"], x["sym"]))
            print(f"\n### {tgt}  ({len(g)} fns)")
            for it in g:
                print(f"   0x{it['addr']:08X} {it['sym']:24s} <- {it['src']:24s} {it['desc'][:46]}")
        print(f"\nTotal {len(items)} functions -> {len(groups)} files.")
        return

    for tgt in sorted(groups):
        g = sorted(groups[tgt], key=lambda x: (x["addr"], x["sym"]))
        used_helpers = set().union(*(it["uses"] for it in g)) if g else set()
        out = [FILE_HDR.get(tgt, f"// {tgt} — PC-native natives."), "",
               '#include "core.h"', '#include "cfg.h"', "#include <stdint.h>", "#include <stdio.h>",
               "", "void rec_dispatch(Core*, uint32_t);", "void rec_syscall(Core*, uint32_t);", ""]
        for h in ("call_fn", "zfill_words", "clip_word", "seed_array"):
            if h in used_helpers and HELPERS.get(h):
                out.append(HELPERS[h].rstrip("\n")); out.append("")
        for it in g:
            out.append(it["text"].rstrip("\n")); out.append("")
        dest = os.path.join(ROOT, tgt)
        open(dest, "w").write("\n".join(out) + "\n")
        print(f"wrote {tgt}: {len(g)} fns" + (f"  helpers={sorted(used_helpers)}" if used_helpers else ""))
    # empty the grab-bags (delete the files)
    for g in GRABBAGS:
        p = os.path.join(ROOT, "engine", g)
        if os.path.exists(p):
            os.remove(p)
            print(f"removed {os.path.relpath(p, ROOT)}")


if __name__ == "__main__":
    main()
