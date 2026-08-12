#!/usr/bin/env python3
"""lineage_probe.py — WHOLE-FUNCTION + STRING lineage evidence between PS-EXE executables.

WHY THIS EXISTS SEPARATELY FROM `exe_similarity.py`. That tool answers "what fraction of 16-word
opcode-shape windows do two binaries share". It was distrusted on 2026-08-12 because (a) the denominator
was the SMALLER window set, letting a small library-dominated binary score 22-36% against everything, and
(b) DATA inside .text was decoded as instructions, so jump-offset tables counted as matches. It was
RECALIBRATED the same day — Jaccard, a code/data filter, and a null distribution stratified by PSY-Q
version — and its conclusions now AGREE with this tool's on every pair (see its docstring). This tool
remains the deliberately INDEPENDENT second opinion: it shares no code with it, its failure modes are
different by construction, and it resolves "same architecture, rewritten" better (CRASH2|CRASH3 3.8x here
vs 2.7x there).

WHAT IT MEASURES, and why each choice removes one of the failure modes above:

  1. WHOLE FUNCTIONS, not sliding windows. .text is segmented at `jr $ra` + delay slot, each block is
     validated as code, normalised, and hashed. A shared unit is therefore "an entire function is
     present in both binaries", which is a direct statement about lineage. A 16-word coincidence
     inside two unrelated functions cannot produce a match.
  2. A CODE/DATA GATE with a stated rejection count. A block is only a function if it ends in
     `jr $ra`, contains no undecodable word, is 6..3000 instructions, is >=55% ordinary integer/branch
     opcodes, and is not dominated by one repeated word. Jump tables, pointer tables and padding fail
     this. The gate PRINTS how many blocks it rejected per binary — a silent filter is a lie.
  3. EXCLUSIVITY IS FIRST-CLASS. Every function hash is counted across the WHOLE corpus (its SPREAD).
     A unit in 10 of 17 binaries is the Sony SDK; a unit in 2 or 3 is lineage. The headline number for
     a pair is the count of shared functions whose spread is inside a band, and the band is SWEPT
     (<=2, <=3, <=4) with every level printed — because a strict "exactly 2" hides a family of three
     (Crash 2 + Crash 3 + the Warped demo drops CRASH2|CRASH3 from 57 to 1). Library similarity cannot
     inflate any of these levels, because a library function is by definition not low-spread.
  4. ABSOLUTE COUNTS, NOT A RATIO OF THE SMALLER SET. The primary output is "N shared functions,
     W instruction words" — so a small binary cannot score high by being small. `%min` is printed too,
     but as context, never as the verdict. Read a pair against the MEASURED NULL below, not a
     hand-picked ceiling.
  5. AN INDEPENDENT SECOND CHANNEL: exclusive STRING overlap (>=8-char printable runs, e.g. source
     filenames, PSY-Q version strings, format magic, printf formats). Same exclusivity rule. Strings
     have completely different failure modes from disassembly, so agreement between the two channels
     is real corroboration and disagreement is a finding.

TWO NORMALISATIONS, both reported, because they bracket the answer:
  * STRICT — mask only what MUST change when the same object is linked at another address: j/jal
    targets and `lui` immediates. Everything else (registers, branch offsets, struct offsets,
    constants) is compared bit-exact. STRICT matches mean "the same object code".
  * LOOSE  — additionally mask every non-branch 16-bit immediate. Catches the same source recompiled
    against differently-placed globals, at the cost of being easier to collide. Branch offsets stay
    exact in both, so control-flow shape is always compared.

REFUSALS (exit 2, never a clean empty answer): fewer than 2 inputs; a missing/unreadable file; a file
without `PS-X EXE` magic; a binary yielding fewer than 50 validated functions (reported as "segmented
almost nothing", NOT as "no similarity"). A pair with zero shared functions is reported WITH its
denominators (functions per side, corpus size, blocks rejected) so a negative can never be confused
with "the tool never looked".

SELF-TEST: `--selftest` builds synthetic PS-EXEs — a relocated copy of the same function, a
deliberately different function, and a pointer table — and asserts the relocated pair matches, the
different pair does not, and the pointer table is REJECTED by the code gate. It exits non-zero if any
of those fails, so a broken build cannot report a clean corpus.

MEASURED 2026-08-12 over a 17-binary corpus (13 titles + Tomba!2's loader as a labelled control +
Digimon World, Vagrant Story and Starfighter Sanvein added purely as extra NULLS), all extracted with
`discdump`. Two cross-promo demos (Warped on the Spyro 1 disc, Spyro 2 on the CTR disc) were measured
separately as SAME-CODE-DIFFERENT-BUILD scale controls, then excluded from the matrix because a demo is
a third owner and would suppress its own family's exclusivity.

  THE NULL DISTRIBUTION, which is what the old tool never published: 123 cross-studio pairs.
  STRICT / spread<=3 -- mean 1.02, median 0, 74 of 123 exactly ZERO, MAX 15 (SPYRO1|TOMBA1).
  The max is explained rather than waved away: Spyro 1 and Tomba! 1 link the SAME PSY-Q
  (`sys.c` 1.129 / 1996-12-25), so the residual floor is per-version library variants -- which is
  exactly the thing exclusivity cannot remove when only 2-3 corpus members link that version.

  SAME-ENGINE pairs against that null max of 15 (STRICT/spread<=3, absolute function counts):
      SPIDER1|SPIDER2   246   (16.4x null max)   + 180 exclusive strings
      SPYRO2|SPYRO3      59   ( 3.9x)            +  18 exclusive strings
      CRASH2|CRASH3      57   ( 3.8x)            +   8 exclusive strings
      SPYRO1|SPYRO2/3    27   ( 1.8x)  -- and NOT a PSY-Q artifact: Spyro 1 links 1.129 while
                                          Spyro 2/3 link 1.140, i.e. a different cohort
      CRASH1|CRASH2      25   ( 1.7x)            +  10 exclusive strings incl. the `\\S%X\\S%07X.NSF/.NSD` NSF/NSD format magic
      CRASH1|CRASH3      12   (BELOW the null max -- not separable)
      TOMBA1|TOMBA2       1   (BELOW the null max -- NOT one codebase, and below two random titles)
      CRASHBASH|TOMBA2    3   (BELOW the null max -- the pair exe_similarity.py read as 33.4%)
      TOMBA2_MAIN|TOMBA2_SCUS  0 spread<=3 of 41 shared units; every shared unit sits in >=4 of 17
                               binaries and every one of their 98 shared strings in >=8. The loader
                               and the engine of ONE GAME share only SDK. That pair's 76.8% on the
                               old metric was 100% generic, and "must come out as the same game" is
                               the wrong expectation for it: they are two different programs.

  WHERE IT DISAGREES WITH THE RECORDED PERCENTAGES. Spider 1/2 (74.2%) and Spyro 2/3 (64.2%) both
  hold, but they are NOT comparable strengths: Spider shares 4x more whole functions than Spyro does.
  Tomba! 1/2 at 18.8% and CrashBash/Tomba!2 at 33.4% are both indistinguishable from noise here.
  STRICT separates and LOOSE does not: at LOOSE/spread<=3 the null max rises to 74 (CRASHBASH|SANVEIN)
  while CRASH2|CRASH3 is 137 -- only 1.9x -- so masking immediates buys sensitivity and loses the
  discrimination that matters. Read STRICT first; LOOSE only ranks candidates.

USAGE
    python3 tools/lineage_probe.py --dir scratch/lineage2/exes
    python3 tools/lineage_probe.py NAME=a.exe NAME2=b.exe [--json out.json] [--exclude N,N]
    python3 tools/lineage_probe.py --dir <d> --focus TOMBA1,TOMBA2_MAIN   # one pair, with its
                                                                          # denominators stated
    python3 tools/lineage_probe.py --selftest

Extract inputs with the framework disc tool (never a game's run.sh):
    cmake --build build --target discdump
    ./build/tools/discdump list "/path/disc.chd"
    ./build/tools/discdump get SCUS_942.28 "/path/disc.chd" <outdir>
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import re
import struct
import sys
from collections import Counter, defaultdict

JR_RA = 0x03E00008
MIN_FUNCS = 50
MIN_FN_INSNS = 6
MAX_FN_INSNS = 3000
MIN_ORDINARY_FRAC = 0.55
MAX_REPEAT_FRAC = 0.45
MIN_STR = 8

# ---------------------------------------------------------------------------
# MIPS R3000A decode tables. Only what a PSX compiler/assembler actually emits
# is "valid"; anything else marks the block as data.
# ---------------------------------------------------------------------------
OP_VALID = {
    0x00: 'SPECIAL', 0x01: 'REGIMM', 0x02: 'j', 0x03: 'jal', 0x04: 'beq', 0x05: 'bne',
    0x06: 'blez', 0x07: 'bgtz', 0x08: 'addi', 0x09: 'addiu', 0x0A: 'slti', 0x0B: 'sltiu',
    0x0C: 'andi', 0x0D: 'ori', 0x0E: 'xori', 0x0F: 'lui',
    0x10: 'cop0', 0x11: 'cop1', 0x12: 'cop2', 0x13: 'cop3',
    0x20: 'lb', 0x21: 'lh', 0x22: 'lwl', 0x23: 'lw', 0x24: 'lbu', 0x25: 'lhu', 0x26: 'lwr',
    0x28: 'sb', 0x29: 'sh', 0x2A: 'swl', 0x2B: 'sw', 0x2E: 'swr',
    0x32: 'lwc2', 0x3A: 'swc2',
}
SPECIAL_VALID = {
    0x00, 0x02, 0x03, 0x04, 0x06, 0x07,          # sll srl sra sllv srlv srav
    0x08, 0x09, 0x0C, 0x0D,                      # jr jalr syscall break
    0x10, 0x11, 0x12, 0x13,                      # mfhi mthi mflo mtlo
    0x18, 0x19, 0x1A, 0x1B,                      # mult multu div divu
    0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,  # add addu sub subu and or xor nor
    0x2A, 0x2B,                                  # slt sltu
}
REGIMM_VALID = {0x00, 0x01, 0x10, 0x11}          # bltz bgez bltzal bgezal
# "ordinary" = the opcodes a normal function body is mostly made of.
ORDINARY_OPS = {0x00, 0x01, 0x04, 0x05, 0x06, 0x07, 0x03, 0x08, 0x09, 0x0A, 0x0B,
                0x0C, 0x0D, 0x0E, 0x0F, 0x20, 0x21, 0x23, 0x24, 0x25, 0x28, 0x29, 0x2B}
# instructions whose 16-bit immediate is a PC-relative branch offset (keep it: it is structure)
BRANCH_OPS = {0x04, 0x05, 0x06, 0x07, 0x01}
# control transfer: branches + j/jal (jr/jalr handled via SPECIAL funct in gate())
CONTROL_OPS = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07}


def decodable(w: int) -> bool:
    op = (w >> 26) & 0x3F
    name = OP_VALID.get(op)
    if name is None:
        return False
    if op == 0x00:
        return (w & 0x3F) in SPECIAL_VALID
    if op == 0x01:
        return ((w >> 16) & 0x1F) in REGIMM_VALID
    return True


def norm_word(w: int, loose: bool) -> int:
    op = (w >> 26) & 0x3F
    if op in (0x02, 0x03):              # j/jal — absolute target, must be masked
        return op << 26
    if op == 0x0F:                      # lui — high half of an address, must be masked
        return w & 0xFFFF0000
    if loose and op not in BRANCH_OPS and op >= 0x08:
        return w & 0xFFFF0000           # mask struct offsets / constants too
    return w


# ---------------------------------------------------------------------------
# PS-EXE parsing + function segmentation
# ---------------------------------------------------------------------------
def read_exe(path: str) -> tuple[int, bytes]:
    try:
        with open(path, 'rb') as f:
            data = f.read()
    except OSError as e:
        raise SystemExit(f"REFUSING: cannot read {path}: {e} — segmented NOTHING, compared NOTHING.")
    if data[:8] != b'PS-X EXE':
        raise SystemExit(f"REFUSING: {path} lacks 'PS-X EXE' magic. Nothing was segmented; this is "
                         f"NOT a 0-shared-functions result.")
    t_addr, t_size = struct.unpack_from('<II', data, 0x18)
    text = data[0x800:0x800 + t_size] if 0 < t_size <= len(data) - 0x800 else data[0x800:]
    return t_addr, text


def gate(ws: list[int]) -> str | None:
    """Return None if ws is a plausible function, else the REASON it was rejected."""
    n = len(ws)
    if n < MIN_FN_INSNS:
        return 'too_short'
    if n > MAX_FN_INSNS:
        return 'too_long'
    if ws[-2] != JR_RA:
        return 'no_jr_ra'
    ordinary = 0
    for w in ws:
        if not decodable(w):
            return 'undecodable'
        if ((w >> 26) & 0x3F) in ORDINARY_OPS:
            ordinary += 1
    if ordinary / n < MIN_ORDINARY_FRAC:
        return 'not_ordinary'
    c = Counter(ws)
    top, cnt = c.most_common(1)[0]
    if top != 0 and cnt / n > MAX_REPEAT_FRAC:
        return 'repetitive'
    # Compiled MIPS has short basic blocks (mean ~5-7 insns), so any real function of
    # non-trivial length contains control flow. A pointer/offset table contains none —
    # this is the gate that rejects the `0x80068bd4` jump-offset-table class of match that
    # exe_similarity.py counted as code. Also: a table uses 1-2 distinct opcodes.
    if n >= 20:
        ctl = sum(1 for w in ws if ((w >> 26) & 0x3F) in CONTROL_OPS
                  or (((w >> 26) & 0x3F) == 0 and (w & 0x3F) in (0x08, 0x09)))
        if ctl < 1 + n // 40:
            return 'no_control_flow'
        if len({(w >> 26) & 0x3F for w in ws}) < 4:
            return 'too_few_opcodes'
    return None


def segment(path: str, name: str) -> dict:
    t_addr, text = read_exe(path)
    nw = len(text) // 4
    ws = list(struct.unpack_from('<%dI' % nw, text, 0))
    ends = [i for i, w in enumerate(ws) if w == JR_RA and i + 1 < nw]
    funcs, rejects = [], Counter()
    prev = 0
    for e in ends:
        s = prev
        while s <= e and ws[s] in (0, 0x03E00008):   # skip padding / a stray tail
            s += 1
        blk = ws[s:e + 2]
        prev = e + 2
        why = gate(blk)
        if why:
            rejects[why] += 1
            continue
        funcs.append((t_addr + s * 4, blk))
    raw = open(path, 'rb').read()
    strs = set(m.group(0) for m in re.finditer(rb'[\x20-\x7e]{%d,}' % MIN_STR, raw))
    return dict(name=name, path=path, t_addr=t_addr, words=nw, jr_ra_sites=len(ends),
                funcs=funcs, rejects=rejects, strings=strs)


def fn_keys(b: dict, loose: bool) -> dict:
    """map normalised-function-key -> (count, total instruction words)"""
    out: dict[bytes, list[int]] = {}
    for addr, blk in b['funcs']:
        key = struct.pack('<%dI' % len(blk), *[norm_word(w, loose) for w in blk])
        e = out.setdefault(key, [0, len(blk)])
        e[0] += 1
    return out


# ---------------------------------------------------------------------------
# self-test: prove the instrument fires, and prove the data gate rejects data
# ---------------------------------------------------------------------------
def _exe(words: list[int], t_addr: int) -> bytes:
    h = bytearray(0x800)
    h[0:8] = b'PS-X EXE'
    struct.pack_into('<II', h, 0x18, t_addr, len(words) * 4)
    return bytes(h) + struct.pack('<%dI' % len(words), *words)


def selftest() -> int:
    import tempfile
    def fn(seed: int, hi: int) -> list[int]:
        # addiu sp,sp,-24 / sw ra / lui a0,hi / jal / nop / lw ra / jr ra / addiu sp,sp,24
        body = [0x27BDFFE8, 0xAFBF0014, 0x3C040000 | hi, 0x0C000000 | (hi << 4),
                0x00000000, 0x24020000 | (seed & 0xFFFF), 0x00621821, 0x8FBF0014,
                JR_RA, 0x27BD0018]
        return body * 3
    A = fn(0x1234, 0x8005)
    B = fn(0x1234, 0x8009)          # same function, relocated -> MUST match STRICT
    C = fn(0x9999, 0x8005)          # different constant -> STRICT differs, LOOSE matches
    table = [0x80068bd4 + 4 * i for i in range(200)] + [JR_RA, 0]
    ok = True
    with tempfile.TemporaryDirectory(dir=os.path.dirname(os.path.abspath(__file__))) as d:
        pa, pb, pc = (os.path.join(d, n) for n in ('a.exe', 'b.exe', 'c.exe'))
        open(pa, 'wb').write(_exe(A * 20, 0x80010000))
        open(pb, 'wb').write(_exe(B * 20, 0x80200000))
        open(pc, 'wb').write(_exe(C * 20 + table, 0x80010000))
        sa, sb, sc = (segment(p, n) for p, n in ((pa, 'A'), (pb, 'B'), (pc, 'C')))
        ka, kb, kc = (fn_keys(s, False) for s in (sa, sb, sc))
        la, lc = fn_keys(sa, True), fn_keys(sc, True)
        checks = [
            ('A segmented >0 functions', len(sa['funcs']) > 0),
            ('relocated copy matches STRICT', bool(set(ka) & set(kb))),
            ('different constant differs STRICT', not (set(ka) & set(kc))),
            ('different constant matches LOOSE', bool(set(la) & set(lc))),
            ('pointer table REJECTED by gate', sc['rejects']['no_control_flow'] +
             sc['rejects']['too_few_opcodes'] + sc['rejects']['not_ordinary'] +
             sc['rejects']['undecodable'] + sc['rejects']['repetitive'] > 0),
        ]
        for label, good in checks:
            print(f"  [{'PASS' if good else 'FAIL'}] {label}")
            ok &= good
    print('SELFTEST', 'PASS' if ok else 'FAIL')
    return 0 if ok else 1


# ---------------------------------------------------------------------------
def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('pairs', nargs='*', help='NAME=path entries')
    ap.add_argument('--dir', help='every *.exe in this dir, named by its filename stem')
    ap.add_argument('--json', help='write the full result table here')
    ap.add_argument('--top', type=int, default=25, help='rows of the pair ranking to print')
    ap.add_argument('--selftest', action='store_true')
    ap.add_argument('--focus', action='append', default=[],
                    help='A,B — dump the spread breakdown of every unit this pair shares. '
                         'Repeatable. Prints the negative WITH its denominators.')
    ap.add_argument('--exclude', default='', help='comma-separated names to drop from the corpus')
    a = ap.parse_args()
    if a.selftest:
        return selftest()

    items: list[tuple[str, str]] = []
    if a.dir:
        for p in sorted(glob.glob(os.path.join(a.dir, '*.exe'))):
            items.append((os.path.splitext(os.path.basename(p))[0], p))
    for spec in a.pairs:
        if '=' not in spec:
            raise SystemExit(f"REFUSING: '{spec}' is not NAME=path. Nothing was compared.")
        n, p = spec.split('=', 1)
        items.append((n, p))
    if a.exclude:
        drop = set(a.exclude.split(','))
        have = {n for n, _ in items}
        if drop - have:
            raise SystemExit(f"REFUSING: --exclude names not in corpus: {sorted(drop - have)}")
        items = [(n, p) for n, p in items if n not in drop]
    if len(items) < 2:
        raise SystemExit(f"REFUSING: {len(items)} input(s). Exclusivity needs a CORPUS; a pair in "
                         f"isolation cannot tell lineage from library. Nothing was compared.")

    bins = []
    for n, p in items:
        b = segment(p, n)
        if len(b['funcs']) < MIN_FUNCS:
            raise SystemExit(f"REFUSING: {p} segmented only {len(b['funcs'])} functions from "
                             f"{b['words']} words / {b['jr_ra_sites']} jr-ra sites — that is "
                             f"'segmented almost nothing', NOT 'no similarity'. rejects={dict(b['rejects'])}")
        bins.append(b)

    N = len(bins)
    print(f"CORPUS: {N} executables. Every pair count below is out of {N*(N-1)//2} pairs.\n")
    print(f"{'binary':<24}{'words':>8}{'jr_ra':>7}{'funcs':>7}{'uniq':>7}{'insn':>8}  rejected(by reason)")
    for b in bins:
        k = fn_keys(b, False)
        print(f"{b['name']:<24}{b['words']:>8}{b['jr_ra_sites']:>7}{len(b['funcs']):>7}"
              f"{len(k):>7}{sum(len(x[1]) for x in b['funcs']):>8}  {dict(b['rejects'])}")
    print()

    results = {}
    names = [b['name'] for b in bins]
    for loose in (False, True):
        keysets = {b['name']: fn_keys(b, loose) for b in bins}
        owners: dict[bytes, list[str]] = defaultdict(list)
        for n, ks in keysets.items():
            for k in ks:
                owners[k].append(n)
        label = 'LOOSE' if loose else 'STRICT'
        hist = Counter(len(v) for v in owners.values())
        print(f"=== {label}: function-hash SPREAD histogram "
              f"(#binaries containing a unit -> #units)")
        print('   ' + '  '.join(f"{k}:{hist[k]}" for k in sorted(hist)))
        print(f"    total distinct units {len(owners)}\n")
        nf = {n: len(keysets[n]) for n in names}
        # NOTE ON WHY A BAND AND NOT JUST "EXACTLY 2": a function shared by a FAMILY of three
        # (Crash 2, Crash 3 and the Warped demo) is not exclusive-to-2, so a strict ==2 filter
        # reports the real Crash pair as ~0. The band is swept and every level printed, so the
        # reader sees whether a pair's evidence is spread-sensitive instead of being handed one
        # cut-off. A unit in <=4 of 19 binaries still cannot be Sony library code.
        for maxown in (2, 3, 4):
            pair_fn: dict[tuple, list[int]] = defaultdict(lambda: [0, 0])
            for k, v in owners.items():
                if not 2 <= len(v) <= maxown:
                    continue
                w = keysets[v[0]][k][1]
                for i in range(len(v)):
                    for j in range(i + 1, len(v)):
                        pr = tuple(sorted((v[i], v[j])))
                        pair_fn[pr][0] += 1
                        pair_fn[pr][1] += w
            rows = []
            for i in range(len(names)):
                for j in range(i + 1, len(names)):
                    pr = tuple(sorted((names[i], names[j])))
                    c, w = pair_fn.get(pr, (0, 0))
                    rows.append((c, w, pr))
            rows.sort(reverse=True)
            nz = [r for r in rows if r[0] > 0]
            print(f"{label} / spread<={maxown}: pair ranking by shared functions whose owner set "
                  f"is 2..{maxown} of {N}")
            print(f"{'pair':<40}{'fn':>7}{'insn':>9}{'min_fn':>8}{'%min':>7}")
            for c, w, pr in rows[:a.top]:
                m = min(nf[pr[0]], nf[pr[1]])
                print(f"{pr[0]+' | '+pr[1]:<40}{c:>7}{w:>9}{m:>8}{100.0*c/m:>6.1f}%")
            print(f"  ...{len(rows)} pairs; {len(nz)} with >0; {len(rows)-len(nz)} with exactly 0. "
                  f"NULL BAND: median {sorted(r[0] for r in rows)[len(rows)//2]}, "
                  f"90th pct {sorted(r[0] for r in rows)[int(len(rows)*0.9)]}\n")
            results[f'{label}<={maxown}'] = {f"{p[0]}|{p[1]}": dict(fn=c, insn=w)
                                             for c, w, p in rows}

    # ---- independent channel: exclusive strings
    sowners: dict[bytes, list[str]] = defaultdict(list)
    for b in bins:
        for s in b['strings']:
            sowners[s].append(b['name'])
    for maxown in (2, 3):
        ps: dict[tuple, list] = defaultdict(list)
        for k, v in sowners.items():
            if not 2 <= len(v) <= maxown:
                continue
            for i in range(len(v)):
                for j in range(i + 1, len(v)):
                    ps[tuple(sorted((v[i], v[j])))].append(k)
        print(f"=== STRING channel, spread<={maxown}: printable runs >={MIN_STR} chars present in "
              f"2..{maxown} of {N} binaries")
        srows = sorted(((len(v), k) for k, v in ps.items()), reverse=True)
        print(f"{'pair':<40}{'strings':>8}  examples")
        for c, pr in srows[:a.top]:
            ex = sorted(ps[pr], key=len, reverse=True)[:3]
            print(f"{pr[0]+' | '+pr[1]:<40}{c:>8}  " +
                  ' ; '.join(x.decode('latin1')[:40] for x in ex))
        print(f"  ...{len(srows)} pairs have >0; {N*(N-1)//2-len(srows)} have exactly 0. "
              f"Corpus: {len(sowners)} distinct strings.\n")
        results[f'STRINGS<={maxown}'] = {f"{p[0]}|{p[1]}": c for c, p in srows}

    for spec in a.focus:
        try:
            x, y = spec.split(',')
        except ValueError:
            raise SystemExit(f"REFUSING: --focus '{spec}' is not A,B. Nothing was reported.")
        if x not in names or y not in names:
            raise SystemExit(f"REFUSING: --focus {spec}: not in corpus {names}.")
        print(f"\n=== FOCUS {x} | {y}")
        for loose in (False, True):
            ks = {b['name']: fn_keys(b, loose) for b in bins}
            own: dict[bytes, list[str]] = defaultdict(list)
            for n2, kk in ks.items():
                for k in kk:
                    own[k].append(n2)
            shared = [k for k in ks[x] if k in ks[y]]
            by = Counter(len(own[k]) for k in shared)
            lab = 'LOOSE' if loose else 'STRICT'
            print(f"  {lab}: {x} has {len(ks[x])} distinct fns, {y} has {len(ks[y])}; "
                  f"they share {len(shared)} units.")
            print(f"    those {len(shared)} units by corpus spread (#of {N} binaries containing it): "
                  + (', '.join(f'{k}->{by[k]}' for k in sorted(by)) or '(none)'))
            lo = [k for k in shared if len(own[k]) <= 3]
            print(f"    spread<=3 (candidate lineage): {len(lo)} units, "
                  f"{sum(ks[x][k][1] for k in lo)} instruction words")
            if not shared:
                print(f"    NEGATIVE, and here is what was actually examined: "
                      f"{len(bins[names.index(x)]['funcs'])} vs "
                      f"{len(bins[names.index(y)]['funcs'])} gated functions, corpus {N}. "
                      f"This is 'no whole function matched', NOT 'nothing was scanned'.")
        sx2 = set(bins[names.index(x)]['strings']) & set(bins[names.index(y)]['strings'])
        sown: dict[bytes, int] = Counter()
        for b in bins:
            for st in b['strings']:
                sown[st] += 1
        bys = Counter(sown[st] for st in sx2)
        print(f"  STRINGS: share {len(sx2)}; by spread: "
              + (', '.join(f'{k}->{bys[k]}' for k in sorted(bys)) or '(none)'))
        ex = sorted((st for st in sx2 if sown[st] <= 3), key=len, reverse=True)[:8]
        print("    spread<=3 examples: " + (' ; '.join(e.decode('latin1')[:52] for e in ex) or '(none)'))

    if a.json:
        with open(a.json, 'w') as f:
            json.dump(dict(corpus=[dict(name=b['name'], funcs=len(b['funcs']),
                                        words=b['words'], rejects=dict(b['rejects']))
                                   for b in bins], results=results), f, indent=1)
        print(f"\nwrote {a.json}")
    return 0


if __name__ == '__main__':
    sys.exit(main())
