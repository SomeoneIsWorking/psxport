#include "core.h"
#include "game.h"    // Game / GpuGpuState (per-instance render state)
#include "gpu_gpu.h"  // public Core*-threaded API decls (wrappers below forward to core->game->gpu_gpu)
#include "render/screen_fade.h"   // class ScreenFade — the single fade driver (present reads its state)
#include "render/render.h"                    // Render::stats (RenderStats — was g_dbg_world_quads)
// gpu_gpu.cpp — SDL3 GPU API present backend for the Tomba2Engine port.
//
// This is the PC-native renderer re-expressed on the SDL3 GPU API (SDL_gpu.h), replacing gpu_gpu.cpp
// (Vulkan + SDL2). ONE stack — SDL3 owns the window, input, audio AND the GPU device — so the Mac runs
// native Metal (the original MoltenVK SBS-black bug is gone) and Linux/Windows run Vulkan/D3D12.
//
// PASS 1 (this file's current scope): the 2D VRAM present path + the fullscreen IMAGE present + the
// headless VRAM readback (`shot`). The native 3D raster (draw_tri/tritri/semi → depth-ordered offscreen
// target) is PASS 2: opaque geometry renders into the packed-1555 VRAM colour target as before; semi-
// transparent geometry renders into a float RGBA intermediate using the GPU's REAL fixed-function blend
// unit (one pipeline per PSX blend mode), decoded from and re-encoded into the packed VRAM around that
// pass — see render_geom's header comment for why (packed 1555 can't be correctly HW-blended directly).
//
// State model: the SDL_GPU device/window/pipelines live on class GpuDevice (gpu_gpu_device.h),
// ONE per process — the first Game constructed claims it (GpuDevice::sInstance); the wrappers
// ignore Core* exactly as before. The per-frame batch state lives on GpuGpuState (game.h).
#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include "cfg.h"
#include "mods.h"             // Mods: per-Game mod toggles (wide/ires) — reached via game->mods
#include "overlay_glue.h"     // RmlUi mod/debug overlay hooks (init / event / per-frame / record)
#include "gpu_gpu_shaders.h"  // generated: spv_g_present_{vert,frag} / spv_g_image_{vert,frag}
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define VRAM_W 1024
#define VRAM_H 512

// ---- device / window state — class GpuDevice (gpu_gpu_device.h), one per process ---------------------
// The first Game constructed claims GpuDevice::sInstance (Game()); every entry point below reaches the
// claimed instance through gdev(). The historical `s_*` spellings are shadow macros over gdev() fields
// so the (large) function bodies are unchanged by the move.
GpuDevice* GpuDevice::sInstance = nullptr;
static inline GpuDevice& gdev() { return *GpuDevice::sInstance; }
#define s_gpu_on       (gdev().s_gpu_on)
#define s_inited       (gdev().s_inited)
#define s_headless     (gdev().s_headless)
#define s_win          (gdev().s_win)
#define s_dev          (gdev().s_dev)
#define s_swap_fmt     (gdev().s_swap_fmt)
#define s_samp_nearest (gdev().s_samp_nearest)
#define s_samp_linear  (gdev().s_samp_linear)
#define s_present_pipe (gdev().s_present_pipe)
#define s_image_pipe   (gdev().s_image_pipe)
#define s_img_tex      (gdev().s_img_tex)
#define s_img_xfer     (gdev().s_img_xfer)
#define s_img_w        (gdev().s_img_w)
#define s_img_h        (gdev().s_img_h)
#define s_tri_pipe     (gdev().s_tri_pipe)
#define s_tritex_pipe  (gdev().s_tritex_pipe)
#define s_semi_pipe    (gdev().s_semi_pipe)
#define s_decode_pipe  (gdev().s_decode_pipe)
#define s_encode_pipe  (gdev().s_encode_pipe)
#define s_sbs_tex      (gdev().s_sbs_tex)
#define s_sbs_xfer     (gdev().s_sbs_xfer)
#define s_sbs_w        (gdev().s_sbs_w)
#define s_sbs_h        (gdev().s_sbs_h)

// (Engine-owned screen fade moved to class ScreenFade at game/render/screen_fade.h. State lives in guest
// memory so it's per-Core / SBS-clean without needing per-instance C++ fields. Native present path
// reads ScreenFade::get(core) directly — see readback + PresentPC uniform builders below.)

// Present-pass fragment uniform: matches present.frag's `PC { ivec4 disp; ivec4 fade; }`.
struct PresentPC { int32_t disp[4]; int32_t fade[4]; };

// ---- Pass 2: native 3D / textured raster (depth-ordered, REAL HW blend for semi) ----------------------
// The engine draws the world AND the 2D menus/HUD/sprites as textured/flat prims through the tee
// (gpu_gpu_draw_tri/tritri/semi). Each present: upload CPU VRAM into the packed-1555 COLOR-TARGET image
// AND a SAMPLER snapshot (the texture/CLUT atlas source), then render OPAQUE geometry on top with a D32
// depth buffer. Semi-transparent geometry is a SEPARATE pass into a FLOAT RGBA intermediate (decoded from
// the just-drawn opaque result) using the GPU's OWN fixed-function blend unit — one pipeline per PSX blend
// mode (avg/add/sub/add4), since blend state is static per-pipeline, not per-draw. This replaced an
// in-shader "sample a second VRAM snapshot as the destination" scheme that read a stale pre-frame buffer
// for anything drawn by the native (non-legacy-2D) path, producing solid-black artifacts where a
// near-black semi vertex was meant to fade invisibly into whatever was behind it (2026-07-01 dark-outline
// root cause). See shaders_gpu/{tri,tritex,decode,encode,trisemi_hw}.{vert,frag} + trisemi_hw.frag's header
// comment for the per-mode blend-factor derivation. Single batch (SBS dual-pane is Pass 3).
#define NATIVE_3D_MIN 0.0625f
#define NATIVE_3D_MAX 0.9375f
static inline float ord3d(float d) { return NATIVE_3D_MIN + d * (NATIVE_3D_MAX - NATIVE_3D_MIN); }
// 3D-band depth with the paint-order tiebreak folded in and clamped to the 3D band. When two world prims
// share a (near-)equal real depth, the later-emitted one gets a marginally larger value and wins the
// GREATER_OR_EQUAL depth test uniformly (deterministic, motion-stable), replacing the per-pixel
// interpolation-noise coin-flip that produced the barrel/decoration z-fight flicker. Clamp to NATIVE_3D_MAX:
// two prims that both hit the ceiling still resolve later-wins (paint order), and stay below the 2D bands.
static inline float ord3d_b(float d, float bias) { float o = ord3d(d) + bias;
  return o < NATIVE_3D_MIN ? NATIVE_3D_MIN : (o > NATIVE_3D_MAX ? NATIVE_3D_MAX : o); }
#define TRI_CAP 196608   // max batched vertices (= 65536 tris)
#define TEX_CAP 196608
#define NUM_BLEND_MODES GGS_NUM_BLEND_MODES   // PSX semi blend modes (0=avg 1=add 2=sub 3=add4)
struct TriVtx { float x, y, r, g, b, ord; };                                                    // 24 bytes
struct TexVtx { float x, y, u, v, r, g, b; int32_t tp[4], clut[4], tw[4], da[4]; float ord; };   // 96 bytes
// Batch buffers + counts moved onto GpuGpuState (per-Core) — reach as `this->s_tri_buf` (cast from
// void* to TriVtx*) inside the methods. The `render_geom` free function below takes a `GpuGpuState&`
// so it can pull the right instance's batches at present time.
// (raster/pipeline resources live on GpuDevice — see the shadow macros above)
static void create_3d_pipelines(void);
static void init_gpu(Game* game);
static void poll_quit(Game* game);

// ---- enable / windowed gates (mirror gpu_gpu.cpp) ----------------------------------------------------
int gpu_gpu_enabled(void) {
  if (s_gpu_on < 0) {
    s_headless = cfg_on("PSXPORT_VK_HEADLESS") ? 1 : 0;   // keep the env name stable across the backend swap
    s_gpu_on = 1;
  }
  return s_gpu_on;
}
extern "C" int gpu_windowed(void)   { return gpu_gpu_enabled() && !s_headless; }
extern "C" int gpu_has_window(void) { return s_win != 0; }

// Live window size in pixels (swapchain extent). Fall back to native 4:3 before the window exists.
static int win_w(void) { int w = 320, h = 240; if (s_win) SDL_GetWindowSizeInPixels(s_win, &w, &h); return w > 0 ? w : 320; }
static int win_h(void) { int w = 320, h = 240; if (s_win) SDL_GetWindowSizeInPixels(s_win, &w, &h); return h > 0 ? h : 240; }

// ---- PC-native widescreen accessors (kept; the engine projection reads these) -----------------------
static int wide_native_w(const Game* game) {
  switch (game->mods.aspect) {
    case ASPECT_16_9: return 428;
    case ASPECT_21_9: return 560;
    case ASPECT_AUTO: { // AUTO = match the live window aspect — identically in standalone and under SBS
                        // (each core renders its full-window FOV into its own target; the SBS compositor
                        // letterboxes that into its half-window pane, so no special-case here).
                        int ww = win_w();
                        int w = (int)((240.0 * ww) / win_h() + 0.5); w &= ~1;
                        if (w < 320) w = 320; if (w > VRAM_W) w = VRAM_W; return w; }
    default:          return 320;
  }
}
// Per-core (was process-global): widescreen never touches the PSX oracle — and `oracle` is per-Game
// now, so one process can hold a wide user core and a pure 4:3 oracle core (SBS honesty).
int gpu_gpu_wide_engine(Core* c)     { Game* g = c->game; return g->mods.aspect != ASPECT_4_3 && !g->oracle; }
int gpu_gpu_wide_engine_ofx(Core* c) { return wide_native_w(c->game) / 2; }
int gpu_gpu_wide_engine_w(Core* c)   { return wide_native_w(c->game); }
void gpu_gpu_video_status(Core* c, int* native_w, int* ires, int* fbw, int* fbh, int* ww, int* wh, int* ires_cap) {
  const Mods& m = c->game->mods;
  // Cap = how many integer multiples of the (possibly wide) native FB width fit in VRAM. Raised to 4 (was
  // 3): at 4:3 native_w=320 → VRAM_W/320 = 3, so 4x needs the extra headroom the cap now allows; at 16:9
  // native_w=428 → cap=2, at 21:9 native_w=560 → cap=1. The clamp keeps the scaled FB inside VRAM.
  int nw = wide_native_w(c->game), cap = VRAM_W / nw; if (cap < 1) cap = 1; if (cap > 4) cap = 4;
  // mods.ires: 0 = AUTO (derive the scale from the live window height, ~round(h/240)), 1..4 = fixed.
  int i = m.ires;
  if (i == 0) { i = (int)((win_h() / 240.0) + 0.5); if (i < 1) i = 1; }
  if (i < 1) i = 1; if (i > cap) i = cap;
  if (native_w) *native_w = nw; if (ires) *ires = i; if (fbw) *fbw = nw * i;
  if (fbh) *fbh = 240 * i;      if (ww) *ww = win_w(); if (wh) *wh = win_h();
  if (ires_cap) *ires_cap = cap;
}
// Pass 1: no scaled scratch FB yet (3D is Pass 2), so a frame never renders via the FB.
int GpuGpuState::frame_via_fb() { return 0; }

// ---- SDL_GPU helpers --------------------------------------------------------------------------------
#define GPUCHK(p, what) do { if (!(p)) { fprintf(stderr, "[gpu_gpu] %s failed: %s\n", what, SDL_GetError()); exit(2); } } while (0)

