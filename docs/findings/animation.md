# Findings — Animation subsystem (game/object/animation.cpp)

## FIXED: `anim_unpack_pose_triple` ate a shared nibble, corrupting every subsequent per-limb pose field (2026-07-08)

- **Symptom**: after `register_engine_overrides()` was fixed to actually run for SBS/DualCore/Selftest's
  own Games (commit b078729) and the GTE `Math` cluster was wired (commit 7187c93), `PSXPORT_SBS_MODE=full`
  went from a fake trivial 0-diff (both cores silently ran the substrate) to **84,218 sbs-div lines,
  first at frame 61**, in the object-pool region `0x800F2Bxx..0x800F5xxx`, cascading through the whole run.
- **Bisection** (register_engine_overrides / Animation::registerOverrides, one cluster disabled at a time,
  full rebuild + `PSXPORT_SBS_MODE=full` rerun each step):
  - Disabling `c->math.registerOverrides()` alone: divergence UNCHANGED (byte-identical first-diff) — Math
    is not the cause (ruled out, despite being the prime suspect from the commit history).
  - Disabling `c->engine.animation.registerOverrides()` entirely: **0 sbs-div through 7400+ frames** —
    isolates the whole divergence to the Animation cluster (loadFrame/advanceLinkChain/attach,
    0x80076904/0x80077B5C/0x80077C40).
  - Disabling only `loadFrame`'s own `ov.register_`: divergence UNCHANGED — because `loadFrame` is also
    reached directly as a plain C++ call from `Animation::attach`/`Animation::step`, independent of its
    own EngineOverrides entry (own registration only matters for *other* callers that reach it via
    `rec_dispatch`).
  - Disabling only `advanceLinkChain`: divergence UNCHANGED — not the cause.
  - Disabling only `attach` (0x80077C40): **0 sbs-div through 7400+ frames** — isolates the bug to
    `Animation::attach` (which internally calls the native `loadFrame`).
- **Root cause, RE'd byte-for-byte** (`sbs bt`/`sbs diff` + `PSXPORT_SBS_PREWATCH=0x800F2BCC` +
  manual `r <addr>` reads of the live guest RAM at the pause, no guessing): for the call
  `Animation::attach(node=0x800FB960, table=0x80017FE8, id=2)` (SOP-intro-lifted actor's `anim_env_setup`,
  ra=0x8010B830), `loadFrame` resolves `rec = 0x9100166E` at entryPtr 0x80161204 → `flagsByte=0x91`
  (bit 0x40 clear, bit 0x80 set) → Loop2 (parity-gated per-limb unpack), `phase=1`, and
  `anim_unpack_pose_triple` consumes 5 stream bytes at 0x8016283A (`00 00 07 00 0F`) for the
  obj+0x88/0x8a/0x8c pose triple.
  - `anim_unpack_pose_triple`'s 5-byte window packs **three 12-bit fields into 40 bits**: v88 = e0
    (8 bits) + e1's high nibble (4 bits); v8a = e1's low nibble (4 bits) + e2 (8 bits); v8c = e3
    (8 bits) + **e4's high nibble** (4 bits) = 36 bits total. **e4's LOW nibble is never consumed** —
    it's a shared/straddling nibble that belongs to the very next 12-bit-field reader (exactly why
    `phase` seeds to 1 when this unpack runs: "there's a pending nibble").
  - The old code did `stream = s + 5` — a full byte-advance that **silently discards e4's pending low
    nibble** instead of leaving `stream` pointing AT e4 so the next reader (Loop2's ODD-phase first
    field) can re-read that byte and mask off the nibble.
  - Hand-simulating BOTH formulas against the live bytes (stream continuing at 0x8016283F: `E1 04 A0 9A FF`)
    proved the OLD (buggy, `+5`) native code exactly reproduces what SBS reported as the WRONG (`A`) side:
    `f8=0x0104, f10=0x0A09, f12=0x0AFF` → bytes `04 01 09 0A FF 0A` (matches the observed native divergence
    byte-for-byte). Re-deriving with `stream = s + 4` (bytes `0F E1 04 A0 9A`) gives
    `f8=0x0FE1, f10=0x004A, f12=0x009A` → bytes `E1 0F 4A 00 9A 00` — **byte-identical to the substrate's
    (`B`) side** from the same `sbs diff` dump.
- **Fix**: `game/object/animation.cpp`, `anim_unpack_pose_triple`: `stream = s + 5;` → `stream = s + 4;`.
  One-line fix, no magic constant — the `+4` is the exact bit-accounting fact (36 bits consumed of 40
  available), not a tuned offset.
