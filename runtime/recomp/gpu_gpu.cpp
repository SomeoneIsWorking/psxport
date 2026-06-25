#include "core.h"
#include "game.h"    // Game / GpuGpuState (per-instance render state)
#include "gpu_gpu.h"  // public Core*-threaded API decls (wrappers below forward to core->game->gpu_gpu)
// gpu_gpu.cpp — SDL3 GPU API present backend for the Tomba2Engine port.
//
// This is the PC-native renderer re-expressed on the SDL3 GPU API (SDL_gpu.h), replacing gpu_gpu.cpp
// (Vulkan + SDL2). ONE stack — SDL3 owns the window, input, audio AND the GPU device — so the Mac runs
// native Metal (the original MoltenVK SBS-black bug is gone) and Linux/Windows run Vulkan/D3D12.
//
// PASS 1 (this file's current scope): the 2D VRAM present path + the fullscreen IMAGE present + the
// headless VRAM readback (`shot`). That alone makes the SCEA splash, FMV, title and 2D menus visible —
// the user's immediate gap. The native 3D raster (draw_tri/tritri/semi → depth-ordered offscreen target)
// is PASS 2 (see docs/render-backend-port.md): SDL_GPU has NO format-aliasing, so the Vulkan
// R16_UINT↔A1R5G5B5 blend-alias trick can't port — Pass 2 renders into an R16_UINT color target with the
// fragment outputting the packed 1555 word and does the 4 PSX semi modes as an in-shader blend against a
// VRAM snapshot (the original pre-HW-blend approach). Until then draw_* are STOPGAP no-ops (3D world is
// dark; 2D screens render). This is the phasing the handoff prescribes.
//
// State model mirrors gpu_gpu.cpp: the SDL_GPU device/window/pipelines are file-static (one per process);
// the wrappers ignore Core* exactly as before. The per-frame batch state lives on GpuGpuState (game.h).
#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include "cfg.h"
#include "mods.h"             // g_mods: live PC-native mod toggles (wide/ires); seeded from cfg
#include "overlay_glue.h"     // RmlUi mod/debug overlay hooks (init / event / per-frame / record)
#include "gpu_gpu_shaders.h"  // generated: spv_g_present_{vert,frag} / spv_g_image_{vert,frag}
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define VRAM_W 1024
#define VRAM_H 512

// ---- device / window singletons (one per process; not per-instance machine state) -------------------
static int               s_gpu_on = -1;
static int               s_inited = 0;
static int               s_headless = 0;
static SDL_Window*       s_win;
static SDL_GPUDevice*    s_dev;
static SDL_GPUTextureFormat s_swap_fmt;

// VRAM image: RG8 1024x512 (PSX 1555 LE bytes) = PSX VRAM (1555). Uploaded from CPU s_vram each present, sampled by the
// present pass (integer usampler2D → 1555 unpack), downloaded for `shot`.
static SDL_GPUTexture*        s_vram_tex;
static SDL_GPUTransferBuffer* s_vram_xfer;   // host→VRAM upload (1024*512*2)
static SDL_GPUTransferBuffer* s_rb_xfer;     // VRAM→host download (readback / shot)
static SDL_GPUSampler*        s_samp_nearest; // integer VRAM sampler (nearest)
static SDL_GPUSampler*        s_samp_linear;  // RGBA image sampler (linear)
static SDL_GPUGraphicsPipeline* s_present_pipe;
static SDL_GPUGraphicsPipeline* s_image_pipe;

// Fullscreen IMAGE present (gpu_gpu_present_image): RGBA8 texture, recreated on size change.
static SDL_GPUTexture*        s_img_tex;
static SDL_GPUTransferBuffer* s_img_xfer;
static int                    s_img_w, s_img_h;

// Engine-owned screen FADE (set per logic frame by the engine; applied in present.frag + the readback).
static int     s_fade_mode = 0;
static uint8_t s_fade_r = 0, s_fade_g = 0, s_fade_b = 0;
void gpu_set_fade(int mode, uint8_t r, uint8_t g, uint8_t b) { s_fade_mode = mode; s_fade_r = r; s_fade_g = g; s_fade_b = b; }
void gpu_clear_fade(void) { s_fade_mode = 0; s_fade_r = s_fade_g = s_fade_b = 0; }

// Present-pass fragment uniform: matches present.frag's `PC { ivec4 disp; ivec4 fade; }`.
struct PresentPC { int32_t disp[4]; int32_t fade[4]; };