static SDL_GPUShader* make_shader(const uint32_t* code, unsigned len, SDL_GPUShaderStage stage,
                                  Uint32 num_samplers, Uint32 num_uniform_buffers) {
  SDL_GPUShaderCreateInfo ci = {};
  ci.code_size = len; ci.code = (const Uint8*)code; ci.entrypoint = "main";
  ci.format = SDL_GPU_SHADERFORMAT_SPIRV; ci.stage = stage;
  ci.num_samplers = num_samplers; ci.num_uniform_buffers = num_uniform_buffers;
  SDL_GPUShader* s = SDL_CreateGPUShader(s_dev, &ci);
  GPUCHK(s, "SDL_CreateGPUShader"); return s;
}

// A fullscreen-triangle pipeline (no vertex input) sampling one fragment texture, with one fragment
// uniform buffer, no depth, no blend — used for both present (R16_UINT VRAM) and image (RGBA8) passes.
static SDL_GPUGraphicsPipeline* make_fullscreen_pipeline(const uint32_t* vs_code, unsigned vs_len,
                                                         const uint32_t* fs_code, unsigned fs_len,
                                                         SDL_GPUTextureFormat fmt) {
  SDL_GPUShader* vs = make_shader(vs_code, vs_len, SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
  SDL_GPUShader* fs = make_shader(fs_code, fs_len, SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
  SDL_GPUColorTargetDescription ct = {};
  ct.format = fmt;   // blend disabled (enable_blend = false by zero-init); writes all channels
  SDL_GPUGraphicsPipelineCreateInfo gp = {};
  gp.vertex_shader = vs; gp.fragment_shader = fs;
  gp.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
  gp.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
  gp.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
  gp.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
  gp.target_info.color_target_descriptions = &ct;
  gp.target_info.num_color_targets = 1;
  SDL_GPUGraphicsPipeline* p = SDL_CreateGPUGraphicsPipeline(s_dev, &gp);
  GPUCHK(p, "SDL_CreateGPUGraphicsPipeline");
  SDL_ReleaseGPUShader(s_dev, vs); SDL_ReleaseGPUShader(s_dev, fs);
  return p;
}

// A fullscreen-triangle pipeline (no vertex input, no uniform buffer) sampling one fragment texture into
// an OFFSCREEN target of `fmt` — used for the decode (1555 -> float RGBA) and encode (float RGBA -> 1555)
// passes around the real-HW-blend semi step.
static SDL_GPUGraphicsPipeline* make_fullscreen_offscreen_pipeline(const uint32_t* vs_code, unsigned vs_len,
                                                                    const uint32_t* fs_code, unsigned fs_len,
                                                                    SDL_GPUTextureFormat fmt) {
  SDL_GPUShader* vs = make_shader(vs_code, vs_len, SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
  SDL_GPUShader* fs = make_shader(fs_code, fs_len, SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 0);
  SDL_GPUColorTargetDescription ct = {}; ct.format = fmt;   // blend disabled; writes all channels
  SDL_GPUGraphicsPipelineCreateInfo gp = {};
  gp.vertex_shader = vs; gp.fragment_shader = fs;
  gp.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
  gp.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
  gp.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
  gp.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
  gp.target_info.color_target_descriptions = &ct;
  gp.target_info.num_color_targets = 1;
  SDL_GPUGraphicsPipeline* p = SDL_CreateGPUGraphicsPipeline(s_dev, &gp);
  GPUCHK(p, "SDL_CreateGPUGraphicsPipeline(offscreen)");
  SDL_ReleaseGPUShader(s_dev, vs); SDL_ReleaseGPUShader(s_dev, fs);
  return p;
}

// A geometry pipeline: a vertex-buffer pipeline rendering into the R16_UINT VRAM color target + a D32
// depth target. `depth_write` distinguishes opaque (test+write) from semi (test, no write).
static SDL_GPUGraphicsPipeline* make_geom_pipeline(const uint32_t* vs_code, unsigned vs_len,
    const uint32_t* fs_code, unsigned fs_len, Uint32 pitch,
    const SDL_GPUVertexAttribute* attrs, Uint32 n_attr, Uint32 num_samplers, bool depth_write) {
  SDL_GPUShader* vs = make_shader(vs_code, vs_len, SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
  SDL_GPUShader* fs = make_shader(fs_code, fs_len, SDL_GPU_SHADERSTAGE_FRAGMENT, num_samplers, 0);
  SDL_GPUVertexBufferDescription vbd = {}; vbd.slot = 0; vbd.pitch = pitch; vbd.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
  SDL_GPUColorTargetDescription ct = {}; ct.format = SDL_GPU_TEXTUREFORMAT_R8G8_UNORM;   // VRAM (RG8), no blend
  SDL_GPUGraphicsPipelineCreateInfo gp = {};
  gp.vertex_shader = vs; gp.fragment_shader = fs;
  gp.vertex_input_state.vertex_buffer_descriptions = &vbd;
  gp.vertex_input_state.num_vertex_buffers = 1;
  gp.vertex_input_state.vertex_attributes = attrs;
  gp.vertex_input_state.num_vertex_attributes = n_attr;
  gp.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
  gp.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
  gp.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
  gp.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
  gp.depth_stencil_state.enable_depth_test = true;
  gp.depth_stencil_state.enable_depth_write = depth_write;
  gp.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;
  gp.target_info.color_target_descriptions = &ct;
  gp.target_info.num_color_targets = 1;
  gp.target_info.has_depth_stencil_target = true;
  gp.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
  SDL_GPUGraphicsPipeline* p = SDL_CreateGPUGraphicsPipeline(s_dev, &gp);
  GPUCHK(p, "CreateGPUGraphicsPipeline(geom)");
  SDL_ReleaseGPUShader(s_dev, vs); SDL_ReleaseGPUShader(s_dev, fs);
  return p;
}

// One real-HW-blend semi pipeline per PSX blend mode, targeting the float RGBA intermediate (s_color_rgba).
// Shares trisemi_hw.frag (and tritex.vert's vertex layout) across all 4 — only the blend state differs.
// See trisemi_hw.frag's header comment for the derivation: src_color_factor=ONE always; dst_color_factor=
// SRC_ALPHA reads the shader's own per-fragment STP output (0=opaque, 1=real PSX blend); the op is ADD for
// avg/add/add4 and REVERSE_SUBTRACT for sub. Depth: test against the opaque pass's depth, never write/clear.
static SDL_GPUGraphicsPipeline* make_semi_pipeline(int mode,
    const SDL_GPUVertexAttribute* attrs, Uint32 n_attr, Uint32 pitch) {
  SDL_GPUShader* vs = make_shader(spv_g_tritex_vert, spv_g_tritex_vert_len, SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
  SDL_GPUShader* fs = make_shader(spv_g_trisemi_hw_frag, spv_g_trisemi_hw_frag_len, SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 0);
  SDL_GPUVertexBufferDescription vbd = {}; vbd.slot = 0; vbd.pitch = pitch; vbd.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
  SDL_GPUColorTargetBlendState bs = {};
  bs.enable_blend = true;
  bs.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
  bs.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
  bs.color_blend_op = (mode == 2) ? SDL_GPU_BLENDOP_REVERSE_SUBTRACT : SDL_GPU_BLENDOP_ADD;   // 2 = sub
  bs.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE; bs.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
  bs.alpha_blend_op = SDL_GPU_BLENDOP_ADD;   // alpha channel unused downstream; keep it well-defined
  SDL_GPUColorTargetDescription ct = {}; ct.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT; ct.blend_state = bs;
  SDL_GPUGraphicsPipelineCreateInfo gp = {};
  gp.vertex_shader = vs; gp.fragment_shader = fs;
  gp.vertex_input_state.vertex_buffer_descriptions = &vbd;
  gp.vertex_input_state.num_vertex_buffers = 1;
  gp.vertex_input_state.vertex_attributes = attrs;
  gp.vertex_input_state.num_vertex_attributes = n_attr;
  gp.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
  gp.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
  gp.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
  gp.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
  gp.depth_stencil_state.enable_depth_test = true;
  gp.depth_stencil_state.enable_depth_write = false;
  gp.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;
  gp.target_info.color_target_descriptions = &ct;
  gp.target_info.num_color_targets = 1;
  gp.target_info.has_depth_stencil_target = true;
  gp.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
  SDL_GPUGraphicsPipeline* p = SDL_CreateGPUGraphicsPipeline(s_dev, &gp);
  GPUCHK(p, "CreateGPUGraphicsPipeline(semi)");
  SDL_ReleaseGPUShader(s_dev, vs); SDL_ReleaseGPUShader(s_dev, fs);
  return p;
}

// Per-Game GPU render TARGETS (deglobalized 2026-07-10): each Game owns its own guest-VRAM image,
// upload/readback staging, texture-atlas snapshot, depth buffer, float semi-blend intermediate and
// vertex buffers, so two Games (SBS) can never render through each other's surfaces. Lazy: needs the
// shared device up (init_gpu), then created once per Game on first touch.
void GpuGpuState::ensure_targets() {
  if (s_have_3d) return;
  s_have_3d = 1;
  SDL_GPUTextureCreateInfo vi = {};
  vi.type = SDL_GPU_TEXTURETYPE_2D; vi.format = SDL_GPU_TEXTUREFORMAT_R8G8_UNORM;
  vi.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
  vi.width = VRAM_W; vi.height = VRAM_H; vi.layer_count_or_depth = 1; vi.num_levels = 1;
  s_vram_tex = SDL_CreateGPUTexture(s_dev, &vi); GPUCHK(s_vram_tex, "CreateGPUTexture(VRAM)");
  SDL_GPUTransferBufferCreateInfo up = {}; up.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD; up.size = VRAM_W * VRAM_H * 2;
  s_vram_xfer = SDL_CreateGPUTransferBuffer(s_dev, &up); GPUCHK(s_vram_xfer, "CreateGPUTransferBuffer(up)");
  SDL_GPUTransferBufferCreateInfo dn = {}; dn.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD; dn.size = VRAM_W * VRAM_H * 2;
  s_rb_xfer = SDL_CreateGPUTransferBuffer(s_dev, &dn); GPUCHK(s_rb_xfer, "CreateGPUTransferBuffer(dn)");
  SDL_GPUTextureCreateInfo ti = {}; ti.type = SDL_GPU_TEXTURETYPE_2D; ti.format = SDL_GPU_TEXTUREFORMAT_R8G8_UNORM;
  ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER; ti.width = VRAM_W; ti.height = VRAM_H; ti.layer_count_or_depth = 1; ti.num_levels = 1;
  s_vram_snap = SDL_CreateGPUTexture(s_dev, &ti); GPUCHK(s_vram_snap, "snapshot tex");
  SDL_GPUTransferBufferCreateInfo sx = {}; sx.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD; sx.size = VRAM_W * VRAM_H * 2;
  s_snap_xfer = SDL_CreateGPUTransferBuffer(s_dev, &sx); GPUCHK(s_snap_xfer, "snap xfer");
  SDL_GPUTextureCreateInfo di = {}; di.type = SDL_GPU_TEXTURETYPE_2D; di.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
  di.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET; di.width = VRAM_W; di.height = VRAM_H; di.layer_count_or_depth = 1; di.num_levels = 1;
  s_depth = SDL_CreateGPUTexture(s_dev, &di); GPUCHK(s_depth, "depth tex");
  auto mkbuf = [](Uint32 sz, SDL_GPUBuffer** b, SDL_GPUTransferBuffer** x) {
    SDL_GPUBufferCreateInfo bi = {}; bi.usage = SDL_GPU_BUFFERUSAGE_VERTEX; bi.size = sz;
    *b = SDL_CreateGPUBuffer(s_dev, &bi); GPUCHK(*b, "vbuf");
    SDL_GPUTransferBufferCreateInfo ci = {}; ci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD; ci.size = sz;
    *x = SDL_CreateGPUTransferBuffer(s_dev, &ci); GPUCHK(*x, "vbuf xfer");
  };
  mkbuf(sizeof(TriVtx) * TRI_CAP, &s_tri_vbuf, &s_tri_xfer);
  mkbuf(sizeof(TexVtx) * TEX_CAP, &s_tex_vbuf, &s_tex_xfer);
  for (int m = 0; m < NUM_BLEND_MODES; m++) {
    mkbuf(sizeof(TexVtx) * TEX_CAP, &s_semi_vbuf[m], &s_semi_xfer[m]);
  }
  // CPU-side batch buffers stay lazily allocated per draw call (ggs_alloc_batches).
  // Float RGBA semi-blend intermediate (decode target / real-HW-blend target / encode source).
  SDL_GPUTextureCreateInfo cti = {}; cti.type = SDL_GPU_TEXTURETYPE_2D; cti.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
  cti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
  cti.width = VRAM_W; cti.height = VRAM_H; cti.layer_count_or_depth = 1; cti.num_levels = 1;
  s_color_rgba = SDL_CreateGPUTexture(s_dev, &cti); GPUCHK(s_color_rgba, "color_rgba tex");
}

// Build the 3D raster PIPELINES (shared device objects; the render targets are per-Game). Once.
static void create_3d_pipelines(void) {
  if (gdev().s_pipes_3d) return;
  static const SDL_GPUVertexAttribute tri_attr[] = {
    { 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 0 },
    { 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 8 },
    { 2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, 20 },
  };
  static const SDL_GPUVertexAttribute tex_attr[] = {
    { 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 0 },
    { 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 8 },
    { 2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 16 },
    { 3, 0, SDL_GPU_VERTEXELEMENTFORMAT_INT4, 28 },
    { 4, 0, SDL_GPU_VERTEXELEMENTFORMAT_INT4, 44 },
    { 5, 0, SDL_GPU_VERTEXELEMENTFORMAT_INT4, 60 },
    { 6, 0, SDL_GPU_VERTEXELEMENTFORMAT_INT4, 76 },
    { 7, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, 92 },
  };
  s_tri_pipe    = make_geom_pipeline(spv_g_tri_vert, spv_g_tri_vert_len, spv_g_tri_frag, spv_g_tri_frag_len,
                                     sizeof(TriVtx), tri_attr, 3, 0, true);
  s_tritex_pipe = make_geom_pipeline(spv_g_tritex_vert, spv_g_tritex_vert_len, spv_g_tritex_frag, spv_g_tritex_frag_len,
                                     sizeof(TexVtx), tex_attr, 8, 1, true);
  for (int m = 0; m < NUM_BLEND_MODES; m++) s_semi_pipe[m] = make_semi_pipeline(m, tex_attr, 8, sizeof(TexVtx));
  s_decode_pipe = make_fullscreen_offscreen_pipeline(spv_g_fsq_vert, spv_g_fsq_vert_len,
                                                      spv_g_decode_frag, spv_g_decode_frag_len,
                                                      SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT);
  s_encode_pipe = make_fullscreen_offscreen_pipeline(spv_g_fsq_vert, spv_g_fsq_vert_len,
                                                      spv_g_encode_frag, spv_g_encode_frag_len,
                                                      SDL_GPU_TEXTUREFORMAT_R8G8_UNORM);
  gdev().s_pipes_3d = 1;
  fprintf(stderr, "[gpu_gpu] 3D raster up (RG8 color target + D32 depth, real HW-blend semi via float intermediate; per-Game targets)\n");
}

static void init_gpu(Game* game) {
  s_inited = 1;
  // SDL_GPU requires the video subsystem even headless (the device is created against it; we just don't
  // open a window or claim a swapchain).
  if (!SDL_Init(SDL_INIT_VIDEO)) { fprintf(stderr, "[gpu_gpu] SDL_Init(VIDEO) failed: %s\n", SDL_GetError()); exit(2); }
  if (!s_headless) {
    int fullscreen = cfg_on("PSXPORT_FULLSCREEN")
                  || (cfg_str("PSXPORT_WINDOWED") && atoi(cfg_str("PSXPORT_WINDOWED")) == 0);
    SDL_WindowFlags flags = fullscreen ? SDL_WINDOW_FULLSCREEN : SDL_WINDOW_RESIZABLE;
    s_win = SDL_CreateWindow("Tomba! 2 (SDL_GPU)", 960, 720, flags);
    GPUCHK(s_win, "SDL_CreateWindow");
  }
  // Create the GPU device (SPIR-V shaders; let SDL pick the optimal driver — Vulkan on Linux, Metal on Mac).
  s_dev = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, cfg_on("PSXPORT_GPU_DEBUG") ? true : false, NULL);
  GPUCHK(s_dev, "SDL_CreateGPUDevice");
  fprintf(stderr, "[gpu_gpu] SDL_GPU device up (driver: %s)\n", SDL_GetGPUDeviceDriver(s_dev));
  if (!s_headless) {
    GPUCHK(SDL_ClaimWindowForGPUDevice(s_dev, s_win), "SDL_ClaimWindowForGPUDevice");
    s_swap_fmt = SDL_GetGPUSwapchainTextureFormat(s_dev, s_win);
  }

  // (the guest-VRAM image + its upload/download staging are PER-GAME now — GpuGpuState::ensure_targets.
  //  VRAM is stored R8G8_UNORM, not R16_UINT: SDL_GPU forbids SAMPLER usage on integer formats, so the
  //  uint16 LE 1555 word rides as two 8-bit channels and the shaders reconstruct it.)
  SDL_GPUSamplerCreateInfo si = {};
  si.min_filter = SDL_GPU_FILTER_NEAREST; si.mag_filter = SDL_GPU_FILTER_NEAREST;
  si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
  s_samp_nearest = SDL_CreateGPUSampler(s_dev, &si); GPUCHK(s_samp_nearest, "CreateGPUSampler(nearest)");
  si.min_filter = SDL_GPU_FILTER_LINEAR; si.mag_filter = SDL_GPU_FILTER_LINEAR;
  s_samp_linear = SDL_CreateGPUSampler(s_dev, &si); GPUCHK(s_samp_linear, "CreateGPUSampler(linear)");

  // Pipelines need the swapchain format (color target) — windowed only; headless never runs a render pass.
  if (!s_headless) {
    s_present_pipe = make_fullscreen_pipeline(spv_g_present_vert, spv_g_present_vert_len, spv_g_present_frag, spv_g_present_frag_len, s_swap_fmt);
    s_image_pipe   = make_fullscreen_pipeline(spv_g_image_vert,   spv_g_image_vert_len,   spv_g_image_frag,   spv_g_image_frag_len, s_swap_fmt);
  }
  create_3d_pipelines();   // the native 3D/textured raster pipelines — windowed AND headless
  fprintf(stderr, "[gpu_gpu] %s renderer up (VRAM %dx%d RG8 = PSX 1555)\n", s_headless ? "headless" : "windowed", VRAM_W, VRAM_H);
  // RmlUi mod/debug overlay — windowed only (it records into the swapchain present pass). No-op if its
  // assets/fonts are missing; ESC toggles it once up.
  if (!s_headless) overlay_glue_init(game, s_win, s_dev, s_swap_fmt);
}

static void poll_quit(Game* game) {
  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    overlay_glue_event(game, &e);   // RmlUi overlay: ESC toggle + mouse/keyboard nav (no-op if not inited)
    if (e.type == SDL_EVENT_QUIT) exit(0);
  }
}

// Upload the whole CPU VRAM (src, 1024*512 uint16) into THIS Game's VRAM image via a copy pass on `cmd`.
static void upload_vram(GpuGpuState& g, SDL_GPUCommandBuffer* cmd, const uint16_t* src) {
  g.ensure_targets();
  void* p = SDL_MapGPUTransferBuffer(s_dev, g.s_vram_xfer, true);
  memcpy(p, src, (size_t)VRAM_W * VRAM_H * 2);
  SDL_UnmapGPUTransferBuffer(s_dev, g.s_vram_xfer);
  SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
  SDL_GPUTextureTransferInfo srci = {}; srci.transfer_buffer = g.s_vram_xfer; srci.pixels_per_row = VRAM_W; srci.rows_per_layer = VRAM_H;
  SDL_GPUTextureRegion dst = {}; dst.texture = g.s_vram_tex; dst.w = VRAM_W; dst.h = VRAM_H; dst.d = 1;
  SDL_UploadToGPUTexture(cp, &srci, &dst, false);
  SDL_EndGPUCopyPass(cp);
}

// Compute the letterboxed viewport for an aspect aw:ah within the window (ow x oh).
static SDL_GPUViewport letterbox(int aw, int ah, int ow, int oh) {
  int dw, dh;
  if (ow * ah >= oh * aw) { dh = oh; dw = oh * aw / ah; } else { dw = ow; dh = ow * ah / aw; }
  SDL_GPUViewport vp = { (float)((ow - dw) / 2), (float)((oh - dh) / 2), (float)dw, (float)dh, 0.0f, 1.0f };
  return vp;
}

// Upload this frame's VRAM snapshot (texture/CLUT atlas source) + the geometry batches, then render in
// THREE steps so semi-transparent world quads use the GPU's REAL fixed-function blend unit against THIS
// FRAME's actual rendered content — no manual "sample a destination texture" in the shader, no snapshot
// staleness window:
//   Pass A: flat + textured-OPAQUE onto s_vram_tex (over the uploaded 2D backdrop), depth CLEARed+STOREd.
//   Decode: fullscreen pass unpacks s_vram_tex (now holding this frame's opaque content) into s_color_rgba,
//     a float RGBA target — packed-1555 can't be HW-blended correctly (its 5-bit channels straddle byte
//     boundaries), so blending needs a real per-channel format.
//   Pass B (one draw call per non-empty PSX blend mode bucket): textured-SEMI into s_color_rgba with the
//     GPU's OWN blend state (see make_semi_pipeline / trisemi_hw.frag) — the hardware reads the color
//     target's CURRENT content directly, so this is always this frame's real background, never stale.
//     Depth LOADed from Pass A (tests, never writes/clears), so semi still respects opaque occlusion.
//   Encode: fullscreen pass packs s_color_rgba back into s_vram_tex (1555) for present/shot/vkvram/etc.
// This replaced an in-shader blend against a legacy CPU-uploaded VRAM snapshot: for the native (non-
// legacy-2D) render path that buffer is mostly empty/stale in the display region, so a semi quad whose
// vertex colour additively fades toward black (meant to blend to near-invisible against whatever's behind
// it) instead blended against emptiness and rendered solid black (2026-07-01 dark-outline root cause,
// scratch/handoff.md). A later same-day attempt fixed that by GPU-copying s_vram_tex into the snapshot
// right after the opaque pass — correct destination, but still hand-rolled blend math, and it introduced a
// transient wrong-colour flash on the first couple of frames after a scene fade-in (root cause not fully
// chased down before this rewrite; REAL hardware blending removes the whole class of bug instead).
// No-op if nothing was batched. Reports the drawn counts for vkstats.
static void render_geom(GpuGpuState& g, SDL_GPUCommandBuffer* cmd, const uint16_t* src, int* dtri, int* dtex, int* dsemi) {
  int semi_total = 0; for (int m = 0; m < NUM_BLEND_MODES; m++) semi_total += g.s_semi_n[m];
  *dtri = g.s_tri_n; *dtex = g.s_tex_n; *dsemi = semi_total;
  if ((g.s_tri_n + g.s_tex_n + semi_total) == 0) return;
  g.ensure_targets();
  { void* p = SDL_MapGPUTransferBuffer(s_dev, g.s_snap_xfer, true); memcpy(p, src, (size_t)VRAM_W*VRAM_H*2); SDL_UnmapGPUTransferBuffer(s_dev, g.s_snap_xfer); }
  if (g.s_tri_n)  { void* p = SDL_MapGPUTransferBuffer(s_dev, g.s_tri_xfer, true);  memcpy(p, g.s_tri_buf,  (size_t)g.s_tri_n*sizeof(TriVtx));  SDL_UnmapGPUTransferBuffer(s_dev, g.s_tri_xfer); }
  if (g.s_tex_n)  { void* p = SDL_MapGPUTransferBuffer(s_dev, g.s_tex_xfer, true);  memcpy(p, g.s_tex_buf,  (size_t)g.s_tex_n*sizeof(TexVtx));  SDL_UnmapGPUTransferBuffer(s_dev, g.s_tex_xfer); }
  for (int m = 0; m < NUM_BLEND_MODES; m++) if (g.s_semi_n[m]) {
    void* p = SDL_MapGPUTransferBuffer(s_dev, g.s_semi_xfer[m], true);
    memcpy(p, g.s_semi_buf[m], (size_t)g.s_semi_n[m]*sizeof(TexVtx));
    SDL_UnmapGPUTransferBuffer(s_dev, g.s_semi_xfer[m]);
  }
  SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
  { SDL_GPUTextureTransferInfo si = {}; si.transfer_buffer = g.s_snap_xfer; si.pixels_per_row = VRAM_W; si.rows_per_layer = VRAM_H;
    SDL_GPUTextureRegion dr = {}; dr.texture = g.s_vram_snap; dr.w = VRAM_W; dr.h = VRAM_H; dr.d = 1;
    SDL_UploadToGPUTexture(cp, &si, &dr, false); }
  auto upv = [&](SDL_GPUTransferBuffer* x, SDL_GPUBuffer* b, int n, Uint32 stride){ if (!n) return;
    SDL_GPUTransferBufferLocation s = {}; s.transfer_buffer = x;
    SDL_GPUBufferRegion d = {}; d.buffer = b; d.offset = 0; d.size = (Uint32)n*stride;
    SDL_UploadToGPUBuffer(cp, &s, &d, false); };
  upv(g.s_tri_xfer, g.s_tri_vbuf, g.s_tri_n, sizeof(TriVtx));
  upv(g.s_tex_xfer, g.s_tex_vbuf, g.s_tex_n, sizeof(TexVtx));
  for (int m = 0; m < NUM_BLEND_MODES; m++) upv(g.s_semi_xfer[m], g.s_semi_vbuf[m], g.s_semi_n[m], sizeof(TexVtx));
  SDL_EndGPUCopyPass(cp);
  SDL_GPUTextureSamplerBinding snap = { g.s_vram_snap, s_samp_nearest };
  SDL_GPUBufferBinding bb = {}; bb.offset = 0;
  SDL_GPUViewport vp = { 0, 0, (float)VRAM_W, (float)VRAM_H, 0.0f, 1.0f }; SDL_Rect sc = { 0, 0, VRAM_W, VRAM_H };
  // ---- Pass A: opaque (flat + textured) -----------------------------------------------------------
  {
    SDL_GPUColorTargetInfo ct = {}; ct.texture = g.s_vram_tex; ct.load_op = SDL_GPU_LOADOP_LOAD; ct.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPUDepthStencilTargetInfo dt = {}; dt.texture = g.s_depth; dt.clear_depth = 0.0f;
    dt.load_op = SDL_GPU_LOADOP_CLEAR; dt.store_op = SDL_GPU_STOREOP_STORE;   // STORE: the semi pass reuses this depth
    dt.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE; dt.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(cmd, &ct, 1, &dt);
    SDL_SetGPUViewport(rp, &vp); SDL_SetGPUScissor(rp, &sc);
    if (g.s_tri_n) { SDL_BindGPUGraphicsPipeline(rp, s_tri_pipe); bb.buffer = g.s_tri_vbuf; SDL_BindGPUVertexBuffers(rp, 0, &bb, 1); SDL_DrawGPUPrimitives(rp, g.s_tri_n, 1, 0, 0); }
    if (g.s_tex_n) { SDL_BindGPUGraphicsPipeline(rp, s_tritex_pipe); bb.buffer = g.s_tex_vbuf; SDL_BindGPUVertexBuffers(rp, 0, &bb, 1);
                   SDL_BindGPUFragmentSamplers(rp, 0, &snap, 1); SDL_DrawGPUPrimitives(rp, g.s_tex_n, 1, 0, 0); }
    SDL_EndGPURenderPass(rp);
  }
  if (!semi_total) return;
  // ---- decode: s_vram_tex (this frame's opaque content) -> s_color_rgba (float, real-blend target) ----
  {
    SDL_GPUColorTargetInfo ct = {}; ct.texture = g.s_color_rgba; ct.load_op = SDL_GPU_LOADOP_DONT_CARE; ct.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(cmd, &ct, 1, nullptr);
    SDL_SetGPUViewport(rp, &vp); SDL_SetGPUScissor(rp, &sc);
    SDL_GPUTextureSamplerBinding vramtex = { g.s_vram_tex, s_samp_nearest };
    SDL_BindGPUGraphicsPipeline(rp, s_decode_pipe); SDL_BindGPUFragmentSamplers(rp, 0, &vramtex, 1);
    SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
    SDL_EndGPURenderPass(rp);
  }
  // ---- Pass B: textured-semi, one draw call per non-empty blend-mode bucket, REAL HW blend, testing
  //      (not clearing/writing) Pass A's depth ----------------------------------------------------------
  {
    SDL_GPUColorTargetInfo ct = {}; ct.texture = g.s_color_rgba; ct.load_op = SDL_GPU_LOADOP_LOAD; ct.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPUDepthStencilTargetInfo dt = {}; dt.texture = g.s_depth;
    dt.load_op = SDL_GPU_LOADOP_LOAD; dt.store_op = SDL_GPU_STOREOP_DONT_CARE;
    dt.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE; dt.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(cmd, &ct, 1, &dt);
    SDL_SetGPUViewport(rp, &vp); SDL_SetGPUScissor(rp, &sc);
    for (int m = 0; m < NUM_BLEND_MODES; m++) if (g.s_semi_n[m]) {
      SDL_BindGPUGraphicsPipeline(rp, s_semi_pipe[m]); bb.buffer = g.s_semi_vbuf[m]; SDL_BindGPUVertexBuffers(rp, 0, &bb, 1);
      SDL_BindGPUFragmentSamplers(rp, 0, &snap, 1); SDL_DrawGPUPrimitives(rp, g.s_semi_n[m], 1, 0, 0);
    }
    SDL_EndGPURenderPass(rp);
  }
  // ---- encode: s_color_rgba -> s_vram_tex (1555), for present/shot/vkvram/provat/SBS -------------------
  {
    SDL_GPUColorTargetInfo ct = {}; ct.texture = g.s_vram_tex; ct.load_op = SDL_GPU_LOADOP_DONT_CARE; ct.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(cmd, &ct, 1, nullptr);
    SDL_SetGPUViewport(rp, &vp); SDL_SetGPUScissor(rp, &sc);
    SDL_GPUTextureSamplerBinding colorrgba = { g.s_color_rgba, s_samp_nearest };
    SDL_BindGPUGraphicsPipeline(rp, s_encode_pipe); SDL_BindGPUFragmentSamplers(rp, 0, &colorrgba, 1);
    SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
    SDL_EndGPURenderPass(rp);
  }
}

// ---- present: upload CPU VRAM, render the 3D/textured batch on top, sample [sx,sy,w,h] to the swapchain
static void dump_to(GpuGpuState& g, const char*, int, int, int, int, int, uint8_t, uint8_t, uint8_t);   // fwd (defined below) — preseq dump
void GpuGpuState::present(const uint16_t* src, int sx, int sy, int w, int h) {
  if (!gpu_gpu_enabled()) return;
  if (!s_inited) init_gpu(game);
  // Widescreen: the engine renders a wider FOV into VRAM columns [sx, sx+nw). Everything downstream (the
  // windowed present sample region AND the `shot`/vkshot readback, which use s_last_w) must span that wide
  // width, else the wide FB is cropped back to the 4:3 s_disp_w. At 4:3 nw==320 so w is unchanged.
  int disp_w = w;
  if (gpu_gpu_wide_engine(&game->core)) disp_w = gpu_gpu_wide_engine_w(&game->core);
  s_present_sx = sx; s_present_sy = sy;
  s_last_sx = sx; s_last_sy = sy; s_last_w = disp_w; s_last_h = h;

  // PSXPORT_GPU_TRACE: per-present source-VRAM occupancy + sampled display region (diagnostic).
  if (cfg_on("PSXPORT_GPU_TRACE")) { int& n = gdev().s_trace_n; if (n++ < 4 || (n % 200) == 0) {
    long nz = 0; for (long i = 0; i < (long)VRAM_W * VRAM_H; i++) if (src[i]) nz++;
    int semi_total = 0; for (int m = 0; m < NUM_BLEND_MODES; m++) semi_total += s_semi_n[m];
    fprintf(stderr, "[gpu_gpu] present #%d src nonzero=%ld/%d disp=%d,%d %dx%d | batch tri=%d tex=%d semi=%d\n",
            n, nz, VRAM_W*VRAM_H, sx, sy, w, h, s_tri_n, s_tex_n, semi_total); } }
  // `debug fadewatch`: per-present log of the ScreenFade state (the PC-native subsystem that owns fade).
  ScreenFade::State fade = game->core.screenFade.get();
  if (cfg_dbg("fadewatch")) { GpuDevice& gd = gdev();
    int& lastmode = gd.s_fw_lastmode; uint8_t& lr = gd.s_fw_lr; uint8_t& lg = gd.s_fw_lg; uint8_t& lb = gd.s_fw_lb;
    int& lsx = gd.s_fw_lsx; int& lsy = gd.s_fw_lsy; int& lw = gd.s_fw_lw; int& lh = gd.s_fw_lh;
    if (fade.mode != lastmode || fade.r != lr || fade.g != lg || fade.b != lb ||
        sx != lsx || sy != lsy || w != lw || h != lh) {
      fprintf(stderr, "[fadewatch] present disp=%d,%d %dx%d fade mode=%d rgb=(%d,%d,%d)\n",
              sx, sy, w, h, fade.mode, fade.r, fade.g, fade.b);
      lastmode = fade.mode; lr = fade.r; lg = fade.g; lb = fade.b;
      lsx = sx; lsy = sy; lw = w; lh = h;
    }
  }
  SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(s_dev);
  GPUCHK(cmd, "AcquireGPUCommandBuffer");
  upload_vram(*this, cmd, src);                             // CPU VRAM -> THIS Game's VRAM image (2D backdrop)
  render_geom(*this, cmd, src, &s_dbg_tri_c, &s_dbg_tex_c, &s_dbg_semi_c);   // draw the tee batch on top (+depth)

  if (s_headless) { SDL_SubmitGPUCommandBuffer(cmd); return; }   // shot reads s_vram_tex via its own cmd

  SDL_GPUTexture* swaptex = NULL; Uint32 sw = 0, sh = 0;
  if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, s_win, &swaptex, &sw, &sh) || !swaptex) {
    SDL_SubmitGPUCommandBuffer(cmd); poll_quit(game); return;   // minimized / no swapchain image this frame
  }
  SDL_GPUColorTargetInfo cti = {};
  cti.texture = swaptex; cti.clear_color = (SDL_FColor){ 0, 0, 0, 1 };
  cti.load_op = SDL_GPU_LOADOP_CLEAR; cti.store_op = SDL_GPU_STOREOP_STORE;
  SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(cmd, &cti, 1, NULL);

  // Widescreen present (disp_w computed above): SAMPLE the wide FB region [sx, sx+nw) and letterbox to the
  // aspect's display shape (nw:240), else the wide FB is squeezed into a 4:3 box. At 4:3 nw==320 = old path.
  PresentPC pc; pc.disp[0] = sx; pc.disp[1] = sy; pc.disp[2] = disp_w; pc.disp[3] = h;
  pc.fade[0] = fade.mode; pc.fade[1] = fade.r; pc.fade[2] = fade.g; pc.fade[3] = fade.b;
  SDL_PushGPUFragmentUniformData(cmd, 0, &pc, sizeof pc);

  SDL_GPUViewport vp = letterbox(disp_w, 240, (int)sw, (int)sh);
  SDL_Rect sc = { 0, 0, (int)sw, (int)sh };
  SDL_BindGPUGraphicsPipeline(rp, s_present_pipe);
  SDL_SetGPUViewport(rp, &vp); SDL_SetGPUScissor(rp, &sc);
  SDL_GPUTextureSamplerBinding tsb = { s_vram_tex, s_samp_nearest };
  SDL_BindGPUFragmentSamplers(rp, 0, &tsb, 1);
  SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
  // RmlUi mod/debug overlay (ESC) composites ON TOP of the game frame, into the same present pass over
  // the FULL window. No-op when the menu is hidden / overlay not inited.
  overlay_glue_record(game, cmd, rp, (int)sw, (int)sh);
  SDL_EndGPURenderPass(rp);
  SDL_SubmitGPUCommandBuffer(cmd);
  poll_quit(game);
}

// ---- present_image: draw a plain RGBA8 image fullscreen (letterboxed 4:3), rgb scaled by `fade` -----
static void img_make_tex(int iw, int ih) {
  if (s_img_tex && s_img_w == iw && s_img_h == ih) return;
  if (s_img_tex) { SDL_ReleaseGPUTexture(s_dev, s_img_tex); SDL_ReleaseGPUTransferBuffer(s_dev, s_img_xfer); }
  SDL_GPUTextureCreateInfo ti = {};
  ti.type = SDL_GPU_TEXTURETYPE_2D; ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
  ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER; ti.width = (Uint32)iw; ti.height = (Uint32)ih;
  ti.layer_count_or_depth = 1; ti.num_levels = 1;
  s_img_tex = SDL_CreateGPUTexture(s_dev, &ti); GPUCHK(s_img_tex, "CreateGPUTexture(image)");
  SDL_GPUTransferBufferCreateInfo up = {}; up.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD; up.size = (Uint32)iw * ih * 4;
  s_img_xfer = SDL_CreateGPUTransferBuffer(s_dev, &up); GPUCHK(s_img_xfer, "CreateGPUTransferBuffer(image)");
  s_img_w = iw; s_img_h = ih;
}
void gpu_gpu_present_image(Core* core, const uint8_t* rgba, int iw, int ih, float fade) {
  Game* game = core ? core->game : nullptr;
  if (!gpu_gpu_enabled() || iw <= 0 || ih <= 0) return;
  if (!s_inited) init_gpu(game);
  img_make_tex(iw, ih);
  if (fade < 0.0f) fade = 0.0f; if (fade > 1.0f) fade = 1.0f;

  SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(s_dev);
  GPUCHK(cmd, "AcquireGPUCommandBuffer");
  void* p = SDL_MapGPUTransferBuffer(s_dev, s_img_xfer, true);
  memcpy(p, rgba, (size_t)iw * ih * 4);
  SDL_UnmapGPUTransferBuffer(s_dev, s_img_xfer);
  SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
  SDL_GPUTextureTransferInfo srci = {}; srci.transfer_buffer = s_img_xfer; srci.pixels_per_row = (Uint32)iw; srci.rows_per_layer = (Uint32)ih;
  SDL_GPUTextureRegion dst = {}; dst.texture = s_img_tex; dst.w = (Uint32)iw; dst.h = (Uint32)ih; dst.d = 1;
  SDL_UploadToGPUTexture(cp, &srci, &dst, false);
  SDL_EndGPUCopyPass(cp);

  if (s_headless) { SDL_SubmitGPUCommandBuffer(cmd); return; }   // caller PPM-dumps its own rgba headless

  SDL_GPUTexture* swaptex = NULL; Uint32 sw = 0, sh = 0;
  if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, s_win, &swaptex, &sw, &sh) || !swaptex) {
    SDL_SubmitGPUCommandBuffer(cmd); poll_quit(game); return;
  }
  SDL_GPUColorTargetInfo cti = {};
  cti.texture = swaptex; cti.clear_color = (SDL_FColor){ 0, 0, 0, 1 };
  cti.load_op = SDL_GPU_LOADOP_CLEAR; cti.store_op = SDL_GPU_STOREOP_STORE;
  SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(cmd, &cti, 1, NULL);
  float fpc[4] = { fade, 0, 0, 0 };
  SDL_PushGPUFragmentUniformData(cmd, 0, fpc, sizeof fpc);
  SDL_GPUViewport vp = letterbox(4, 3, (int)sw, (int)sh);
  SDL_Rect sc = { 0, 0, (int)sw, (int)sh };
  SDL_BindGPUGraphicsPipeline(rp, s_image_pipe);
  SDL_SetGPUViewport(rp, &vp); SDL_SetGPUScissor(rp, &sc);
  SDL_GPUTextureSamplerBinding tsb = { s_img_tex, s_samp_linear };
  SDL_BindGPUFragmentSamplers(rp, 0, &tsb, 1);
  SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
  // RmlUi mod/debug overlay (ESC) composites ON TOP of the image, same as the game present (gpu_present
  // above) — otherwise the manually-drawn SCEA splash would cover the overlay. No-op when hidden.
  overlay_glue_record(game, cmd, rp, (int)sw, (int)sh);
  SDL_EndGPURenderPass(rp);
  SDL_SubmitGPUCommandBuffer(cmd);
  poll_quit(game);
}

