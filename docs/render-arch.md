# Render & present architecture — read before touching graphics/VK

How a frame gets from GP0 commands to the screen, the VK renderer, depth, and how to run headless.
Pairs with `gfx-debug.md` (the debug workflow) and `config.md` (flags). VK is THE renderer; there is
ONE render behavior (native per-pixel depth always on, no oracle to diff against).

## The GP0 → screen path
1. The game (recomp + native engine) builds an ordering-table (OT) of GP0 primitive packets in guest RAM.
2. `ov_draw_otag` (game/game_tomba2.cpp) → `gpu_dma2_linked_list` (gpu_native.cpp) walks the OT and calls
   **`gp0_exec`** (gpu_native.c) per primitive. `gp0_exec` decodes each packet (poly/sprite/fill/copy/
   upload/env) — this is the single chokepoint where every drawn primitive passes.
3. **VK is THE renderer:** `gp0_exec` **tees** each poly/sprite prim to the GPU via
   `gpu_gpu_draw_tritri` / `_draw_semi` (gpu_gpu.c). VK owns poly/sprite raster; the rest
   (fills/copies/uploads) go through `gpu_gpu_dirty` to mirror VRAM regions into the GPU image. A CPU
   `s_vram` (1024×512 R16_UINT) still backs those fill/copy/upload ops so the mirrored image stays in
   sync. VK runs both windowed and headless.
4. **Present:** the per-frame loop (`native_boot.c`, via the `0x800788ac` tick override) → `gpu_present`
   → `gpu_present_ex` → `present_window` → **`blit_src`** (gpu_native.c) → `gpu_gpu_present(src, …)`,
   which renders the tee'd batch into the GPU VRAM image, then presents to the swapchain — or, headless,
   just renders + stops. **VK runs even with no window.**

## VK backend (gpu_gpu.c) at a glance
- One persistent **R16_UINT 1024×512 VRAM image** (`s_tex`) = PSX VRAM; SW-written regions are uploaded
  each frame, drawn geometry is rendered on top. Textured prims sample a snapshot (`s_vram_tex`) to avoid
  a render/sample feedback loop. Semi-transparent prims draw in OT-order groups, each re-snapshotting the
  framebuffer so stacked blends accumulate (the intro-fade fix).