// ---- Pass 2: native 3D / textured raster (depth-ordered, in-shader semi blend) ----------------------
// The engine draws the world AND the 2D menus/HUD/sprites as textured/flat prims through the tee
// (gpu_gpu_draw_tri/tritri/semi). Each present: upload CPU VRAM into the R16_UINT COLOR-TARGET image AND a
// SAMPLER snapshot, then render the accumulated geometry batch ON TOP into the color target with a D32
// depth buffer (the 3 ordering bands), then present samples that image. The 4 PSX semi modes are blended
// IN-SHADER against the snapshot (SDL_GPU has no HW blend on integer targets / no format alias).
// Single batch (SBS dual-pane is Pass 3). See shaders_gpu/{tri,tritex}.{vert,frag}.
#define NATIVE_3D_MIN 0.0625f
#define NATIVE_3D_MAX 0.9375f
static inline float ord3d(float d) { return NATIVE_3D_MIN + d * (NATIVE_3D_MAX - NATIVE_3D_MIN); }
#define TRI_CAP 196608   // max batched vertices (= 65536 tris)
#define TEX_CAP 196608
struct TriVtx { float x, y, r, g, b, ord; };                                                    // 24 bytes
struct TexVtx { float x, y, u, v, r, g, b; int32_t tp[4], clut[4], tw[4], da[4]; float ord; };   // 96 bytes
static TriVtx* s_tri_buf;  static int s_tri_n;
static TexVtx* s_tex_buf;  static int s_tex_n;
static TexVtx* s_semi_buf; static int s_semi_n;
static int s_dbg_tri_c, s_dbg_tex_c, s_dbg_semi_c;   // last-frame counts (vkstats probe)
static SDL_GPUTexture*        s_vram_snap;   // texture-source snapshot (RG8 SAMPLER): CLUT + semi blend dst
static SDL_GPUTransferBuffer* s_snap_xfer;
static SDL_GPUTexture*        s_depth;       // D32 depth (ordering)
static SDL_GPUBuffer* s_tri_vbuf;  static SDL_GPUBuffer* s_tex_vbuf;  static SDL_GPUBuffer* s_semi_vbuf;
static SDL_GPUTransferBuffer* s_tri_xfer; static SDL_GPUTransferBuffer* s_tex_xfer; static SDL_GPUTransferBuffer* s_semi_xfer;
static SDL_GPUGraphicsPipeline* s_tri_pipe;     // flat opaque (depth test + write)
static SDL_GPUGraphicsPipeline* s_tritex_pipe;  // textured opaque (depth test + write)
static SDL_GPUGraphicsPipeline* s_semi_pipe;    // textured semi (depth test, NO write)
static int s_have_3d;                           // 3D resources created (windowed-only, needs depth/pipes)
static void create_3d(void);

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
static int wide_native_w(void) {
  switch (g_mods.aspect) {
    case ASPECT_16_9: return 428;
    case ASPECT_21_9: return 560;
    case ASPECT_AUTO: { int w = (int)((240.0 * win_w()) / win_h() + 0.5); w &= ~1;
                        if (w < 320) w = 320; if (w > VRAM_W) w = VRAM_W; return w; }
    default:          return 320;
  }
}
int gpu_gpu_wide_engine(void)     { mods_init(); return g_mods.aspect != ASPECT_4_3; }
int gpu_gpu_wide_engine_ofx(void) { return wide_native_w() / 2; }
int gpu_gpu_wide_engine_w(void)   { return wide_native_w(); }
void gpu_gpu_video_status(int* native_w, int* ires, int* fbw, int* fbh, int* ww, int* wh, int* ires_cap) {
  mods_init();
  int nw = wide_native_w(), cap = VRAM_W / nw; if (cap < 1) cap = 1; if (cap > 3) cap = 3;
  int i = g_mods.ires < 1 ? 1 : (g_mods.ires > cap ? cap : g_mods.ires);
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

// Build the 3D raster resources: the VRAM snapshot (texture source), the D32 depth buffer, the three host
// vertex buffers (+ their upload staging) and the flat/textured-opaque/textured-semi pipelines. Once.
static void create_3d(void) {
  if (s_have_3d) return;
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
  mkbuf(sizeof(TexVtx) * TEX_CAP, &s_semi_vbuf, &s_semi_xfer);
  s_tri_buf  = (TriVtx*)malloc(sizeof(TriVtx) * TRI_CAP);
  s_tex_buf  = (TexVtx*)malloc(sizeof(TexVtx) * TEX_CAP);
  s_semi_buf = (TexVtx*)malloc(sizeof(TexVtx) * TEX_CAP);
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
  s_semi_pipe   = make_geom_pipeline(spv_g_tritex_vert, spv_g_tritex_vert_len, spv_g_tritex_frag, spv_g_tritex_frag_len,
                                     sizeof(TexVtx), tex_attr, 8, 1, false);
  s_have_3d = 1;
  fprintf(stderr, "[gpu_gpu] 3D raster up (RG8 color target + D32 depth, in-shader semi blend)\n");
}

static void init_gpu(void) {
  s_inited = 1;
  mods_init();
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

  // VRAM R16_UINT image: SAMPLER (present samples it) + COLOR_TARGET (the 3D raster renders the geometry
  // batch into it, on top of the uploaded 2D backdrop). + upload/download transfer buffers.
  // R8G8_UNORM (NOT R16_UINT): SDL_GPU forbids SAMPLER usage on integer formats, so VRAM is stored as two
  // 8-bit channels (R=low byte, G=high byte) — the same uint16 LE bytes — and the shaders reconstruct the
  // 16-bit 1555 word. RG8 is sampler-legal + renderable on every backend (incl. Metal).
  SDL_GPUTextureCreateInfo ti = {};
  ti.type = SDL_GPU_TEXTURETYPE_2D; ti.format = SDL_GPU_TEXTUREFORMAT_R8G8_UNORM;
  ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
  ti.width = VRAM_W; ti.height = VRAM_H; ti.layer_count_or_depth = 1; ti.num_levels = 1;
  s_vram_tex = SDL_CreateGPUTexture(s_dev, &ti); GPUCHK(s_vram_tex, "CreateGPUTexture(VRAM)");
  SDL_GPUTransferBufferCreateInfo up = {}; up.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD; up.size = VRAM_W * VRAM_H * 2;
  s_vram_xfer = SDL_CreateGPUTransferBuffer(s_dev, &up); GPUCHK(s_vram_xfer, "CreateGPUTransferBuffer(up)");
  SDL_GPUTransferBufferCreateInfo dn = {}; dn.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD; dn.size = VRAM_W * VRAM_H * 2;
  s_rb_xfer = SDL_CreateGPUTransferBuffer(s_dev, &dn); GPUCHK(s_rb_xfer, "CreateGPUTransferBuffer(dn)");

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
  create_3d();   // the native 3D/textured raster (R16_UINT color target + depth) — windowed AND headless
  fprintf(stderr, "[gpu_gpu] %s renderer up (VRAM %dx%d RG8 = PSX 1555)\n", s_headless ? "headless" : "windowed", VRAM_W, VRAM_H);
  // RmlUi mod/debug overlay — windowed only (it records into the swapchain present pass). No-op if its
  // assets/fonts are missing; ESC toggles it once up.
  if (!s_headless) overlay_glue_init(s_win, s_dev, s_swap_fmt);
}

static void poll_quit(void) {
  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    overlay_glue_event(&e);   // RmlUi overlay: ESC toggle + mouse/keyboard nav (no-op if not inited)
    if (e.type == SDL_EVENT_QUIT) exit(0);
  }
}