// ---- readback (shot / vram dump): download THIS Game's VRAM image → host, decode 1555 → PPM ---------
static const uint16_t* readback_vram(GpuGpuState& g) {
  g.ensure_targets();
  SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(s_dev); GPUCHK(cmd, "AcquireGPUCommandBuffer");
  SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
  SDL_GPUTextureRegion srcr = {}; srcr.texture = g.s_vram_tex; srcr.w = VRAM_W; srcr.h = VRAM_H; srcr.d = 1;
  SDL_GPUTextureTransferInfo dsti = {}; dsti.transfer_buffer = g.s_rb_xfer; dsti.pixels_per_row = VRAM_W; dsti.rows_per_layer = VRAM_H;
  SDL_DownloadFromGPUTexture(cp, &srcr, &dsti);
  SDL_EndGPUCopyPass(cp);
  SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
  SDL_WaitForGPUFences(s_dev, true, &fence, 1);
  SDL_ReleaseGPUFence(s_dev, fence);
  const uint16_t* p = (const uint16_t*)SDL_MapGPUTransferBuffer(s_dev, g.s_rb_xfer, false);
  if (cfg_on("PSXPORT_GPU_TRACE")) { long nz = 0; for (long i = 0; i < (long)VRAM_W * VRAM_H; i++) if (p[i]) nz++;
    fprintf(stderr, "[gpu_gpu] readback nonzero=%ld/%d\n", nz, VRAM_W * VRAM_H); }
  return p;
}
#include <SDL3_image/SDL_image.h>
// THE one place any shot/dump turns an RGB24 buffer into a file. Format is chosen by extension, and
// PNG is the DEFAULT (a bare or unknown extension writes PNG) so callers get a directly-viewable file
// with no PPM->PNG convert step — only an explicit `.ppm` path keeps the raw P6 dump. Shared by the VK
// readback (dump_to) and the software-GPU shot (gpu_native.cpp) so the rule can't drift between them.
void image_write_rgb24(const char* path, const unsigned char* rgb, int w, int h) {
  size_t n = strlen(path);
  bool ppm = (n > 4 && strcmp(path + n - 4, ".ppm") == 0);
  if (ppm) {
    FILE* f = fopen(path, "wb");
    if (f) { fprintf(f, "P6\n%d %d\n255\n", w, h); fwrite(rgb, 3, (size_t)w * h, f); fclose(f); }
    return;
  }
  SDL_Surface* s = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGB24, (void*)rgb, w * 3);
  if (s) { IMG_SavePNG(s, path); SDL_DestroySurface(s); }
}
static void dump_to(GpuGpuState& g, const char* path, int sx, int sy, int w, int h,
                    int fade_mode, uint8_t fade_r, uint8_t fade_g, uint8_t fade_b) {
  const uint16_t* vram = readback_vram(g);
  unsigned char* rgb = (unsigned char*)malloc((size_t)w * h * 3);
  if (!rgb) { SDL_UnmapGPUTransferBuffer(s_dev, g.s_rb_xfer); return; }
  for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
    uint16_t p = vram[((sy + y) % VRAM_H) * VRAM_W + ((sx + x) & 1023)];
    int r = (p & 31) << 3, g = ((p >> 5) & 31) << 3, b = ((p >> 10) & 31) << 3;
    if (fade_mode == 1)      { r += fade_r; g += fade_g; b += fade_b; if (r>255)r=255; if (g>255)g=255; if (b>255)b=255; }
    else if (fade_mode == 2) { r -= fade_r; g -= fade_g; b -= fade_b; if (r<0)r=0; if (g<0)g=0; if (b<0)b=0; }
    unsigned char* c = &rgb[((size_t)y * w + x) * 3];
    c[0] = (unsigned char)r; c[1] = (unsigned char)g; c[2] = (unsigned char)b;
  }
  image_write_rgb24(path, rgb, w, h);
  free(rgb);
  SDL_UnmapGPUTransferBuffer(s_dev, g.s_rb_xfer);
}
void GpuGpuState::shot(const char* path) {
  if (!gpu_gpu_enabled() || !s_inited) { fprintf(stderr, "[gpu_shot] GPU not active\n"); return; }
  ScreenFade::State f = game->core.screenFade.get();
  dump_to(*this, path, s_last_sx, s_last_sy, s_last_w, s_last_h, f.mode, f.r, f.g, f.b);
  fprintf(stderr, "[gpu_shot] wrote %s (%dx%d @ %d,%d)\n", path, s_last_w, s_last_h, s_last_sx, s_last_sy);
}
void GpuGpuState::shot_b(const char* path) { shot(path); }   // Pass 1: single target
void gpu_gpu_shot_region(Core* core, const char* path, int sx, int sy, int w, int h) {
  if (!gpu_gpu_enabled() || !s_inited) return;
  ScreenFade::State f = core->screenFade.get();
  dump_to(core->game->gpu_gpu, path, sx, sy, w, h, f.mode, f.r, f.g, f.b);
  fprintf(stderr, "[gpu_shot] wrote %s (%dx%d @ %d,%d)\n", path, w, h, sx, sy);
}
void gpu_gpu_vram_region(Core* core, const char* path, int x, int y, int w, int h) {
  if (!gpu_gpu_enabled() || !s_inited) return;
  dump_to(core->game->gpu_gpu, path, x, y, w, h, 0, 0, 0, 0);   // raw VRAM region dump — no engine fade applied
  fprintf(stderr, "[gpu_vram] wrote %s (%dx%d @ %d,%d)\n", path, w, h, x, y);
}
// Raw 16-bit VRAM words at (x,y..y+n-1 wrapped along X) — dark-outline STP-bit diag (2026-07-01,
// scratch/handoff.md): tells apart a genuine opaque texel (STP=0, faithful) from a lost/miscomputed
// STP bit (would-be-blended texel drawing solid instead of translucent).
void gpu_gpu_vram_words(Core* core, int x, int y, int n, uint16_t* out) {
  if (!gpu_gpu_enabled() || !s_inited) { for (int i = 0; i < n; i++) out[i] = 0; return; }
  GpuGpuState& g = core->game->gpu_gpu;
  const uint16_t* vram = readback_vram(g);
  for (int i = 0; i < n; i++) out[i] = vram[(y % VRAM_H) * VRAM_W + ((x + i) & 1023)];
  SDL_UnmapGPUTransferBuffer(s_dev, g.s_rb_xfer);
}
void gpu_gpu_vram_raw(Core* core, const char* path) {
  if (!gpu_gpu_enabled() || !s_inited) return;
  GpuGpuState& g = core->game->gpu_gpu;
  const uint16_t* vram = readback_vram(g);
  FILE* f = fopen(path, "wb"); if (!f) { SDL_UnmapGPUTransferBuffer(s_dev, g.s_rb_xfer); return; }
  for (int y = 0; y < VRAM_H; y++) fwrite(&vram[y * VRAM_W], 2, VRAM_W, f);
  fclose(f);
  SDL_UnmapGPUTransferBuffer(s_dev, g.s_rb_xfer);
  fprintf(stderr, "[gpu_vram] wrote RAW %s (%dx%d u16)\n", path, VRAM_W, VRAM_H);
}