- **Depth = a D32 buffer. Native per-pixel depth is THE depth model — always on, the single render
  behavior.** For a prim whose every vertex resolves to a projected 3D vertex, `gp0_exec` looks up the
  native view-space depth (`projprim_lookup_pz`→`pz`, `proj_pz_to_ord` → inverse-depth in [0,1]) and
  hands it per-vertex via `gpu_gpu_set_vd`; the D32 buffer then does true per-pixel occlusion (opaque
  writes depth, compare `GREATER_OR_EQUAL`, clear 0.0; semi tests, no write). The single buffer is
  partitioned: **3D world** in `[0, NATIVE_3D_MAX=0.9375]`, and a **2D/HUD overlay band** in
  `(0.9375, 1]` via `gpu_gpu_set_order_2d` — any prim where a vertex misses the 3D lookup (a screen-space
  / UI prim) lands in this band, ordered by its OT index so the painter's order is preserved *within the
  overlay* and UI composites over the world. **This is a PC GAME: real per-pixel depth, not the PSX
  OT-order painter's algorithm.** The deferred SHADING pass (SSAO/light) is separate. (later-118; field
  attach coverage ~100%.)
  - **Depth source = the OWNED submit path (game/render/submit.cpp), NOT a measurement hack.** Because the
    engine builds each GPU packet itself (the native POLY_GT3/GT4 submit), it records every vertex's real
    SZ (view-Z) keyed by the packet vertex word's ADDRESS (`projprim_set_pz`, gte_beetle.c); the renderer
    looks it up by the OT read address. Exact + deterministic. The table is reset each frame. This
    REPLACED the old "attach" infra (gte_op SXY-ring capture + value-matched mem.c store hook) — that
    correlated depth and was unreliable (same-pixel ambiguity, cross-frame staleness), now deleted.
    OPEN: only OWNED-submit prims carry real depth; prims from not-yet-ported overlay submitters fall to
    the 2D band, and 3D-projected overlay banners (e.g. a hint sign drawn on-top by OT order) get their
    true geometric depth and so sit behind nearer world geo — overlay-vs-world depth semantics, next.
  - **Coplanar TIEBREAK by paint order (z-fighting fix; does NOT inherit the OT).** Small near objects are
    integer geometry projected through the GTE fixed-point pipeline, so per-vertex view-Z is quantized to
    1/4096 — near-coplanar detail faces genuinely collapse onto one/adjacent depth buckets (nothing finer
    to recover). The D32 `GREATER_OR_EQUAL` test then coin-flips their occlusion per-pixel and it POPS as
    the object moves (the barrel "black chunk through the red top", #5). PSX has no depth buffer and
    resolves these purely by OT/paint order (last drawn wins, stable). We reproduce that as a **bounded
    epsilon on the depth WE own**: `ord3d(depth) + emit_order·ZBIAS_UNIT` (gpu_gpu.cpp `ord3d_b`/`set_order`,
    default 4e-7, capped 1.5e-3). This is NOT reading the guest OT/draw order/GP0 — the engine still owns
    PRIMARY ordering (real depth decides every genuinely-separated surface); the emit/paint order is used
    ONLY as a sub-ULP tiebreak between prims the depth buffer cannot distinguish (coplanar at geometry
    resolution), the one case whose correct answer IS paint order. Span (unit × prim count ≈ 1e-3) sits an
    order of magnitude below genuine world separations (~4.5e-3), so real occlusion is unchanged; host-only
    so SBS stays 0-diff. Resolves ~87% of true ties at the safe magnitude — full coverage needs shared
    per-object depth for detail prims (deferred). See docs/findings/render.md + `PSXPORT_ZFIGHT`/`ZBIAS`.

## SSAO / LIGHT — PC-native deferred shading (one post pass)
**Enabled via the F1 imgui overlay (g_mods), default OFF, persisted to `psxport_settings.ini`
(gitignored) and restored next launch — NOT env vars.** (mods.c owns the factory defaults + load/save;
mods.h is g_mods.) The technical pass below is unchanged; only the enable mechanism moved from
`PSXPORT_SSAO`/`PSXPORT_LIGHT` to the overlay toggle.

A fullscreen deferred pass run **inside `panel_render`, between the opaque and semi passes** (gpu_gpu.c
`ssao_pass`; shaders_vk/ssao.frag) that applies ambient occlusion (AO) and/or a directional
light reconstructed PURELY from the depth buffer — no PSX GTE hooks, no per-face PGXP
normals. Both are properties of the OPAQUE geometry: running before the translucent pass means UI/water
composite OVER the shaded world instead of being shaded by it (the menu's semi fill writes no depth, so
the depth buffer under it is the 3D terrain — shading *after* it would wrongly darken the UI).
- **Directional light:** reconstruct view-space position per depth pixel `P = ((sx-cx)*pz/H,
  (sy-cy)*pz/H, pz)` (cx,cy,H from gte_beetle `proj_screen_center`/`proj_plane_h`; the VRAM→screen map
  handles faithful + wide/hi-res-FB modes — inverse of the tritex.vert relocation), then the surface
  normal = `normalize(cross(dPdx, dPdy))` with a closer-neighbour pick to avoid bleeding a normal across
  a silhouette. Shade = `ambient + diffuse·max(0, N·L)`, applied to the baked color as albedo. This is
  the standard deferred reconstruction (the "proper" PC-native way), NOT the rejected PGXP per-face hack.
- **Inputs:** the 3-band native depth (`s_depth`, made SAMPLED) + the opaque color (`s_tex`). It relies
  on the native per-pixel depth, which is always on.
- **Depth linearize:** the stored depth = `ord3d(proj_pz_to_ord(pz))`; `proj_pz_to_ord` is AFFINE in
  `1/pz`, so the shader undoes the band remap then the affine map to recover view-Z (needs the near
  plane = `proj_near_pz()` = H/2). Only pixels inside the 3D band `[NATIVE_3D_MIN, NATIVE_3D_MAX]` are
  touched; sky/backdrop/HUD bands pass through unchanged.
- **AO model = CURVATURE via opposite-neighbour pairs** (`ssao.frag`), NOT "is the neighbour closer".
  A flat surface (even steeply tilted) has center == the average of opposite neighbours → 0 AO; this is
  what kills the false "the whole tilted ground darkens" wash a naive range-check produces. Only genuine
  concavities/contacts (center recedes behind the line through its neighbours) darken. Pairs touching a
  non-3D pixel (silhouette vs sky/UI) are skipped → no edge halos. 16 fixed taps (2 rings × 4 pairs),
  no per-pixel rotation → no noise → no blur pass needed.
- **Output:** shaded color → `s_ssao_img` → copied back into `s_tex`, so present/screenshot pick it up
  with no extra wiring. The pass only touches the VK path, never `s_vram`; default off.
- **Tuning (env):** AO — `PSXPORT_SSAO_STRENGTH` (def 1.0), `_RADIUS` (px, def 5 × IRES), `_BIAS` (def
  0.01, frac of view-Z), `_RANGE` (def 0.15). Light — `PSXPORT_LIGHT_DIR`="x,y,z" (to-light, view space;
  def -0.4,-0.7,-0.5), `_AMBIENT` (def 0.65), `_DIFFUSE` (def 0.5).
- **Verify** by running the game and eyeballing it (the user verifies visually). `PSXPORT_SSAO_VIZ` = 1:
  AO factor grayscale; 2: reconstructed normal as RGB; 3: lit factor grayscale — all on 3D pixels only,
  2D/sky in original color (confirms the band gating + the math).

## Running it
- **Windowed (VK):** `PSXPORT_GPU_WINDOW=1 … ./scratch/bin/tomba2_port MAIN.EXE` (needs `DISPLAY`).
  VK is THE renderer; this just gives it a window + swapchain.
- **Headless OFFSCREEN VK (deterministic, no window/display):** `PSXPORT_VK_HEADLESS=1`. Renders the real
  VK path into `s_tex` with no swapchain. Use the REPL `shot <path>` command (VK-readback PPM, works
  headless) to capture a frame.

## Screenshots & driving — the REPL
There is no oracle and no diff tooling; you verify by running the PC game and observing it.
- **Screenshot:** REPL `shot <path>` (VK-readback, works headless under `PSXPORT_VK_HEADLESS=1`). The old
  `PSXPORT_VK_SHOT`/`VK_SHOTSEQ` env vars are gone.
- **Drive the game:** the REPL (`PSXPORT_REPL=1`, commands on stdin) — `run N`, `newgame`, `skip N`,
  `press`/`release`/`tap <btn>`, `r`/`rw`/`w` (memory), `dumpram <path>` (+ `.spad` scratchpad sidecar),
  `debug <chans|all>` (enable diagnostic channels at runtime), `stage`, `regs`, `seq`, `quit`. The live
  TCP debug server (`PSXPORT_DEBUG_SERVER=1`, `tools/dbgclient.py`) has the same commands plus
  scene/provat/vkvram for inspecting a windowed run. See `docs/driving-the-game.md`.
- **Workflow:** inspect the live PC game — REPL/debug-server state + scene dump + the user eyeballing a
  build. The engine OWNS its render; there is nothing PSX to diff against. Principle still holds: don't
  conclude from a cherry-picked still — verify on the running game.