// Upload the whole CPU VRAM (src, 1024*512 uint16) into s_vram_tex via a copy pass on `cmd`.
static void upload_vram(SDL_GPUCommandBuffer* cmd, const uint16_t* src) {
  void* p = SDL_MapGPUTransferBuffer(s_dev, s_vram_xfer, true);
  memcpy(p, src, (size_t)VRAM_W * VRAM_H * 2);
  SDL_UnmapGPUTransferBuffer(s_dev, s_vram_xfer);
  SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
  SDL_GPUTextureTransferInfo srci = {}; srci.transfer_buffer = s_vram_xfer; srci.pixels_per_row = VRAM_W; srci.rows_per_layer = VRAM_H;
  SDL_GPUTextureRegion dst = {}; dst.texture = s_vram_tex; dst.w = VRAM_W; dst.h = VRAM_H; dst.d = 1;
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

// Upload this frame's VRAM snapshot (texture source / semi blend dst) + the geometry batches, then render
// the batch ON TOP of s_vram_tex (over the uploaded 2D backdrop) with a fresh-cleared D32 depth: flat ->
// textured-opaque -> textured-semi. No-op if nothing was batched. Reports the drawn counts for vkstats.
static void render_geom(SDL_GPUCommandBuffer* cmd, const uint16_t* src, int* dtri, int* dtex, int* dsemi) {
  *dtri = s_tri_n; *dtex = s_tex_n; *dsemi = s_semi_n;
  if (!s_have_3d || (s_tri_n + s_tex_n + s_semi_n) == 0) return;
  { void* p = SDL_MapGPUTransferBuffer(s_dev, s_snap_xfer, true); memcpy(p, src, (size_t)VRAM_W*VRAM_H*2); SDL_UnmapGPUTransferBuffer(s_dev, s_snap_xfer); }
  if (s_tri_n)  { void* p = SDL_MapGPUTransferBuffer(s_dev, s_tri_xfer, true);  memcpy(p, s_tri_buf,  (size_t)s_tri_n*sizeof(TriVtx));  SDL_UnmapGPUTransferBuffer(s_dev, s_tri_xfer); }
  if (s_tex_n)  { void* p = SDL_MapGPUTransferBuffer(s_dev, s_tex_xfer, true);  memcpy(p, s_tex_buf,  (size_t)s_tex_n*sizeof(TexVtx));  SDL_UnmapGPUTransferBuffer(s_dev, s_tex_xfer); }
  if (s_semi_n) { void* p = SDL_MapGPUTransferBuffer(s_dev, s_semi_xfer, true); memcpy(p, s_semi_buf, (size_t)s_semi_n*sizeof(TexVtx)); SDL_UnmapGPUTransferBuffer(s_dev, s_semi_xfer); }
  SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
  { SDL_GPUTextureTransferInfo si = {}; si.transfer_buffer = s_snap_xfer; si.pixels_per_row = VRAM_W; si.rows_per_layer = VRAM_H;
    SDL_GPUTextureRegion dr = {}; dr.texture = s_vram_snap; dr.w = VRAM_W; dr.h = VRAM_H; dr.d = 1;
    SDL_UploadToGPUTexture(cp, &si, &dr, false); }
  auto upv = [&](SDL_GPUTransferBuffer* x, SDL_GPUBuffer* b, int n, Uint32 stride){ if (!n) return;
    SDL_GPUTransferBufferLocation s = {}; s.transfer_buffer = x;
    SDL_GPUBufferRegion d = {}; d.buffer = b; d.offset = 0; d.size = (Uint32)n*stride;
    SDL_UploadToGPUBuffer(cp, &s, &d, false); };
  upv(s_tri_xfer, s_tri_vbuf, s_tri_n, sizeof(TriVtx));
  upv(s_tex_xfer, s_tex_vbuf, s_tex_n, sizeof(TexVtx));
  upv(s_semi_xfer, s_semi_vbuf, s_semi_n, sizeof(TexVtx));
  SDL_EndGPUCopyPass(cp);
  SDL_GPUColorTargetInfo ct = {}; ct.texture = s_vram_tex; ct.load_op = SDL_GPU_LOADOP_LOAD; ct.store_op = SDL_GPU_STOREOP_STORE;
  SDL_GPUDepthStencilTargetInfo dt = {}; dt.texture = s_depth; dt.clear_depth = 0.0f;
  dt.load_op = SDL_GPU_LOADOP_CLEAR; dt.store_op = SDL_GPU_STOREOP_DONT_CARE;
  dt.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE; dt.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
  SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(cmd, &ct, 1, &dt);
  SDL_GPUViewport vp = { 0, 0, (float)VRAM_W, (float)VRAM_H, 0.0f, 1.0f }; SDL_Rect sc = { 0, 0, VRAM_W, VRAM_H };
  SDL_SetGPUViewport(rp, &vp); SDL_SetGPUScissor(rp, &sc);
  SDL_GPUTextureSamplerBinding snap = { s_vram_snap, s_samp_nearest };
  SDL_GPUBufferBinding bb = {}; bb.offset = 0;
  if (s_tri_n)  { SDL_BindGPUGraphicsPipeline(rp, s_tri_pipe); bb.buffer = s_tri_vbuf; SDL_BindGPUVertexBuffers(rp, 0, &bb, 1); SDL_DrawGPUPrimitives(rp, s_tri_n, 1, 0, 0); }
  if (s_tex_n)  { SDL_BindGPUGraphicsPipeline(rp, s_tritex_pipe); bb.buffer = s_tex_vbuf; SDL_BindGPUVertexBuffers(rp, 0, &bb, 1); SDL_BindGPUFragmentSamplers(rp, 0, &snap, 1); SDL_DrawGPUPrimitives(rp, s_tex_n, 1, 0, 0); }
  if (s_semi_n) { SDL_BindGPUGraphicsPipeline(rp, s_semi_pipe); bb.buffer = s_semi_vbuf; SDL_BindGPUVertexBuffers(rp, 0, &bb, 1); SDL_BindGPUFragmentSamplers(rp, 0, &snap, 1); SDL_DrawGPUPrimitives(rp, s_semi_n, 1, 0, 0); }
  SDL_EndGPURenderPass(rp);
}

// ---- present: upload CPU VRAM, render the 3D/textured batch on top, sample [sx,sy,w,h] to the swapchain
void GpuGpuState::present(const uint16_t* src, int sx, int sy, int w, int h) {
  if (!gpu_gpu_enabled()) return;
  if (!s_inited) init_gpu();
  mods_init();
  s_present_sx = sx; s_present_sy = sy;
  s_last_sx = sx; s_last_sy = sy; s_last_w = w; s_last_h = h;

  // PSXPORT_GPU_TRACE: per-present source-VRAM occupancy + sampled display region (diagnostic).
  if (cfg_on("PSXPORT_GPU_TRACE")) { static int n = 0; if (n++ < 4 || (n % 200) == 0) {
    long nz = 0; for (long i = 0; i < (long)VRAM_W * VRAM_H; i++) if (src[i]) nz++;
    fprintf(stderr, "[gpu_gpu] present #%d src nonzero=%ld/%d disp=%d,%d %dx%d | batch tri=%d tex=%d semi=%d\n",
            n, nz, VRAM_W*VRAM_H, sx, sy, w, h, s_tri_n, s_tex_n, s_semi_n); } }
  SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(s_dev);
  GPUCHK(cmd, "AcquireGPUCommandBuffer");
  upload_vram(cmd, src);                                    // CPU VRAM -> s_vram_tex (2D backdrop)
  render_geom(cmd, src, &s_dbg_tri_c, &s_dbg_tex_c, &s_dbg_semi_c);   // draw the tee batch on top (+depth)

  if (s_headless) { SDL_SubmitGPUCommandBuffer(cmd); return; }   // shot reads s_vram_tex via its own cmd

  SDL_GPUTexture* swaptex = NULL; Uint32 sw = 0, sh = 0;
  if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, s_win, &swaptex, &sw, &sh) || !swaptex) {
    SDL_SubmitGPUCommandBuffer(cmd); poll_quit(); return;   // minimized / no swapchain image this frame
  }
  SDL_GPUColorTargetInfo cti = {};
  cti.texture = swaptex; cti.clear_color = (SDL_FColor){ 0, 0, 0, 1 };
  cti.load_op = SDL_GPU_LOADOP_CLEAR; cti.store_op = SDL_GPU_STOREOP_STORE;
  SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(cmd, &cti, 1, NULL);

  PresentPC pc; pc.disp[0] = sx; pc.disp[1] = sy; pc.disp[2] = w; pc.disp[3] = h;
  pc.fade[0] = s_fade_mode; pc.fade[1] = s_fade_r; pc.fade[2] = s_fade_g; pc.fade[3] = s_fade_b;
  SDL_PushGPUFragmentUniformData(cmd, 0, &pc, sizeof pc);

  SDL_GPUViewport vp = letterbox(4, 3, (int)sw, (int)sh);
  SDL_Rect sc = { 0, 0, (int)sw, (int)sh };
  SDL_BindGPUGraphicsPipeline(rp, s_present_pipe);
  SDL_SetGPUViewport(rp, &vp); SDL_SetGPUScissor(rp, &sc);
  SDL_GPUTextureSamplerBinding tsb = { s_vram_tex, s_samp_nearest };
  SDL_BindGPUFragmentSamplers(rp, 0, &tsb, 1);
  SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
  // RmlUi mod/debug overlay (ESC) composites ON TOP of the game frame, into the same present pass over
  // the FULL window. No-op when the menu is hidden / overlay not inited.
  overlay_glue_record(cmd, rp, (int)sw, (int)sh);
  SDL_EndGPURenderPass(rp);
  SDL_SubmitGPUCommandBuffer(cmd);
  poll_quit();
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
  (void)core;
  if (!gpu_gpu_enabled() || iw <= 0 || ih <= 0) return;
  if (!s_inited) init_gpu();
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
    SDL_SubmitGPUCommandBuffer(cmd); poll_quit(); return;
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
  SDL_EndGPURenderPass(rp);
  SDL_SubmitGPUCommandBuffer(cmd);
  poll_quit();
}