// ---- per-prim state setters (depth/order; consumed by the 3D raster) --------------------------------
void GpuGpuState::set_vd(const float* d3) { s_vd = d3; }
void GpuGpuState::set_vd_n(const float* d3) { s_vdn = d3; }
void GpuGpuState::set_xyf(const float* xf, const float* yf) { s_xf = xf; s_yf = yf; }
// Paint-order z-fight TIEBREAK. ZBIAS_UNIT = the depth nudge per emit-order step; ZBIAS_MAX = the reserved
// headroom cap (the accumulated bias never exceeds this, so it can never push a 3D prim past NATIVE_3D_MAX
// into the 2D/HUD band nor overrun a genuine world depth separation — measured world separations near the
// camera are ~1e-3+, an order of magnitude above ZBIAS_MAX). Tunable via PSXPORT_ZBIAS for sweeps.
// Exposed (non-static) for the zfight scanner (render_queue.cpp) so it can model the fix without a re-run.
float gpu_zbias_unit() { static float u = -1.f; if (u < 0.f) { const char* e = cfg_str("PSXPORT_ZBIAS");
                                                                u = e ? (float)atof(e) : 4e-7f; if (u < 0.f) u = 0.f; } return u; }
#define ZBIAS_MAX 1.5e-3f
void GpuGpuState::set_order(unsigned idx) { s_cur_ord = (float)(idx + 1) / 65536.0f; if (s_cur_ord > 1.0f) s_cur_ord = 1.0f;
                                           s_cur_ordn = s_cur_ord; s_vd = 0; s_vdn = 0; s_xf = 0; s_yf = 0;
                                           float b = (float)idx * gpu_zbias_unit(); s_depth_bias = b > ZBIAS_MAX ? ZBIAS_MAX : b; }
