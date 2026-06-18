# Render & present architecture — read before touching graphics/VK

How a frame gets from GP0 commands to the screen, the SW vs VK split, depth, and how to run/diff
headless. Pairs with `gfx-debug.md` (the debug workflow + tool catalog) and `config.md` (flags).

## The GP0 → screen path
1. The game (recomp + native engine) builds an ordering-table (OT) of GP0 primitive packets in guest RAM.
2. `ov_draw_otag` (engine/game_tomba2.c) → `gpu_dma2_linked_list` (gpu_native.c) walks the OT and calls
   **`gp0_exec`** (gpu_native.c) per primitive. `gp0_exec` decodes each packet (poly/sprite/fill/copy/
   upload/env) — this is the single chokepoint where every drawn primitive passes.
3. **Two rasterizers, switched by `gpu_vk_enabled()`:**
   - **SW** (the oracle/fallback): `gp0_exec` calls `tri()`/`raster_sprite()` writing into `s_vram`
     (the CPU 1024×512 R16_UINT VRAM). Used when VK is off.
   - **VK** (default when a window or headless-VK is on): `gp0_exec` **tees** each prim to the GPU via
     `gpu_vk_draw_tritri` / `_draw_semi` (gpu_vk.c). VK owns poly/sprite raster; SW does the rest
     (fills/copies/uploads still go through `gpu_vk_dirty` to mirror VRAM into the GPU image).
4. **Present:** the per-frame loop (`native_boot.c`, via the `0x800788ac` tick override) → `gpu_present`
   → `gpu_present_ex` → `present_window` → **`blit_src`** (gpu_native.c). `blit_src` dispatches:
   - VK enabled → `gpu_vk_present(src, …)` (renders the tee'd batch into the GPU VRAM image, then
     presents to the swapchain — or, headless, just renders + stops). **VK runs even with no window.**
   - else, a real window → SW SDL blit of `s_vram`.

   GOTCHA (cost a session): `blit_src` must check `gpu_vk_enabled()` **before** `win_enabled()`, else
   headless offscreen VK never reaches `gpu_vk_present`.

## VK backend (gpu_vk.c) at a glance
- One persistent **R16_UINT 1024×512 VRAM image** (`s_tex`) = PSX VRAM; SW-written regions are uploaded
  each frame, drawn geometry is rendered on top. Textured prims sample a snapshot (`s_vram_tex`) to avoid
  a render/sample feedback loop. Semi-transparent prims draw in OT-order groups, each re-snapshotting the
  framebuffer so stacked blends accumulate (the intro-fade fix).
- **Depth = a D32 buffer.** Two depth models share it:
  - **Default (OT order):** each prim gets one normalized depth `s_cur_ord = (OT_index+1)/65536`
    (`gpu_vk_set_order`). Opaque writes depth (compare `GREATER_OR_EQUAL`, clear 0.0); semi tests, no
    write. Reproduces painter/OT order. This is the faithful oracle.
  - **Native per-vertex depth (Phase 2) — NOW THE DEFAULT (`native_depth_on()`, always on):** for a prim
    whose every vertex resolves to a projected 3D vertex, `gp0_exec` looks up the native view-space depth
    (`projprim_lookup_pz`→`pz`, `proj_pz_to_ord` → inverse-depth in [0,1]) and hands it per-vertex via
    `gpu_vk_set_vd`; the D32 buffer then does true per-pixel occlusion. The single buffer is partitioned:
    **3D world** in `[0, NATIVE_3D_MAX=0.9375]`, **2D/HUD overlay** (any vertex misses the lookup →
    screen-space prim) in `(0.9375, 1]` via `gpu_vk_set_order_2d`, so UI composites over the world.
    **This is a PC GAME: real per-pixel depth is the default** (the OT-order painter's algorithm is the
    PSX limitation, and genuine widescreen needs real depth). Opt OUT only to A/B-diff the faithful
    OT-order oracle: **`PSXPORT_FAITHFUL_DEPTH=1`** (or legacy `PSXPORT_NATIVE_DEPTH=0`). The deferred
    SHADING pass (SSAO/light) is separate and still opt-in. (later-118; field attach coverage ~100%.)
  - **Depth source = the OWNED submit path (engine/engine_submit.c), NOT a measurement hack.** Because the
    engine builds each GPU packet itself (the native POLY_GT3/GT4 submit), it records every vertex's real
    SZ (view-Z) keyed by the packet vertex word's ADDRESS (`projprim_set_pz`, gte_beetle.c); the renderer
    looks it up by the OT read address. Exact + deterministic. The table is reset each frame. This
    REPLACED the old "attach" infra (gte_op SXY-ring capture + value-matched mem.c store hook) — that
    correlated depth and was unreliable (same-pixel ambiguity, cross-frame staleness), now deleted.
    `attach_enabled()` (= `native_depth_on() || PSXPORT_SBS`) gates the recording + reset.
    OPEN: only OWNED-submit prims carry real depth; prims from not-yet-ported overlay submitters fall to
    the 2D band, and 3D-projected overlay banners (e.g. a hint sign drawn on-top by OT order) get their
    true geometric depth and so sit behind nearer world geo — overlay-vs-world depth semantics, next.

