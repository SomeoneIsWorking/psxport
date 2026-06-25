# Renderer port: Vulkan → raylib/rlgl (one PC-native render API)

**Decision (user, 2026-06-25):** ONE render API — **raylib/rlgl**, PC-native. **Delete Vulkan entirely**
(`gpu_vk.cpp`, the SPIR-V shaders, the SDL window/swapchain, the `vulkan`/`sdl2`-window link deps). raylib
was chosen for out-of-the-box macOS + Android support and to escape MoltenVK (the SBS two-pane VK composite
rendered black on MoltenVK — the trigger for this whole effort).

**Keep the "vanilla PSX" rasterization path as an ORACLE** (for the SBS pane-B native-vs-PSX compare, and
for the still-PSX-drawn 2D screens: menus / FMV / title / HUD). It is reimplemented on rlgl too — there is
no Vulkan left, but the PSX GP0 raster semantics (1555 VRAM, CLUT 4/8/15bpp, texture window, semi blend
modes) are preserved as a SEPARATE render target.

This supersedes the earlier (wrong) handoff `scratch/handoff_raylib_sbs.md`, which scoped raylib as a
present/window replacement over a still-Vulkan render. That was implemented (raylib window + 2-pane blit of
VK-readback buffers) and is now being REPLACED by the full port. Reusable from that work: raylib vendored
(`vendor/raylib` @5.5) + `tools/build_raylib.sh` + run.sh/build_port.sh wiring; the keyboard→PSX-pad mapping
in `runtime/recomp/sbs_raylib.cpp`.

## The render API surface (what the engine/gpu_native call — must ALL be reimplemented)
Thin C wrappers forward to `core->game->gpu_vk.method(...)`. Port = same signatures, rlgl backend.

- **Ordering / depth:** `gpu_vk_set_order` (OT idx→depth), `gpu_vk_set_order_2d` (HUD band),
  `gpu_vk_set_order_2d_bg` (backdrop band), `gpu_vk_set_vd` (per-vertex float 3D depth),
  `gpu_vk_set_vd_n` (SBS native-depth channel), `gpu_vk_set_xyf` (sub-pixel XY smoothing).
  Depth bands: backdrop [0,MIN) | 3D world [MIN,MAX] (real per-vertex depth) | overlay (MAX,1].
- **Draw:** `gpu_vk_draw_tri` (flat gouraud tri), `gpu_vk_draw_tritri` (textured tri: tp/clut/mode/tw/da),
  `gpu_vk_draw_semi` (semi tri + blend 0=avg/1=add/2=sub/3=add4). Native 3D enters via
  `gpu_draw_world_quad` (float world quad→verts, real depth) and the GP0 tee `gpu_emit_rq_item`.
- **VRAM 2D:** `gpu_vk_dirty` (dirty-rect), `gpu_vk_semi_group` (semi-overlap bbox), CPU VRAM is `s_vram`
  (uint16 1555, 1024×512) in gpu_native — 2D screens are VRAM-resident and presented directly.
- **Present:** `gpu_vk_present(src,sx,sy,w,h)` (sample VRAM region, letterbox to aspect, fade),
  `gpu_vk_present_image` (RGBA8 fullscreen, SCEA splash), `gpu_set_fade`/`gpu_clear_fade` (mode 1 add-white
  / 2 sub-black), `gpu_vk_shot` + headless readback (1555→RGB PPM).
- **SBS:** `g_sbs`/`g_dualview` → two targets composited side-by-side; `gpu_vk_select_target`.
- **Enhancements (DEFER):** `gpu_vk_shadow_push_tri` + shadow_pass (2048² depth map), ssao_pass. Skipped
  under SBS already; port last or stub initially.
- **Misc:** `gpu_vk_wide_engine*` (4:3/16:9/21:9 widths), `gpu_seen3d_this_frame`, `gpu_vk_stats`.

## rlgl design
- **Window + input:** raylib `InitWindow` (the ONE window). Input via raylib keys/gamepad (mapping already
  in sbs_raylib.cpp); replaces `pad_poll_sdl`. Headless = `FLAG_WINDOW_HIDDEN` + render-to-texture + readback.
- **Targets (RenderTexture2D, color+depth FBO):** `RT_SCREEN` (the native PC-rendered frame). For the PSX
  oracle / SBS pane-B: `RT_PSX`. 2D VRAM screens: keep CPU `s_vram` uint16; upload to a raylib texture and
  blit (the menu/FMV/title path). `gpu_vk_select_target` picks which RT the batch draws into.
- **Native 3D:** rlgl immediate (`rlBegin(RL_TRIANGLES)`, `rlColor4ub`, `rlVertex3f` with z=float depth),
  `rlEnableDepthTest`. Ortho-ish projection matching the engine's screen-space verts (verts arrive as
  VRAM/screen coords + per-vertex depth → map to NDC).
- **PSX textured raster:** custom GLSL-330 shader = port of `tritex.frag` (CLUT 4/8/15bpp decode + texture
  window + draw-area clip), sampling the VRAM as a texture. VRAM texture: rlgl-create an R16UI (or RGBA8
  packed) texture from `s_vram`; the shader decodes 1555/CLUT. This is the hardest piece.
- **Semi blend modes:** `rlSetBlendMode(RL_BLEND_CUSTOM)` + `rlSetBlendFactorsSeparate` per mode
  (avg/add/sub/add4). Semi pass after opaque, depth-test on / depth-write off, per semi-group.
- **Present:** draw `RT_SCREEN` to the window with letterbox (4:3 for 2D screens, selected aspect for 3D)
  + fade in a present shader (port `present.frag`). Headless: `LoadImageFromTexture` + write PPM (`shot`).
- **Shaders → GLSL 330 via `LoadShaderFromMemory`:** present, tri, tritex, semi (tritex variants), [later]
  shadow, ssao. Drop `tools/gen_vk_shaders.sh` / `gpu_vk_shaders.h` (SPIR-V) for inline GLSL strings.

## Phasing
1. **Module + build:** new `runtime/recomp/gpu_rl.cpp` (+ helpers) implementing every entry point above on
   rlgl. Wire into run.sh/build_port.sh SRC; drop `vulkan` + SDL-window from CFLAGS/link (keep SDL only if
   still needed for audio — check; SPU uses SDL audio sink). Build green with stubs first.
2. **Single-core game renders:** native 3D (depth) + 2D VRAM screens + present + fade + `shot`. Verify the
   game looks right on linux (USER eyeballs) and on the user's Mac.
3. **PSX oracle path:** the GP0 textured/semi raster into RT_PSX (CLUT shader). Verify a 2D screen + the
   field PSX render.
4. **SBS:** two RenderTextures (A native | B PSX) drawn side-by-side in the one window; input mirrored.
   No CPU readback, no second window.
5. **Delete Vulkan:** remove gpu_vk.cpp, shaders_vk/, gpu_vk_shaders.h, gen_vk_shaders.sh, vulkan link.
6. **Shadows/SSAO** (enhancements) port or drop.

## Status
- [x] raylib vendored + built static + run.sh/build_port.sh wiring (commit pending)
- [x] keyboard→pad mapping (sbs_raylib.cpp)
- [ ] gpu_rl.cpp module skeleton + entry-point stubs, build green
- [ ] native 3D + 2D + present (single-core game on raylib)
- [ ] PSX oracle raster (CLUT shader)
- [ ] SBS two-target compose
- [ ] delete Vulkan
- [ ] shadows/ssao