void GpuGpuState::set_order_2d(unsigned idx) { float t = (float)(idx + 1) / 65536.0f; if (t > 1.0f) t = 1.0f;
                                              s_cur_ord = NATIVE_3D_MAX + (1.0f - NATIVE_3D_MAX) * t; s_vd = 0; }
void GpuGpuState::set_order_2d_n(unsigned idx) { float t = (float)(idx + 1) / 65536.0f; if (t > 1.0f) t = 1.0f;
                                                s_cur_ordn = NATIVE_3D_MAX + (1.0f - NATIVE_3D_MAX) * t; s_vdn = 0; }
void GpuGpuState::set_order_2d_bg(unsigned idx) { float t = (float)(idx + 1) / 65536.0f; if (t > 1.0f) t = 1.0f;
                                                 s_cur_ord = NATIVE_3D_MIN * t; s_vd = 0; }
void GpuGpuState::set_order_2d_bg_n(unsigned idx) { float t = (float)(idx + 1) / 65536.0f; if (t > 1.0f) t = 1.0f;
                                                   s_cur_ordn = NATIVE_3D_MIN * t; s_vdn = 0; }
void GpuGpuState::semi_group(int x0, int y0, int x1, int y1) { (void)x0; (void)y0; (void)x1; (void)y1; }
void GpuGpuState::dirty(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; }
void GpuGpuState::stats(int* tri, int* tex, int* semi) { if (tri) *tri = s_dbg_tri_c; if (tex) *tex = s_dbg_tex_c; if (semi) *semi = s_dbg_semi_c; }
// Per-logic-frame reset of the host geometry batch (present consumed it; the next frame re-emits the queue).
void GpuGpuState::frame_end(const uint16_t* svram, int frame) { (void)svram; (void)frame;
  // PRESENT-SEQUENCE capture (REPL `preseq <N> [dir]`): frame_end runs once per PRESENT PASS — the
  // real pass AND the fps60 interp pass, windowed AND headless — so dumping here (before the batch
  // reset; dump_to's readback renders the pending batch) interleaves real/interp frames for
  // tools/preseq_flicker.py's 30Hz-oscillation detection.
  if (s_preseq_left > 0) {
    char p[192]; snprintf(p, sizeof p, "%s/p%04d.ppm", s_preseq_dir, s_preseq_idx++);
    ScreenFade::State f = game->core.screenFade.get();
    dump_to(*this, p, s_last_sx, s_last_sy, s_last_w, s_last_h, f.mode, f.r, f.g, f.b);
    if (--s_preseq_left == 0) fprintf(stderr, "[preseq] done: %d frames -> %s\n", s_preseq_idx, s_preseq_dir);
  }
  s_tri_n = s_tex_n = 0;
  for (int m = 0; m < NUM_BLEND_MODES; m++) s_semi_n[m] = 0; }

