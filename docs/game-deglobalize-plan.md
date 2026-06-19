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
- **P4 (done):** pad_input.cpp — host button state `s_buttons` + REPL drive `s_repl_*` → `PadState`
  (`c->game->pad`). Threaded `Core*` through `pad_init/pad_set_buttons/pad_buttons/pad_fill_buffer/
  pad_poll_sdl/pad_repl_hold/pad_repl_tap/pad_repl_release/pad_overrides_init` and their callers
  (boot, dbg_server via `s_ctx`, native_fmv `fmv_pace(core,…)`, native_stub `ov_stub_vsync(c)`,
  native_boot repl + auto-navigator). Test-hook/config-cache statics inside `pad_service_frame`
  (`s_fc`, FORCE_* env caches) left shared per policy. Dropped the dead `PSXPORT_PAD_NO_OVERRIDES`
  standalone path (no build uses it). 0-diff ✓.
- **P5 (done):** native_fmv.cpp — Start-skip edge flag `s_fmv_start_prev` → `FmvState::start_prev`
  (`core->game->fmv`). The SDL audio-sink handles (`s_fmv_dev/freq/bytes_per_frame`) stay file-scope
  **shared by design** — host audio-output singleton, not guest machine state, so a lockstep RAM diff
  is unaffected (documented in-code). 0-diff ✓ (FMV not exercised at the NO_FMV frame-50 gate, but the
  change is a pure variable relocation with identical semantics).
- **P6 (done):** native_stub.cpp — boot-stub state → `StubState` (`c->game->stub`): `g_stub_vblank`→
  `vblank`, `g_main_path`→`main_path`, `g_stub_exit`(jmp_buf)→`exit_jmp`. **Eliminated `g_boot_ctx`**
  (redundant — every user is an override carrying the boot `Core* c`, so it reloads MAIN into `c`).
  All users had `c` in scope; setjmp/longjmp now on `c->game->stub.exit_jmp`. 0-diff ✓.
- **P7 (done — AUDIT, nothing migrated):** interp.cpp — audited all file-scope statics; NONE are
  per-instance mutable game state. All are either debug/trace (stderr/file, cfg-gated: g_trace_fp,
  g_ncall_*, g_ldhaz*, g_ifn_*, g_callring, g_interp_pc) or config-caches (spindbg, sg_*) → stay
  shared per policy. The override dispatch table `g_iov`/`g_iov_n` (+ `g_overlay_load_hook`) stays
  SHARED by design: build-level dispatch (175 init sites register the same fns at the same addrs;
  the only runtime churn is overlay flush/rescan, lockstep-identical across cores). The dual-core
  diff expresses its per-core difference (terrain ON vs neutralized) as a **flag on Game read inside
  the override** (override has Core* c), NOT divergent tables — so no migration needed. Rationale
  documented in-code above `g_iov`. (No build/0-diff needed — comment-only change.)
- **R1 (done):** gpu_native.cpp render machine state → `GpuState` on Game (`c->game->gpu`). ALL mutable
  render state moved (VRAM `s_vram`+`s_interp`+`s_prov`/`s_provmeta`, draw clip/offset/texpage/CLUT/texwin,
  display `s_disp_*`, GP0 FIFO+xfer, `s_prim_order`/`s_prim_gid`, `s_ndl_cur`, `s_frame`, `s_cur_node`,
  `g_ot_madr`, `g_dma_src`). The rasterizer fns are now **methods of GpuState** (field names kept their
  `s_`-spelling so bodies are byte-unchanged — a scripted signature-only transform, scratch/r1_gpu_native.py).
  Public C API kept stable via thin free-fn wrappers forwarding to `core->game->gpu`. Threaded `Core*` into
  the carved-out diag TUs (gpu_trace `trace_record`/`trace_flush`/`gputrace_arm`, gpu_debug `gpu_prov_dump`/
  `gpu_provat_display`/`gpu_scene_dump*`) + dbg_server + fps60's synth path (`fps60_synthesize`/dumptest) +
  the Core*-less `gpu_gp1`/`gpu_frame_no`/`gpu_native_*`/`gpu_repaint`/`gpu_fps60_*`. Cross-file `s_frame`
  reads (engine_submit, margin_render, game_tomba2) → `gpu_frame_no(c)`. **DEFERRED to R2:** `s_seen3d`/
  `s_prev_had3d` stay file-scope shared (read by gpu_vk's Core*-less present path; move when gpu_vk is
  threaded). Diagnostics/host-singletons stay shared per policy (s_prims, s_fade_*, s_cw_*, texwatch,
  s_sbs_on, SDL s_win/s_ren/s_tex, g_nd_*). Gates: frame-50 RAM 0-diff vs baseline ✓ AND field-frame 520
  VK render byte-identical pre-vs-post ✓.