## `PSXPORT_SSAO` / `PSXPORT_LIGHT` — PC-native deferred shading (one post pass)
A fullscreen deferred pass run **inside `panel_render`, between the opaque and semi passes** (gpu_vk.c
`ssao_pass`; shaders_vk/ssao.frag) that applies ambient occlusion (`PSXPORT_SSAO`) and/or a directional
light (`PSXPORT_LIGHT`) reconstructed PURELY from the depth buffer — no PSX GTE hooks, no per-face PGXP
normals. Both are properties of the OPAQUE geometry: running before the translucent pass means UI/water
composite OVER the shaded world instead of being shaded by it (the menu's semi fill writes no depth, so
the depth buffer under it is the 3D terrain — shading *after* it would wrongly darken the UI).
- **Directional light:** reconstruct view-space position per depth pixel `P = ((sx-cx)*pz/H,
  (sy-cy)*pz/H, pz)` (cx,cy,H from gte_beetle `proj_screen_center`/`proj_plane_h`; the VRAM→screen map
  handles faithful + wide/hi-res-FB modes — inverse of the tritex.vert relocation), then the surface
  normal = `normalize(cross(dPdx, dPdy))` with a closer-neighbour pick to avoid bleeding a normal across
  a silhouette. Shade = `ambient + diffuse·max(0, N·L)`, applied to the baked color as albedo. This is
  the standard deferred reconstruction (the "proper" PC-native way), NOT the rejected PGXP per-face hack.
- **Inputs:** the 3-band native depth (`s_depth`, made SAMPLED) + the opaque color (`s_tex`). SSAO
  **implies `PSXPORT_NATIVE_DEPTH`** (it needs real per-vertex depth) — the gate is OR'd into the
  native-depth checks (gpu_native.c / gte_beetle `attach_enabled`). Disabled under `PSXPORT_SBS`.
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
- **Output:** shaded color → `s_ssao_img` → copied back into `s_tex`, so present/dump pick it up with no
  extra wiring. Faithful: the pass only touches the VK path, never `s_vram`; default off.
- **Tuning (env):** AO — `PSXPORT_SSAO_STRENGTH` (def 1.0), `_RADIUS` (px, def 5 × IRES), `_BIAS` (def
  0.01, frac of view-Z), `_RANGE` (def 0.15). Light — `PSXPORT_LIGHT_DIR`="x,y,z" (to-light, view space;
  def -0.4,-0.7,-0.5), `_AMBIENT` (def 0.65), `_DIFFUSE` (def 0.5).