// ---- native 3D / textured raster: accumulate the tee'd geometry into the host batch (Pass 2) ---------
// Append one flat triangle (VRAM coords + per-vertex RGB 0..255); depth = per-vertex native (s_vd) or the
// per-prim OT-order (s_cur_ord) band.
// Lazy-alloc THIS core's CPU vertex batches on first use (the VK backend's create_3d already ran on
// s_have_3d = 1, but the batches live on GpuGpuState now so each Core allocates its own).
static inline void ggs_alloc_batches(GpuGpuState& g) {
  if (!g.s_tri_buf)  g.s_tri_buf  = (TriVtx*)malloc(sizeof(TriVtx) * TRI_CAP);
  if (!g.s_tex_buf)  g.s_tex_buf  = (TexVtx*)malloc(sizeof(TexVtx) * TEX_CAP);
  for (int m = 0; m < NUM_BLEND_MODES; m++)
    if (!g.s_semi_buf[m]) g.s_semi_buf[m] = (TexVtx*)malloc(sizeof(TexVtx) * TEX_CAP);
}

void GpuGpuState::draw_tri(int x0,int y0,int r0,int g0,int b0, int x1,int y1,int r1,int g1,int b1,
                          int x2,int y2,int r2,int g2,int b2) {
  if (s_tri_n + 3 > TRI_CAP) return;
  ggs_alloc_batches(*this);
  TriVtx* v = ((TriVtx*)s_tri_buf) + s_tri_n;
  v[0] = { (float)x0, (float)y0, r0/255.f, g0/255.f, b0/255.f, s_vd ? ord3d_b(s_vd[0], s_depth_bias) : s_cur_ord };
  v[1] = { (float)x1, (float)y1, r1/255.f, g1/255.f, b1/255.f, s_vd ? ord3d_b(s_vd[1], s_depth_bias) : s_cur_ord };
  v[2] = { (float)x2, (float)y2, r2/255.f, g2/255.f, b2/255.f, s_vd ? ord3d_b(s_vd[2], s_depth_bias) : s_cur_ord };
  s_tri_n += 3;
}
// Fill 3 textured vertices: per-vertex pos/uv/color + shared page/CLUT/window/clip/semi/blend state. Uses
// the sub-pixel float XY (s_xf/s_yf) for the world path when set, else the integer xs/ys (2D/HUD).
void GpuGpuState::tex_emit(TexVtx* t, const int* xs, const int* ys, const int* us, const int* vs,
                          const unsigned char* rs, const unsigned char* gs, const unsigned char* bs,
                          int tpx, int tpy, int mode, int raw, int clutx, int cluty,
                          int twmx, int twmy, int twox, int twoy, int dax0, int day0, int dax1, int day1,
                          int semi, int blend) {
  for (int i = 0; i < 3; i++) {
    t[i].x = s_xf ? s_xf[i] : (float)xs[i];
    t[i].y = s_yf ? s_yf[i] : (float)ys[i];
    t[i].u = us[i]; t[i].v = vs[i];
    t[i].r = rs[i]/255.f; t[i].g = gs[i]/255.f; t[i].b = bs[i]/255.f;
    t[i].tp[0] = tpx; t[i].tp[1] = tpy; t[i].tp[2] = mode; t[i].tp[3] = raw;
    t[i].clut[0] = clutx; t[i].clut[1] = cluty; t[i].clut[2] = semi; t[i].clut[3] = blend;
    t[i].tw[0] = twmx; t[i].tw[1] = twmy; t[i].tw[2] = twox; t[i].tw[3] = twoy;
    t[i].da[0] = dax0; t[i].da[1] = day0; t[i].da[2] = dax1; t[i].da[3] = day1;
    t[i].ord = s_vd ? ord3d_b(s_vd[i], s_depth_bias) : s_cur_ord;
  }
}
void GpuGpuState::draw_tritri(const int* xs, const int* ys, const int* us, const int* vs,
                             const unsigned char* rs, const unsigned char* gs, const unsigned char* bs,
                             int tpx, int tpy, int mode, int raw, int clutx, int cluty,
                             int twmx, int twmy, int twox, int twoy, int dax0, int day0, int dax1, int day1) {
  if (s_tex_n + 3 > TEX_CAP) return;
  ggs_alloc_batches(*this);
  tex_emit(((TexVtx*)s_tex_buf) + s_tex_n, xs, ys, us, vs, rs, gs, bs, tpx, tpy, mode, raw, clutx, cluty,
           twmx, twmy, twox, twoy, dax0, day0, dax1, day1, 0, 0);
  s_tex_n += 3;
}
void GpuGpuState::draw_semi(const int* xs, const int* ys, const int* us, const int* vs,
                           const unsigned char* rs, const unsigned char* gs, const unsigned char* bs,
                           int tpx, int tpy, int mode, int raw, int clutx, int cluty,
                           int twmx, int twmy, int twox, int twoy, int dax0, int day0, int dax1, int day1, int blend) {
  int m = blend & 3;   // bucket by PSX blend mode: one HW-blend pipeline/vertex-buffer per mode (see render_geom)
  if (s_semi_n[m] + 3 > TEX_CAP) return;
  ggs_alloc_batches(*this);
  tex_emit(((TexVtx*)s_semi_buf[m]) + s_semi_n[m], xs, ys, us, vs, rs, gs, bs, tpx, tpy, mode, raw, clutx, cluty,
           twmx, twmy, twox, twoy, dax0, day0, dax1, day1, 1, blend);
  s_semi_n[m] += 3;
}
void GpuGpuState::tri_render_and_readback(uint16_t* out) { (void)out; }
void GpuGpuState::tri_over_bg_readback(const uint16_t* bg, uint16_t* out) { (void)bg; (void)out; }
void GpuGpuState::panel_upload(Panel* p) { (void)p; }
void GpuGpuState::panel_render(Panel* p) { (void)p; }
void GpuGpuState::ssao_pass() {}
void GpuGpuState::shadow_pass() {}
void GpuGpuState::present_sbs(const uint16_t* vramA, const uint16_t* vramB, int sx, int sy, int w, int h, int repaint) {
  // STOPGAP: SBS two-pane composite is Pass 3. For now present core A's frame so the window stays live.
  (void)vramB; (void)repaint;
  if (vramA) present(vramA, sx, sy, w, h);
}
// PSXPORT_GPU_SELFTEST=1: headless renderer self-test, then exit. Renders a KNOWN VRAM pattern through the
// REAL present pipeline (present.vert/frag) into an offscreen RGBA8 target, reads it back, and asserts:
//   (1) ORIENTATION — VRAM row 0 (top) lands at the TOP of the output, not the bottom. This is the
//       regression guard for the SDL_GPU swapchain Y-flip (the "rendering upside down" bug).
//   (2) 1555 UNPACK — the present.frag 1555→RGB decode is correct (red marker decodes red, blue→blue).
// No disc/game needed (boot.cpp calls this before load_exe). Prints PASS/FAIL and exits with that status.
void GpuGpuState::tritest() {
  if (!cfg_on("PSXPORT_GPU_SELFTEST")) return;
  s_headless = 1;                      // force offscreen — no window/swapchain
  if (!s_inited) init_gpu(game);
  const int TW = 320, TH = 240;        // 4:3 offscreen target (so present's letterbox is full-viewport)

  // Offscreen RGBA8 color target + its present pipeline + a download buffer.
  SDL_GPUTextureCreateInfo ti = {};
  ti.type = SDL_GPU_TEXTURETYPE_2D; ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
  ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET; ti.width = TW; ti.height = TH;
  ti.layer_count_or_depth = 1; ti.num_levels = 1;
  SDL_GPUTexture* tgt = SDL_CreateGPUTexture(s_dev, &ti); GPUCHK(tgt, "selftest target");
  SDL_GPUGraphicsPipeline* pp = make_fullscreen_pipeline(spv_g_present_vert, spv_g_present_vert_len,
      spv_g_present_frag, spv_g_present_frag_len, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM);
  SDL_GPUTransferBufferCreateInfo dni = {}; dni.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD; dni.size = TW * TH * 4;
  SDL_GPUTransferBuffer* dl = SDL_CreateGPUTransferBuffer(s_dev, &dni); GPUCHK(dl, "selftest dl");

  // Pattern: VRAM top half (rows < 256) = PSX red (1555 0x001F), bottom half = PSX blue (0x7C00).
  uint16_t* pat = gdev().s_selftest_pat;
  for (int y = 0; y < VRAM_H; y++) for (int x = 0; x < VRAM_W; x++)
    pat[y * VRAM_W + x] = (y < VRAM_H / 2) ? 0x001F : 0x7C00;

  SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(s_dev); GPUCHK(cmd, "selftest cmd");
  upload_vram(*this, cmd, pat);
  SDL_GPUColorTargetInfo cti = {}; cti.texture = tgt; cti.clear_color = (SDL_FColor){ 0, 0, 0, 1 };
  cti.load_op = SDL_GPU_LOADOP_CLEAR; cti.store_op = SDL_GPU_STOREOP_STORE;
  SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(cmd, &cti, 1, NULL);
  PresentPC pc; pc.disp[0] = 0; pc.disp[1] = 0; pc.disp[2] = VRAM_W; pc.disp[3] = VRAM_H;
  pc.fade[0] = pc.fade[1] = pc.fade[2] = pc.fade[3] = 0;
  SDL_PushGPUFragmentUniformData(cmd, 0, &pc, sizeof pc);
  SDL_GPUViewport vp = { 0, 0, (float)TW, (float)TH, 0.0f, 1.0f };
  SDL_Rect sc = { 0, 0, TW, TH };
  SDL_BindGPUGraphicsPipeline(rp, pp);
  SDL_SetGPUViewport(rp, &vp); SDL_SetGPUScissor(rp, &sc);
  SDL_GPUTextureSamplerBinding tsb = { s_vram_tex, s_samp_nearest };
  SDL_BindGPUFragmentSamplers(rp, 0, &tsb, 1);
  SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
  SDL_EndGPURenderPass(rp);
  SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
  SDL_GPUTextureRegion srcr = {}; srcr.texture = tgt; srcr.w = TW; srcr.h = TH; srcr.d = 1;
  SDL_GPUTextureTransferInfo dsti = {}; dsti.transfer_buffer = dl; dsti.pixels_per_row = TW; dsti.rows_per_layer = TH;
  SDL_DownloadFromGPUTexture(cp, &srcr, &dsti);
  SDL_EndGPUCopyPass(cp);
  SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
  SDL_WaitForGPUFences(s_dev, true, &fence, 1); SDL_ReleaseGPUFence(s_dev, fence);

  const uint8_t* px = (const uint8_t*)SDL_MapGPUTransferBuffer(s_dev, dl, false);
  const uint8_t* top = px + ((size_t)(TH / 4) * TW + TW / 2) * 4;        // y=60: should be RED (VRAM row ~64)
  const uint8_t* bot = px + ((size_t)(3 * TH / 4) * TW + TW / 2) * 4;    // y=180: should be BLUE (VRAM row ~448)
  int top_red  = (top[0] > 200 && top[1] < 60 && top[2] < 60);
  int bot_blue = (bot[2] > 200 && bot[0] < 60 && bot[1] < 60);
  int ok = top_red && bot_blue;
  fprintf(stderr, "[gpu_selftest] top(%d,%d,%d) expect RED  bottom(%d,%d,%d) expect BLUE  orientation+1555 => %s\n",
          top[0], top[1], top[2], bot[0], bot[1], bot[2], ok ? "PASS" : "FAIL");
  if (!ok && bot[0] > 200 && top[2] > 200)
    fprintf(stderr, "[gpu_selftest] (top is BLUE, bottom is RED → image is UPSIDE DOWN — the swapchain Y-flip regressed)\n");
  SDL_UnmapGPUTransferBuffer(s_dev, dl);
  exit(ok ? 0 : 1);
}