- **Post-fix verification**: `PSXPORT_SBS_MODE=full` (autonav, headless, NOAUDIO): sbs-div count dropped
  from 84,218/132k+ (varies run-to-run due to autonav RNG) to **56 lines total**, ALL now in a totally
  different region (`0x801FE90C`, guest-stack scratch — see residual below), clearing entirely by frame
  117 and staying byte-identical through frame 9570+ (full run length under the 90s headless cap).
- **Residual RESOLVED (2026-07-08)**: `0x801FE90C..0x801FE910` (4 B, later widened to the full
  `0x801FE908..0x801FE914` 3-word frame) diverged f61-f116 only: `A=00 00 00 00` vs `B=<stack value,
  e.g. 0x800ECF58>`.
  - **RE** (`generated/shard_5.c` `gen_func_80077C40`, the compiled body of `Animation::attach`):
    prologue does `sp -= 32; mem_w32(sp+24, ra); mem_w32(sp+20, r17); mem_w32(sp+16, r16);` where
    `r17 = r4 + r0` (its own `node` argument, copied verbatim) and `r16` = a table-lookup pointer.
    The epilogue reloads all three from the SAME offsets then `sp += 32` and returns. Its two leaf
    callees (`func_80076904`/loadFrame, `func_80075FF8`) never touch `sp` at all — confirmed by
    scanning both bodies for `r[29]` writes (none). So the ENTIRE set of writes attach's compiled
    body makes to its own stack frame is these 3 words, and the ONLY reads of them are its own
    epilogue's restore, before the frame is popped.
  - **Verified DEAD** (not waved off): `PSXPORT_SBS_BYTETRACE=0x801FE900,0x801FE920
    PSXPORT_SBS_BYTETRACE_ALL=1` over an autonav run classified every byte in the 32-byte frame
    window CLEAN/PHASE/SOFT-PHASE (0 REAL) — value sets + per-value counts match within tolerance
    for the whole run, meaning both cores visit the SAME set of values at that address; nothing
    reads the stale spill as live data (a live consumer would show up as a REAL/ONE-SIDED byte).
    Native `Animation::attach` (game/object/animation.cpp) is a plain C++ call with no guest stack
    frame, so A leaves the slots at cold-boot 0 while attach is repeatedly re-entered loading one
    scene's actors (f61-f116); the frame converges the instant ordinary (matching, non-attach)
    traffic reuses that stack depth at f117, and stays byte-identical through 11,900+ further
    frames in the verification run.
  - **Fix**: `runtime/recomp/sbs.cpp` — added `Sbs::Impl::isDeadStackScratch(addr)`, a narrow,
    unconditional (non-pc_skip-gated) exclusion for exactly `0x801FE908..0x801FE914`, wired into
    every RAM-compare call site (`checkDivergence`'s `scan`, the rewind-pause first-byte loop,
    `summarizeDivergence`'s masked-byte counter) alongside the existing `isPcSkipScratch`. Same
    class of exception `Animation::step`'s own-frame exclusion already carves out
    (animation.cpp:46, gated behind the `animvm` per-call verify channel); attach has no per-call
    A/B harness of its own, so the exclusion lives in `sbs.cpp` instead, scoped to exactly this
    3-word range (not a broad mask).
  - **Post-fix verification**: `PSXPORT_SBS_MODE=full` autonav, two independent runs (headless,
    NOAUDIO, ~90 s each, reaching f8600-f9100+): **0 sbs-div lines**, all frames report
    `A/B identical (mode=full)`.
- **Bisection method note**: `register_engine_overrides()` disabling one `ov.register_` line at a time,
  full rebuild each step, is slow (~2 min/cycle) but the ONLY reliable way to isolate which of several
  co-registered natives caused a cascading divergence — direct in-process A/B diffing inside an
  EngineOverrides handler (`rec_super_call` re-entry from inside the handler itself) is UNSAFE: it
  corrupts the coroutine/fiber scheduler's `c->pc`/stack bookkeeping and hangs/crashes (tried once here,
  reverted). Use `PSXPORT_SBS_PREWATCH=<addr>` + `sbs bt`/`sbs diff` + manual `r <addr>` guest-RAM reads
  instead — safe, and gives the exact last-writer `pc`/`ra` (though `pc` is STALE/unreliable for a write
  made from inside a native EngineOverrides handler, since `rec_dispatch` returns before the substrate's
  `func_X` trampoline would have stamped `c->pc = X` — a real but out-of-scope tooling gap, noted for
  workflow follow-up, not fixed here).
- **refs**: commits b078729 (register_engine_overrides fix), 7187c93 (Math wiring) — the two prior
  commits that made this gate honest; this fix commit; `game/object/animation.cpp`.