// ---- readback (shot / vram dump): download s_vram_tex → host, decode 1555 → PPM with the fade ---------
static const uint16_t* readback_vram(void) {
  SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(s_dev); GPUCHK(cmd, "AcquireGPUCommandBuffer");
  SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
  SDL_GPUTextureRegion srcr = {}; srcr.texture = s_vram_tex; srcr.w = VRAM_W; srcr.h = VRAM_H; srcr.d = 1;
  SDL_GPUTextureTransferInfo dsti = {}; dsti.transfer_buffer = s_rb_xfer; dsti.pixels_per_row = VRAM_W; dsti.rows_per_layer = VRAM_H;
  SDL_DownloadFromGPUTexture(cp, &srcr, &dsti);
  SDL_EndGPUCopyPass(cp);
  SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
  SDL_WaitForGPUFences(s_dev, true, &fence, 1);
  SDL_ReleaseGPUFence(s_dev, fence);
  const uint16_t* p = (const uint16_t*)SDL_MapGPUTransferBuffer(s_dev, s_rb_xfer, false);
  if (cfg_on("PSXPORT_GPU_TRACE")) { long nz = 0; for (long i = 0; i < (long)VRAM_W * VRAM_H; i++) if (p[i]) nz++;
    fprintf(stderr, "[gpu_gpu] readback nonzero=%ld/%d\n", nz, VRAM_W * VRAM_H); }
  return p;
}
static void dump_to(const char* path, int sx, int sy, int w, int h) {
  const uint16_t* vram = readback_vram();
  FILE* f = fopen(path, "wb"); if (!f) { SDL_UnmapGPUTransferBuffer(s_dev, s_rb_xfer); return; }
  fprintf(f, "P6\n%d %d\n255\n", w, h);
  for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
    uint16_t p = vram[((sy + y) % VRAM_H) * VRAM_W + ((sx + x) & 1023)];
    int r = (p & 31) << 3, g = ((p >> 5) & 31) << 3, b = ((p >> 10) & 31) << 3;
    if (s_fade_mode == 1)      { r += s_fade_r; g += s_fade_g; b += s_fade_b; if (r>255)r=255; if (g>255)g=255; if (b>255)b=255; }
    else if (s_fade_mode == 2) { r -= s_fade_r; g -= s_fade_g; b -= s_fade_b; if (r<0)r=0; if (g<0)g=0; if (b<0)b=0; }
    unsigned char c[3] = { (unsigned char)r, (unsigned char)g, (unsigned char)b };
    fwrite(c, 1, 3, f);
  }
  fclose(f);
  SDL_UnmapGPUTransferBuffer(s_dev, s_rb_xfer);
}
void GpuGpuState::shot(const char* path) {
  if (!gpu_gpu_enabled() || !s_inited) { fprintf(stderr, "[gpu_shot] GPU not active\n"); return; }
  dump_to(path, s_last_sx, s_last_sy, s_last_w, s_last_h);
  fprintf(stderr, "[gpu_shot] wrote %s (%dx%d @ %d,%d)\n", path, s_last_w, s_last_h, s_last_sx, s_last_sy);
}
void GpuGpuState::shot_b(const char* path) { shot(path); }   // Pass 1: single target
void gpu_gpu_shot_region(Core* core, const char* path, int sx, int sy, int w, int h) {
  (void)core; if (!gpu_gpu_enabled() || !s_inited) return;
  dump_to(path, sx, sy, w, h);
  fprintf(stderr, "[gpu_shot] wrote %s (%dx%d @ %d,%d)\n", path, w, h, sx, sy);
}
void gpu_gpu_vram_region(const char* path, int x, int y, int w, int h) {
  if (!gpu_gpu_enabled() || !s_inited) return;
  dump_to(path, x, y, w, h);
  fprintf(stderr, "[gpu_vram] wrote %s (%dx%d @ %d,%d)\n", path, w, h, x, y);
}
void gpu_gpu_vram_raw(const char* path) {
  if (!gpu_gpu_enabled() || !s_inited) return;
  const uint16_t* vram = readback_vram();
  FILE* f = fopen(path, "wb"); if (!f) { SDL_UnmapGPUTransferBuffer(s_dev, s_rb_xfer); return; }
  for (int y = 0; y < VRAM_H; y++) fwrite(&vram[y * VRAM_W], 2, VRAM_W, f);
  fclose(f);
  SDL_UnmapGPUTransferBuffer(s_dev, s_rb_xfer);
  fprintf(stderr, "[gpu_vram] wrote RAW %s (%dx%d u16)\n", path, VRAM_W, VRAM_H);
}