// ---- shadow capture / dynamic-state hooks (3D-only; inert in Pass 1) --------------------------------
int  gpu_gpu_shadows_active(void) { return 0; }
void gpu_gpu_shadow_push_tri(Core* core, const float* v0, const float* v1, const float* v2) { (void)core; (void)v0; (void)v1; (void)v2; }
int  gpu_seen3d_this_frame(Core* core);   // defined in gpu_native.cpp
int  gpu_had3d_last_frame(Core* core);

// ---- dual-view / SBS target selection (single target in Pass 1) -------------------------------------
void gpu_gpu_select_target(int t) { (void)t; }
int  gpu_gpu_target_count(int t) { (void)t; return 0; }
void gpu_gpu_rawdump_arm(const char* path, int frame) { (void)path; (void)frame; }

// SBS per-pane render: render ONE core's VRAM + geometry batch into s_vram_tex (NO swapchain present),
// then read the display region [sx,sy,w,h] back to host RGBA8 (`rgba` holds w*h*4). The SBS composites the
// two returned panes via gpu_gpu_present_sbs2. Reuses the proven upload+geom+readback path; the engine
// screen-fade is applied (same math as dump_to / present.frag).
void gpu_gpu_render_readback(Core* core, const uint16_t* vram, int sx, int sy, int w, int h, uint8_t* rgba) {
  ScreenFade::State f = core->screenFade.get();       // THIS core's fade (guest-backed, SBS-clean)
  const int s_fade_mode = f.mode;
  const uint8_t s_fade_r = f.r, s_fade_g = f.g, s_fade_b = f.b;
  if (!gpu_gpu_enabled()) { memset(rgba, 0, (size_t)w * h * 4); return; }
  if (!s_inited) init_gpu(core ? core->game : nullptr);
  GpuGpuState& g = core->game->gpu_gpu;            // THIS core's own VRAM image + targets (no cross-core sharing)
  SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(s_dev); GPUCHK(cmd, "render_readback cmd");
  upload_vram(g, cmd, vram);
  int a, b, c; render_geom(g, cmd, vram, &a, &b, &c);
  SDL_SubmitGPUCommandBuffer(cmd);                 // render into THIS core's VRAM image; NO swapchain present
  const uint16_t* src = readback_vram(g);          // download it (RG8 bytes == uint16 1555 words)
  if (cfg_on("PSXPORT_GPU_TRACE")) { long nz = 0; for (int yy=0; yy<h; yy++) for (int xx=0; xx<w; xx++) if (src[((sy+yy)%VRAM_H)*VRAM_W + ((sx+xx)&1023)]) nz++;
    RenderStats& st = core->mRender->stats;
    fprintf(stderr, "[gpu_gpu] readback region sx=%d sy=%d %dx%d region-nonzero=%ld/%d fade=%d(%d,%d,%d) batch tri=%d tex=%d semi=%d worldquads=%ld\n", sx, sy, w, h, nz, w*h, s_fade_mode, s_fade_r, s_fade_g, s_fade_b, a, b, c, st.dbgWorldQuads);
    st.dbgWorldQuads = 0; }
  for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
    uint16_t p = src[((sy + y) % VRAM_H) * VRAM_W + ((sx + x) & 1023)];
    int r = (p & 31) << 3, g = ((p >> 5) & 31) << 3, bl = ((p >> 10) & 31) << 3;
    if (s_fade_mode == 1)      { r += s_fade_r; g += s_fade_g; bl += s_fade_b; if (r>255)r=255; if (g>255)g=255; if (bl>255)bl=255; }
    else if (s_fade_mode == 2) { r -= s_fade_r; g -= s_fade_g; bl -= s_fade_b; if (r<0)r=0; if (g<0)g=0; if (bl<0)bl=0; }
    uint8_t* o = rgba + ((size_t)y * w + x) * 4; o[0] = (uint8_t)r; o[1] = (uint8_t)g; o[2] = (uint8_t)bl; o[3] = 255;
  }
  SDL_UnmapGPUTransferBuffer(s_dev, g.s_rb_xfer);
}

