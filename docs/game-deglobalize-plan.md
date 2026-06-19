# De-globalization → `Game` (nothing global) — plan & status

**User directive (2026-06-19):** make a big `Game` that holds EVERYTHING so **nothing is a file-scope
global**. This lets TWO cores run in one process in lockstep (overrides-ON vs overrides-OFF) and diff
their state to find the FIRST divergence — the tool that will then root-cause the `submit_terrain`
bug (Tomba falls through the floor in the fisherman cutscene; f400 model corruption). Sequence chosen:
**full nothing-global FIRST, then build the in-process dual-core diff, then diff-driven terrain fix.**

## Architecture
- `class Game` (runtime/recomp/game.h) OWNS `Core core` (CPU regs + 2 MB RAM + scratchpad) **plus**
  every subsystem's state as a `*State` sub-struct member. `Game()` sets `core.game = this`.
- `Core` keeps a back-pointer `Game* game`. The interpreter + generated shards already thread
  `Core* c` everywhere, so we do NOT re-thread the CPU handle. Subsystem code that holds a `Core* c`
  reaches migrated state via `c->game->...`; new subsystem code may take `Game*` directly.
- `boot.cpp`: `Game* game = new Game(); Core* c = &game->core;` — single instance today; two instances
  later for the diff harness.

## The gate (MANDATORY each phase — a pure refactor must not change behavior)
- **0-diff:** `scratch/abrun.sh scratch/raw/deglob_pN.bin 50` then `cmp` vs `scratch/raw/deglob_baseline.bin`
  (AUTO_NEWGAME, frame 50). Must be byte-identical. Baseline captured at the clean pre-refactor HEAD.
- **Render-path modules** (gpu_*, fps60) also need a later-frame check (terrain runs in the field, not
  by frame 50): capture a field frame headless before & after and confirm identical
  (`PSXPORT_AUTO_GAMEPLAY=1 ... PSXPORT_VK_SHOTSEQ="400:520:..."`, compare PPMs).
- Build with `tools/build_port.sh <file>`; keep its SRC list + run.sh in sync when adding files.
- **GOTCHA (critical):** the object cache does NOT track header deps. ANY change to `core.h` or
  `game.h` (e.g. adding a struct/member) requires a **full** rebuild: `tools/build_port.sh all`.
  A partial rebuild leaves mixed object files disagreeing on `Core`/`Game` layout → memory corruption
  → SIGSEGV in interpreted guest code (looks unrelated). Always `build_port.sh all` per phase.

## Global inventory (file-scope `static` counts; targets, hardest last)
runtime/recomp: gpu_vk 107, gpu_native 46, gte_beetle 20, interp 16, native_fmv 12, dbg_server 12,
gpu_trace 11, hle 10, native_stub 9, cd_override 8, native_boot 7, imgui_overlay 6, pad_input 5,
memcard 5, sync_overrides 4, threads 3, timing 2, mem 1, boot 1. (~459 lines; many are CONST tables /
fn-ptr tables / string literals that are read-only and may stay shared — only MUTABLE state must move.)
engine: fps60 56, native_path 37, engine_submit 36, native_path_b1 20, game_tomba2 15, … (~200).
**Beetle GTE wrinkle:** the GTE registers live in the vendored fork (`mednafen/psx/gte.c`
`gteCONTROL/gteDATA` globals), not in gte_beetle.cpp. De-globalizing GTE means either making gte.c's
state a per-instance struct (modify the fork) or snapshot/restore around core switches — decide when
reaching that phase. Same likely for SPU/MDEC (Beetle cores).

## Policy: what stays shared
- **CONST / read-only tables** (jump tables, fn-ptr dispatch tables filled once at init, name strings):
  identical across cores → stay `static const` shared.
- **Config-caches** (`g_cd_verbose` etc.: read an env/cfg flag once at init, then only read): effectively
  const, identical across cores → leave shared (document with a `// shared: config-cache` note).
- **Debug/trace statics** (under `cfg_dbg`/`PSXPORT_*_DBG` gates, only touch stderr — `static uint32_t prev`
  in trace blocks): do NOT affect `Core::ram`; deferred (migrate last, or leave shared). Note them.
- **Everything else (mutable game/machine state)** MUST move onto `Game`.

## CONST vs MUTABLE
Only **mutable** state needs to move. Read-only tables (jump tables, name strings, fn-ptr dispatch
tables that are filled once at init) can stay `static const` shared — they're identical across cores.
Flag init-once-then-read tables case by case; when in doubt, move it (safe).

## Phase log (each: move statics → `*State` in game.h, rewrite refs to `c->game->st`,
## `build_port.sh all`, 0-diff vs baseline, commit+push)
- **P0 (done):** `Game` container + `Core::game` back-ptr + boot uses `new Game()`. Empty wrapper. 0-diff ✓.
- **P1 (done):** timing.cpp — `g_vblank` → `TimingState::vblank`. 0-diff ✓.
- **P2 (done):** cd_override.cpp — deferred-music `s_pending_music/s_pm_*` → `CdState`. 0-diff ✓.
- **P3 (done):** hle.cpp — event blocks `s_ev`, BIOS heap `s_blk/s_nblk/s_heap_*`, `s_work_ok`,
  `s_int_handler`, `s_irq_enabled` → `HleState`. Threaded `c` through heap_*/ev_index/work_area_init
  and the `hle_deliver_event(c,...)` signature (rippled to memcard/native_stub/timing/native_boot;
  card_deliver_complete(c), frame_tick(c)). 0-diff ✓. (sync_overrides/threads/memcard have NO migratable
  state — only config-caches.)
- **Next (order, small→large):** pad_input → hle → threads → memcard → native_fmv → native_stub →
  interp → gpu_native → gpu_trace → dbg_server → native_boot → gpu_vk → gte/spu/mdec (Beetle fork) →
  engine modules (fps60, engine_submit, native_path*, game_tomba2). sync_overrides has NO mutable
  statics (skip). Watch for non-Core-threaded callers (e.g. pad_repl_hold/tap) — thread the instance.

## After de-globalization
Build the dual-core diff: `Game a, b;` (b neutralizes the override under test, e.g. terrain → super-call),
step both the same frames, compare `core.ram` (excluding the render-pool region 0x800BE000–0x800EC000
which legitimately differs native-vs-recomp), STOP + report the first diverging frame + byte addrs +
the responsible guest function. Then fix `submit_terrain` from that precise evidence.