// ---- per-prim state setters (depth/order; consumed by the 3D raster) --------------------------------
void GpuGpuState::set_vd(const float* d3) { s_vd = d3; }
void GpuGpuState::set_vd_n(const float* d3) { s_vdn = d3; }
void GpuGpuState::set_xyf(const float* xf, const float* yf) { s_xf = xf; s_yf = yf; }
void GpuGpuState::set_order(unsigned idx) { s_cur_ord = (float)(idx + 1) / 65536.0f; if (s_cur_ord > 1.0f) s_cur_ord = 1.0f;
                                           s_cur_ordn = s_cur_ord; s_vd = 0; s_vdn = 0; s_xf = 0; s_yf = 0; }
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
void GpuGpuState::frame_end(const uint16_t* svram, int frame) { (void)svram; (void)frame; s_tri_n = s_tex_n = s_semi_n = 0; }

// ---- native 3D / textured raster: accumulate the tee'd geometry into the host batch (Pass 2) ---------
// Append one flat triangle (VRAM coords + per-vertex RGB 0..255); depth = per-vertex native (s_vd) or the
// per-prim OT-order (s_cur_ord) band.
void GpuGpuState::draw_tri(int x0,int y0,int r0,int g0,int b0, int x1,int y1,int r1,int g1,int b1,
                          int x2,int y2,int r2,int g2,int b2) {
  if (!s_have_3d || s_tri_n + 3 > TRI_CAP) return;
  TriVtx* v = s_tri_buf + s_tri_n;
  v[0] = { (float)x0, (float)y0, r0/255.f, g0/255.f, b0/255.f, s_vd ? ord3d(s_vd[0]) : s_cur_ord };
  v[1] = { (float)x1, (float)y1, r1/255.f, g1/255.f, b1/255.f, s_vd ? ord3d(s_vd[1]) : s_cur_ord };
  v[2] = { (float)x2, (float)y2, r2/255.f, g2/255.f, b2/255.f, s_vd ? ord3d(s_vd[2]) : s_cur_ord };
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
    t[i].ord = s_vd ? ord3d(s_vd[i]) : s_cur_ord;
  }
}
void GpuGpuState::draw_tritri(const int* xs, const int* ys, const int* us, const int* vs,
                             const unsigned char* rs, const unsigned char* gs, const unsigned char* bs,
                             int tpx, int tpy, int mode, int raw, int clutx, int cluty,
                             int twmx, int twmy, int twox, int twoy, int dax0, int day0, int dax1, int day1) {
  if (!s_have_3d || s_tex_n + 3 > TEX_CAP) return;
  tex_emit(s_tex_buf + s_tex_n, xs, ys, us, vs, rs, gs, bs, tpx, tpy, mode, raw, clutx, cluty,
           twmx, twmy, twox, twoy, dax0, day0, dax1, day1, 0, 0);
  s_tex_n += 3;
}
void GpuGpuState::draw_semi(const int* xs, const int* ys, const int* us, const int* vs,
                           const unsigned char* rs, const unsigned char* gs, const unsigned char* bs,
                           int tpx, int tpy, int mode, int raw, int clutx, int cluty,
                           int twmx, int twmy, int twox, int twoy, int dax0, int day0, int dax1, int day1, int blend) {
  if (!s_have_3d || s_semi_n + 3 > TEX_CAP) return;
  tex_emit(s_semi_buf + s_semi_n, xs, ys, us, vs, rs, gs, bs, tpx, tpy, mode, raw, clutx, cluty,
           twmx, twmy, twox, twoy, dax0, day0, dax1, day1, 1, blend);
  s_semi_n += 3;
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
  if (!s_inited) init_gpu();
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
  static uint16_t pat[VRAM_W * VRAM_H];
  for (int y = 0; y < VRAM_H; y++) for (int x = 0; x < VRAM_W; x++)
    pat[y * VRAM_W + x] = (y < VRAM_H / 2) ? 0x001F : 0x7C00;

  SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(s_dev); GPUCHK(cmd, "selftest cmd");
  upload_vram(cmd, pat);
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
  (void)core;
  if (!gpu_gpu_enabled()) { memset(rgba, 0, (size_t)w * h * 4); return; }
  if (!s_inited) init_gpu();
  SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(s_dev); GPUCHK(cmd, "render_readback cmd");
  upload_vram(cmd, vram);
  int a, b, c; render_geom(cmd, vram, &a, &b, &c);
  SDL_SubmitGPUCommandBuffer(cmd);                 // render into s_vram_tex; NO swapchain present
  const uint16_t* src = readback_vram();           // download s_vram_tex (RG8 bytes == uint16 1555 words)
  if (cfg_on("PSXPORT_GPU_TRACE")) { long nz = 0; for (int yy=0; yy<h; yy++) for (int xx=0; xx<w; xx++) if (src[((sy+yy)%VRAM_H)*VRAM_W + ((sx+xx)&1023)]) nz++;
    extern long g_dbg_world_quads;
    fprintf(stderr, "[gpu_gpu] readback region sx=%d sy=%d %dx%d region-nonzero=%ld/%d fade=%d(%d,%d,%d) batch tri=%d tex=%d semi=%d worldquads=%ld\n", sx, sy, w, h, nz, w*h, s_fade_mode, s_fade_r, s_fade_g, s_fade_b, a, b, c, g_dbg_world_quads);
    g_dbg_world_quads = 0; }
  for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
    uint16_t p = src[((sy + y) % VRAM_H) * VRAM_W + ((sx + x) & 1023)];
    int r = (p & 31) << 3, g = ((p >> 5) & 31) << 3, bl = ((p >> 10) & 31) << 3;
    if (s_fade_mode == 1)      { r += s_fade_r; g += s_fade_g; bl += s_fade_b; if (r>255)r=255; if (g>255)g=255; if (bl>255)bl=255; }
    else if (s_fade_mode == 2) { r -= s_fade_r; g -= s_fade_g; bl -= s_fade_b; if (r<0)r=0; if (g<0)g=0; if (bl<0)bl=0; }
    uint8_t* o = rgba + ((size_t)y * w + x) * 4; o[0] = (uint8_t)r; o[1] = (uint8_t)g; o[2] = (uint8_t)bl; o[3] = 255;
  }
  SDL_UnmapGPUTransferBuffer(s_dev, s_rb_xfer);
}