- **Verify single-run** (cross-run pixel A/B is unreliable: AUTO_GAMEPLAY scene state drifts between
  processes). `PSXPORT_SSAO_VIZ` = 1: AO factor grayscale; 2: reconstructed normal as RGB; 3: lit factor
  grayscale — all on 3D pixels only, 2D/sky in original color (confirms the band gating + the math).

## Running it
- **Windowed (VK default):** `PSXPORT_GPU_WINDOW=1 … ./scratch/bin/tomba2_port MAIN.EXE` (needs `DISPLAY`).
- **Headless SW:** no `GPU_WINDOW` → SW rasterizer, no window (what `drive.py` uses).
- **Headless OFFSCREEN VK (deterministic, no window/display):** `PSXPORT_VK_HEADLESS=1`. Renders the real
  VK path into `s_tex` with no swapchain; `PSXPORT_VK_SHOT=F` / `PSXPORT_VK_SHOTSEQ="a:b:step:dir"` dump
  frames. This is the basis of the VK render-diff.
- Reach the field headless: `PSXPORT_NO_FMV=1 PSXPORT_AUTO_GAMEPLAY=1` (field ~frame 328).

## VK render-diff tool: `tools/vk_depth_diff.sh [first:last:step]`
Runs the port **headless offscreen** three times — default depth, `NATIVE_DEPTH`, and a default re-run
(determinism control) — captures the same field frames, and `tools/vk_depth_diff.py` reports per frame:
`ctl%` (default-vs-default, must be ~0 or the capture is noisy) and `default-vs-depth%` + maxΔ, plus a
red diff mask per frame in `scratch/screenshots/vkdiff/`. Use this to see exactly where/when real depth
diverges from OT order (e.g. the face flicker) instead of eyeballing a windowed run. Determinism
verified: two identical headless runs are pixel-identical (`ctl% = 0.00`).

This is a VK-vs-VK diff (depth A/B). It complements `gpu_differ` (ours-SW vs Beetle on identical GP0),
which does NOT cover the VK path or the depth model — keep using gpu_differ for rasterizer fidelity.

## `PSXPORT_SBS` — live side-by-side depth A/B (drive the game, compare both modes in one window)
`PSXPORT_SBS=1` renders the frame into TWO panes in one window: **left = default OT depth, right =
`NATIVE_DEPTH`**, full-res each, no seam. Implemented with **object-oriented `Panel`s** (gpu_vk.c): each
Panel owns its PERSISTENT VRAM color image, depth buffer, framebuffer and present descriptor, and renders
independently — `panel_upload()` mirrors the dirty VRAM into *its own* image, `panel_render()` draws with
its depth-channel pipeline. Two depth channels per vertex (`.ord` = OT, `.ordn` = native, from gpu_native
`gpu_vk_set_vd_n`/`set_order_2d_n`); the channel is a **pipeline specialization constant** (`SBS_NATIVE`,
`s_tritex_pipe[native]`), NOT a push constant. Headless `PSXPORT_VK_SHOTSEQ` dumps `vk_*.ppm` (pane 0) +
`vk_*_b.ppm` (pane 1) for verification. `PSXPORT_SBS` implies the attach infra (gte_beetle `attach_env`).
- **Dead-ends that cost a long session (do NOT retry):** (1) one shared image with an instanced/compressed
  split → seam bleed from off-screen-x geometry; (2) a per-frame `vkCmdCopyImage(s_tex→s_tex_b)` to seed
  pane 1 → pane 1's *background* came from the default render, so it always looked identical; (3) a shared
  depth buffer or a push-constant depth select → state bled between the two passes (pane 2 mirrored pane 1).
  The fix for all of them is the same: **fully independent Panels, no shared mutable render state.**
- **Build gotcha (FIXED):** `tools/build_port.sh` now treats `gpu_vk_shaders.h` as a dependency of
  `gpu_vk.c`. Before, a shader-only edit regenerated the embedded SPIR-V header but did NOT recompile
  `gpu_vk.c`, so the binary silently ran STALE shader bytecode (burned a long debugging session).