// SBS two-pane composite: draw CPU RGBA pane A (left) | pane B (right) to the swapchain in one window
// frame, each letterboxed 4:3 within its half. Uses the image pipeline (RGBA sampler). Windowed only.
static void sbs_make_tex(int i, int w, int h) {
  if (s_sbs_tex[i] && s_sbs_w[i] == w && s_sbs_h[i] == h) return;
  if (s_sbs_tex[i]) { SDL_ReleaseGPUTexture(s_dev, s_sbs_tex[i]); SDL_ReleaseGPUTransferBuffer(s_dev, s_sbs_xfer[i]); }
  SDL_GPUTextureCreateInfo ti = {}; ti.type = SDL_GPU_TEXTURETYPE_2D; ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
  ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER; ti.width = (Uint32)w; ti.height = (Uint32)h; ti.layer_count_or_depth = 1; ti.num_levels = 1;
  s_sbs_tex[i] = SDL_CreateGPUTexture(s_dev, &ti); GPUCHK(s_sbs_tex[i], "sbs tex");
  SDL_GPUTransferBufferCreateInfo up = {}; up.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD; up.size = (Uint32)w * h * 4;
  s_sbs_xfer[i] = SDL_CreateGPUTransferBuffer(s_dev, &up); GPUCHK(s_sbs_xfer[i], "sbs xfer");
  s_sbs_w[i] = w; s_sbs_h[i] = h;
}
void gpu_gpu_present_sbs2(Game* game, const uint8_t* rgbaA, int wA, int hA, const uint8_t* rgbaB, int wB, int hB) {
  if (!gpu_gpu_enabled() || s_headless) return;
  if (!s_inited) init_gpu(game);
  if (wA < 1) wA = 1; if (hA < 1) hA = 1; if (wB < 1) wB = 1; if (hB < 1) hB = 1;
  sbs_make_tex(0, wA, hA); sbs_make_tex(1, wB, hB);
  const uint8_t* rgb[2] = { rgbaA, rgbaB };
  int pw[2] = { wA, wB }, ph[2] = { hA, hB };

  SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(s_dev); GPUCHK(cmd, "sbs cmd");
  for (int i = 0; i < 2; i++) { void* p = SDL_MapGPUTransferBuffer(s_dev, s_sbs_xfer[i], true);
    memcpy(p, rgb[i], (size_t)pw[i] * ph[i] * 4); SDL_UnmapGPUTransferBuffer(s_dev, s_sbs_xfer[i]); }
  SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
  for (int i = 0; i < 2; i++) { SDL_GPUTextureTransferInfo si = {}; si.transfer_buffer = s_sbs_xfer[i]; si.pixels_per_row = (Uint32)pw[i]; si.rows_per_layer = (Uint32)ph[i];
    SDL_GPUTextureRegion dr = {}; dr.texture = s_sbs_tex[i]; dr.w = (Uint32)pw[i]; dr.h = (Uint32)ph[i]; dr.d = 1;
    SDL_UploadToGPUTexture(cp, &si, &dr, false); }
  SDL_EndGPUCopyPass(cp);

  SDL_GPUTexture* swaptex = NULL; Uint32 sw = 0, sh = 0;
  if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, s_win, &swaptex, &sw, &sh) || !swaptex) { SDL_SubmitGPUCommandBuffer(cmd); poll_quit(game); return; }
  SDL_GPUColorTargetInfo cti = {}; cti.texture = swaptex; cti.clear_color = (SDL_FColor){ 0, 0, 0, 1 };
  cti.load_op = SDL_GPU_LOADOP_CLEAR; cti.store_op = SDL_GPU_STOREOP_STORE;
  SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(cmd, &cti, 1, NULL);
  float fpc[4] = { 1.0f, 0, 0, 0 };   // no fade (already applied in the readback)
  SDL_PushGPUFragmentUniformData(cmd, 0, fpc, sizeof fpc);
  SDL_BindGPUGraphicsPipeline(rp, s_image_pipe);
  int paneW = (int)sw / 2;
  SDL_Rect sc = { 0, 0, (int)sw, (int)sh };
  for (int i = 0; i < 2; i++) {
    SDL_GPUViewport vp = letterbox(pw[i], ph[i], paneW, (int)sh);   // per-pane aspect (A may be wide, B 4:3)
    vp.x += (float)(i * paneW);
    SDL_SetGPUViewport(rp, &vp); SDL_SetGPUScissor(rp, &sc);
    SDL_GPUTextureSamplerBinding tsb = { s_sbs_tex[i], s_samp_linear };
    SDL_BindGPUFragmentSamplers(rp, 0, &tsb, 1);
    SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
  }
  SDL_EndGPURenderPass(rp);
  SDL_SubmitGPUCommandBuffer(cmd);
  poll_quit(game);
}

// ---- Public API: thin free-function wrappers over the per-instance GpuGpuState methods ---------------
void gpu_gpu_set_vd(Core* core, const float* d3) { core->game->gpu_gpu.set_vd(d3); }
void gpu_gpu_set_vd_n(Core* core, const float* d3) { core->game->gpu_gpu.set_vd_n(d3); }
void gpu_gpu_set_xyf(Core* core, const float* xf, const float* yf) { core->game->gpu_gpu.set_xyf(xf, yf); }
void gpu_gpu_set_order(Core* core, unsigned idx) { core->game->gpu_gpu.set_order(idx); }
void gpu_gpu_set_order_2d(Core* core, unsigned idx) { core->game->gpu_gpu.set_order_2d(idx); }
void gpu_gpu_set_order_2d_n(Core* core, unsigned idx) { core->game->gpu_gpu.set_order_2d_n(idx); }
void gpu_gpu_set_order_2d_bg(Core* core, unsigned idx) { core->game->gpu_gpu.set_order_2d_bg(idx); }
void gpu_gpu_set_order_2d_bg_n(Core* core, unsigned idx) { core->game->gpu_gpu.set_order_2d_bg_n(idx); }
void gpu_gpu_semi_group(Core* core, int x0, int y0, int x1, int y1) { core->game->gpu_gpu.semi_group(x0, y0, x1, y1); }
void gpu_gpu_stats(Core* core, int* tri, int* tex, int* semi) { core->game->gpu_gpu.stats(tri, tex, semi); }
void gpu_gpu_dirty(Core* core, int x, int y, int w, int h) { core->game->gpu_gpu.dirty(x, y, w, h); }
void gpu_gpu_present(Core* core, const uint16_t* src, int sx, int sy, int w, int h) {
  overlay_glue_frame_begin(core);
  core->game->gpu_gpu.present(src, sx, sy, w, h);
  // `debug fadewatch` state-byte tap (2026-07-01, "garbage during fade" investigation): whenever the fade
  // print fires, also dump the two overlapping fade drivers' guest state — bg_scene_transition_sm's struct
  // P=0x80100400 (P+4=state,P+8,P+0xA=ramp counters,P+3=dir) and ov_sop_field_mode's sm+0x50/0x6c (outer
  // field-mode state / its own ramp counter) — to see which driver (if either) is live during the glitch
  // frame where the fade unexpectedly reads back to mode=0 mid-ramp.
  if (cfg_dbg("fadewatch")) {
    GpuDevice& gd = gdev();
    int& lm = gd.s_fws_lastmode; uint8_t& lr = gd.s_fws_lr; uint8_t& lg = gd.s_fws_lg; uint8_t& lb = gd.s_fws_lb;
    int& lsx = gd.s_fws_lsx; int& lsy = gd.s_fws_lsy; int& lw = gd.s_fws_lw; int& lh = gd.s_fws_lh;
    ScreenFade::State f = core->screenFade.get();
    int m = f.mode; uint8_t r = f.r, g = f.g, b = f.b;
    if (m != lm || r != lr || g != lg || b != lb || sx != lsx || sy != lsy || w != lw || h != lh) {
      uint32_t sm = core->mem_r32(0x1f800138u);
      fprintf(stderr, "[fadewatch-state] P.st=%u P8=%u P0xA=%u P3=%u | sm50=%u sm6c=%u\n",
              core->mem_r8(0x80100400u + 4), core->mem_r16(0x80100400u + 8), core->mem_r16(0x80100400u + 0xa),
              core->mem_r8(0x80100400u + 3), core->mem_r16(sm + 0x50), core->mem_r8(sm + 0x6c));
      lm=m; lr=r; lg=g; lb=b; lsx=sx; lsy=sy; lw=w; lh=h;
    }
  }
}
void gpu_gpu_present_sbs(Core* coreA, const uint16_t* vramA, const uint16_t* vramB, int sx, int sy, int w, int h) {
  coreA->game->gpu_gpu.present_sbs(vramA, vramB, sx, sy, w, h, 0);
}
void gpu_gpu_present_sbs_repaint(Core* coreA, int sx, int sy, int w, int h) {
  coreA->game->gpu_gpu.present_sbs(nullptr, nullptr, sx, sy, w, h, 1);
}
void gpu_gpu_draw_tri(Core* core, int x0,int y0,int r0,int g0,int b0, int x1,int y1,int r1,int g1,int b1, int x2,int y2,int r2,int g2,int b2) { core->game->gpu_gpu.draw_tri(x0,y0,r0,g0,b0,x1,y1,r1,g1,b1,x2,y2,r2,g2,b2); }
void gpu_gpu_draw_tritri(Core* core, const int* xs, const int* ys, const int* us, const int* vs, const unsigned char* rs, const unsigned char* gs, const unsigned char* bs, int tpx, int tpy, int mode, int raw, int clutx, int cluty, int twmx, int twmy, int twox, int twoy, int dax0, int day0, int dax1, int day1) { core->game->gpu_gpu.draw_tritri(xs,ys,us,vs,rs,gs,bs,tpx,tpy,mode,raw,clutx,cluty,twmx,twmy,twox,twoy,dax0,day0,dax1,day1); }
void gpu_gpu_draw_semi(Core* core, const int* xs, const int* ys, const int* us, const int* vs, const unsigned char* rs, const unsigned char* gs, const unsigned char* bs, int tpx, int tpy, int mode, int raw, int clutx, int cluty, int twmx, int twmy, int twox, int twoy, int dax0, int day0, int dax1, int day1, int blend) { core->game->gpu_gpu.draw_semi(xs,ys,us,vs,rs,gs,bs,tpx,tpy,mode,raw,clutx,cluty,twmx,twmy,twox,twoy,dax0,day0,dax1,day1,blend); }
void gpu_gpu_shot(Core* core, const char* path) { core->game->gpu_gpu.shot(path); }
void gpu_gpu_shot_b(Core* core, const char* path) { core->game->gpu_gpu.shot_b(path); }
void gpu_gpu_frame_end(Core* core, const uint16_t* svram, int frame) { core->game->gpu_gpu.frame_end(svram, frame); }

// REPL `preseq` arm (repl.cpp) — creates the dir and arms the per-present dump in present().
void gpu_gpu_preseq_arm(Core* core, int n, const char* dir) {
  GpuGpuState& s = core->game->gpu_gpu;
  snprintf(s.s_preseq_dir, sizeof s.s_preseq_dir, "%s", dir);
  char mk[200]; snprintf(mk, sizeof mk, "mkdir -p %s", dir); if (system(mk)) {}
  s.s_preseq_idx = 0; s.s_preseq_left = n;
}
// Present index (0-based) that THIS emit pass will dump to `p<idx>.ppm` at frame_end, or -1 when no
// preseq capture is armed. The emit passes (RenderQueue::emitItem) consult this for the `preseqobj`
// per-object motion log so each logged line is keyed to the exact present frame it belongs to.
int gpu_gpu_preseq_present_index(Core* core) {
  GpuGpuState& s = core->game->gpu_gpu;
  return s.s_preseq_left > 0 ? s.s_preseq_idx : -1;
}