- **R2 (done):** gpu_vk.cpp per-frame render state → `GpuVkState` on Game (`core->game->gpu_vk`). MOVED
  (now members, `s_`-spelling kept so bodies are byte-unchanged): batch counters `s_tri_n`/`s_tex_n`/
  `s_semi_n`, current prim depth/order `s_vd`/`s_vdn`/`s_cur_ord`/`s_cur_ordn`, semi grouping `s_semi_grp[]`/
  `s_semi_grp_n`/`s_sg_*`, dirty regions `s_dirty[]`/`s_dirty_n`, present origin `s_present_sx/sy`, diag
  snapshots `s_dbg_*`/`s_last_*`. The touching fns are now **GpuVkState methods** (scripted, body-preserving:
  scratch/r2_gpu_vk.py) — set_*/semi_group/stats/dirty/present/draw_*/shot/dump/frame_end/tritest + the
  internal helpers panel_upload/panel_render/ssao_pass/tex_emit/tri_*_readback/frame_via_fb. Public C API
  kept stable via thin **wrappers** taking `Core*` (gpu_vk.h, the single decl site — scattered local
  forward-decls in the gp0 tee dropped). Panel/TexVtx made named structs so they can be forward-declared in
  the header. **`s_seen3d`/`s_prev_had3d` (R1 deferral) moved onto GpuState** (writer side stays byte-
  identical); `gpu_seen3d_this_frame`/`gpu_had3d_last_frame` now take `Core*`; `frame_via_fb` reads them via
  a `Game*` back-pointer added to GpuVkState (and to GpuState, used by `blit_src` → `&game->core`). Threaded
  callers: gpu_native gp0 tee + present (have `core`), boot.cpp (tritest moved AFTER `new Game()`, passes
  `c`), dbg_server (shot/stats take `s_ctx`). STAYS SHARED (documented): all Vulkan device/swapchain/
  pipeline/buffer handles (one VK device per process), config-caches (s_vk_on/s_sbs/sbs_on), `s_rawdump_*`.
  Gates: frame-50 RAM 0-diff vs baseline ✓ AND field-frame 520 VK render byte-identical pre-vs-post ✓.
- **R3 (done — AUDIT, nothing migrated):** gpu_trace.cpp + gpu_debug.cpp. gpu_debug.cpp has ZERO file-scope
  statics (already Core*-threaded in R1). gpu_trace.cpp's statics (s_trace_on/frames/count/idx/multi/path/
  init/words/cap/n/inited/arm_path) are the PSXPORT_GPUTRACE GP0-capture state: gated on the trace flag,
  read-only w.r.t. guest RAM (it snapshots VRAM + records GP0 words, then writes a trace FILE for gpu_differ).
  They do NOT affect Core::ram, so per the "Debug/trace statics" policy they STAY SHARED (two lockstep cores
  are both unarmed/no-op or armed identically; the capture buffers can't diverge core.ram). No code change.
- **Next (order):** ~~gpu_native~~(R1) → ~~gpu_vk~~(R2) → ~~gpu_trace/gpu_debug~~(R3 audit) — THEN dbg_server →
  native_boot → gte/spu/mdec (Beetle FORK) → engine modules (fps60, engine_submit, native_path*,
  game_tomba2). (mem 1 static, boot 1 — sweep at the end.) THEN the dual-core diff + the submit_terrain fix.
  DONE/skip: timing, cd_override, hle, pad_input, native_fmv, native_stub, interp (done); sync_overrides,
  threads, memcard (only config-caches).

## Render-subsystem migration (USER DECISION 2026-06-19: "migrate render fully first")
Asked defer-vs-migrate for the renderer, the user chose **migrate render fully first** (honor "nothing
file-scope global" literally; enables future per-core render-output diffing). Biggest chunk (~164 of
~459 statics) and **cross-module**: the VRAM/display state is NOT mere file-scope statics — `s_vram`,
`s_prov`, `s_disp_x/y` are plain globals exported via `runtime/recomp/gpu_native_internal.h` and
consumed by gpu_native, gpu_vk, gpu_trace, gpu_debug.
- **Design:** put ALL render state on a `GpuState` member of `Game` (VRAM `s_vram`+`s_interp`+`s_prov`,
  `s_fb_base`, draw clip `s_da_*`, offset `s_off_*`, tex mode `s_tp_*`, CLUT `s_clut_*`, GP0 parse
  `s_fcount/s_fneed/s_xfer*`, tex window `s_tw_*`, display `s_disp_*`, prim counters `s_prim_*`,
  `s_seen3d/s_prev_had3d`, `s_ndl_cur`, …). Reach it via `core->game->gpu` from entry points that
  already carry `Core*` (`gpu_gp0(Core*)`, `gp0_exec(Core*)`, `gpu_present(Core*)`).
- **Threading crux:** LEAF helpers (`fb()`, `sample_tex`, GP0 sub-handlers, `blit_src`, `gpu_gp1` which
  has NO Core*) carry no handle. Re-thread them to take `GpuState&`/`Core*` (mechanical but large in
  gpu_native.cpp), and update the internal header + cross-file consumers (gpu_vk/gpu_trace/gpu_debug)
  that read `s_vram`/`s_prov`. Existing OOP direction ([[oop-core-refactor-directive]], cpp-scope memory)
  favors making the heavy GPU fns **methods of GpuState** — clean but a big rewrite; method-ize or pass
  `GpuState&`.
- **Stay SHARED (documented host-output singletons / config / diag, NOT machine state):** SDL window/
  renderer/texture (`s_win/s_ren/s_tex/s_tex_w/s_tex_h/s_win_on`); Vulkan device/swapchain in gpu_vk are
  the same category (host GPU singletons, one per process). Config-caches (`s_sbs_on`, `g_log`,
  `s_reddbg`) and pure diagnostics (`s_prims`, `s_gp0_words`, `s_dma2`, `s_cur_node`, `s_fade_*`) stay
  shared per policy. Document each in-code.
- **GATE:** frame-50 0-diff (`scratch/abrun.sh` vs `deglob_baseline.bin`) AND a later FIELD-frame render
  check (terrain runs in the field, not by frame 50) — capture a field frame headless before & after,
  confirm identical (the "Render-path modules" gate note above).

## After de-globalization
Build the dual-core diff: `Game a, b;` (b neutralizes the override under test, e.g. terrain → super-call),
step both the same frames, compare `core.ram` (excluding the render-pool region 0x800BE000–0x800EC000
which legitimately differs native-vs-recomp), STOP + report the first diverging frame + byte addrs +
the responsible guest function. Then fix `submit_terrain` from that precise evidence.