// SBS two-pane composite: draw CPU RGBA pane A (left) | pane B (right) to the swapchain in one window
// frame, each letterboxed 4:3 within its half. Uses the image pipeline (RGBA sampler). Windowed only.
static SDL_GPUTexture*        s_sbs_tex[2];
static SDL_GPUTransferBuffer* s_sbs_xfer[2];
static int                    s_sbs_w[2], s_sbs_h[2];
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
void gpu_gpu_present_sbs2(const uint8_t* rgbaA, int wA, int hA, const uint8_t* rgbaB, int wB, int hB) {
  if (!gpu_gpu_enabled() || s_headless) return;
  if (!s_inited) init_gpu();
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
  if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, s_win, &swaptex, &sw, &sh) || !swaptex) { SDL_SubmitGPUCommandBuffer(cmd); poll_quit(); return; }
  SDL_GPUColorTargetInfo cti = {}; cti.texture = swaptex; cti.clear_color = (SDL_FColor){ 0, 0, 0, 1 };
  cti.load_op = SDL_GPU_LOADOP_CLEAR; cti.store_op = SDL_GPU_STOREOP_STORE;
  SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(cmd, &cti, 1, NULL);
  float fpc[4] = { 1.0f, 0, 0, 0 };   // no fade (already applied in the readback)
  SDL_PushGPUFragmentUniformData(cmd, 0, fpc, sizeof fpc);
  SDL_BindGPUGraphicsPipeline(rp, s_image_pipe);
  int paneW = (int)sw / 2;
  SDL_Rect sc = { 0, 0, (int)sw, (int)sh };
  for (int i = 0; i < 2; i++) {
    SDL_GPUViewport vp = letterbox(4, 3, paneW, (int)sh);
    vp.x += (float)(i * paneW);
    SDL_SetGPUViewport(rp, &vp); SDL_SetGPUScissor(rp, &sc);
    SDL_GPUTextureSamplerBinding tsb = { s_sbs_tex[i], s_samp_linear };
    SDL_BindGPUFragmentSamplers(rp, 0, &tsb, 1);
    SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
  }
  SDL_EndGPURenderPass(rp);
  SDL_SubmitGPUCommandBuffer(cmd);
  poll_quit();
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
void gpu_gpu_present(Core* core, const uint16_t* src, int sx, int sy, int w, int h) { overlay_glue_frame_begin(core); core->game->gpu_gpu.present(src, sx, sy, w, h); }
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
void gpu_gpu_tritest(Core* core) { core->game->gpu_gpu.tritest(); }
