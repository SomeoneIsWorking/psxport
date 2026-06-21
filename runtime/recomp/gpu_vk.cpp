#include "core.h"
#include "game.h"   // Game / GpuVkState (per-instance render state)
#include "gpu_vk.h"  // public Core*-threaded API decls (wrappers below forward to core->game->gpu_vk)
// gpu_vk.c — Vulkan present backend (M0) for the Tomba2Engine port.
//
// M0 scope: take over PRESENTATION via Vulkan (SDL_Vulkan swapchain) — the software rasterizer in
// gpu_native.c still produces s_vram; here we upload the display region to a texture and draw it as a
// fullscreen quad (letterboxed 4:3), replacing the SDL_Renderer blit. This proves the VK device /
// swapchain / pipeline path end-to-end with the game running. Later milestones move rasterization
// itself onto the GPU (VRAM as a device image, triangle/sprite pipelines).
//
// Enabled by PSXPORT_VK=1 (and only when a window is on). Headless (PSXPORT_NOWINDOW) never touches VK.
// Cross-platform: MoltenVK-ready (portability enumeration/subset added only when the loader reports it).
#ifdef PSXPORT_SDL
#include <SDL2/SDL.h>
#include "cfg.h"
#include <SDL2/SDL_vulkan.h>
#include <vulkan/vulkan.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>             // clock_gettime — CPU-side per-frame timing for the `vkprof` channel
#include "gpu_vk_shaders.h"   // generated: spv_present_vert / spv_present_frag (tools/gen_vk_shaders.sh)
#include "mods.h"             // g_mods: live PC-native mod toggles (wide/ires/ssao/light), seeded from cfg
#include "overlay_glue.h"     // RmlUi mod/debug overlay integration hooks (no-op until init'd; windowed)

#define VRAM_W 1024
#define VRAM_H 512
// The R16_UINT image is taller than PSX VRAM (512): rows [VRAM_H, IMG_H) are a VK-only scratch
// framebuffer used for PC-native widescreen / higher-res rendering. PSX VRAM is fully packed
// (two 320 framebuffers + the texture atlas at x>=320), so the wide/hi-res 3D cannot be drawn
// in place — we relocate the tee'd geometry into this scratch FB (no VRAM conflict; textures are
// still sampled from rows <512). FB origin (0, FB_Y0); max FB = 960x720 (4:3 at 3x internal res, or
// 856x480 16:9 at 2x). Width is capped by VRAM_W (1024): 320*3=960 / 428*2=856 both fit.
#define FB_Y0   VRAM_H
#define FB_MAXH 720
#define IMG_H   (VRAM_H + FB_MAXH)   // 1232
#define MAX_SWAP 8

// The VRAM image is stored as R16_UINT (packed PSX 1555 words: R=bits0-4, G=5-9, B=10-14, STP=bit15) —
// the format the GP0 VRAM-copy/upload word-moves, the paletted-texture sampler, present and readback all
// see, kept bit-for-bit unchanged. R16_UINT cannot fixed-function BLEND, so the image is also created
// VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT and a SECOND view is made in VRAM_BLEND_FMT — A1R5G5B5_UNORM_PACK16
// (core since Vulkan 1.0, same 16-bit compatibility class as R16_UINT -> a legal aliased view). Its layout
// is A=bit15=STP, R=bits10-14, G=5-9, B=0-4 — PSX 1555 with R<->B SWAPPED. We keep the stored BITS as PSX
// 1555 (PSX-red in the low 5) by having the fragment shaders write PSX-blue into the view's R slot and
// PSX-red into its B slot (a pure output swizzle); the stored word is therefore identical to the old
// `uint o_px`, so every other consumer (copy/upload/sample/present/readback) is untouched. HW blend is
// per-channel and symmetric, so blending the swapped channels still composites PSX-R with PSX-R etc. The
// view is used ONLY as the render-pass color attachment, giving the 4 PSX semi modes real hardware blend.
// (A1B5G5R5 would match PSX exactly with no swizzle, but it is a Vulkan-1.4/maintenance5 format — invalid
// under our 1.1 instance per the validation layer; A1R5G5B5 is the portable 1.0-core choice.) Verified on
// RADV + llvmpipe: A1R5G5B5 has COLOR_ATTACHMENT + BLEND + SAMPLED (scratch/fmtprobe.c).
#define VRAM_FMT       VK_FORMAT_R16_UINT
#define VRAM_BLEND_FMT VK_FORMAT_A1R5G5B5_UNORM_PACK16

#define VKC(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) { \
    fprintf(stderr, "[gpu_vk] %s failed: VkResult %d (%s:%d)\n", #x, _r, __FILE__, __LINE__); exit(2); } } while (0)

static int s_vk_on = -1;
static int s_inited = 0;
static int s_failed = 0;

static SDL_Window*     s_win;
static VkInstance      s_inst;
static VkSurfaceKHR    s_surf;
static int             s_headless = 0;   // offscreen render (no window/swapchain) for the VK render-diff
static VkPhysicalDevice s_phys;
static VkDevice        s_dev;
static uint32_t        s_qfam;
static VkQueue         s_queue;

static VkSwapchainKHR  s_swap;
static VkFormat        s_swap_fmt;
static VkExtent2D      s_extent;
static int             s_swap_dirty = 0;   // window resized -> recreate the swapchain before the next frame
static uint32_t        s_swap_n;
static VkImage         s_swap_img[MAX_SWAP];
static VkImageView     s_swap_view[MAX_SWAP];
static VkFramebuffer   s_swap_fb[MAX_SWAP];

static VkRenderPass    s_rpass;
static VkDescriptorSetLayout s_dsl;
static VkPipelineLayout s_pll;
static VkPipeline      s_pipe;
static VkDescriptorPool s_dpool;
static VkDescriptorSet s_dset;
static VkSampler       s_sampler;
static VkCommandPool   s_cpool;
static VkCommandBuffer s_cmd;
static VkSemaphore     s_sem_acq, s_sem_rel[MAX_SWAP];   // release: one per swapchain image (spec-correct reuse)
static VkFence         s_fence;

// ---- `vkprof` profiling channel (PSXPORT_DEBUG=vkprof / REPL `debug vkprof`) -------------------------
// GPU time via a 2-slot timestamp query pool (write TOP at cmd begin, BOTTOM at cmd end; read the PREVIOUS
// frame's pair right after this frame's fence wait, so no extra stall). CPU phase times via clock_gettime.
// Prints a rolling average every 60 frames. This directly answers "where does render time go" — and on
// MoltenVK exposes whether the per-frame full-VRAM CPU->GPU upload + per-batch VRAM-snapshot copies dominate.
static VkQueryPool     s_tspool;          // 2 timestamps
static double          s_ts_period_ns;    // VkPhysicalDeviceLimits.timestampPeriod (ns per tick); 0 = unsupported
static int             s_ts_validbits;    // queue family timestampValidBits; 0 = no GPU timing
static int             s_ts_primed;       // a prior frame wrote timestamps -> safe to read
static int             s_vkprof = -1;     // cached cfg_dbg("vkprof") (re-checked lazily)
static long            s_vp_frames;
static double          s_vp_gpu_ms, s_vp_upload_ms, s_vp_record_ms, s_vp_submit_ms;
static long            s_vp_tri, s_vp_tex, s_vp_semi;
static long            s_vp_dirty_rects, s_vp_dirty_kpx, s_vp_full_uploads;   // per-frame VRAM->GPU upload volume
static long            s_vp_semi_grps;   // semi overlap-groups/frame: EACH does a full 1024x512 vkCmdCopyImage snapshot
static inline double now_ms() { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static void vkprof_tick() {
  if (++s_vp_frames < 60) return;
  double n = s_vp_frames;
  // `upload`/`dirty` answer the suspected M2 bottleneck: a full 1024x512 VRAM image re-upload every
  // frame vs. only the small SW-written (atlas/fill/copy) dirty rects. full=0 after frame 0 => incremental.
  fprintf(stderr, "[vkprof] %.0ff avg: GPU %.2fms | CPU upload %.2f rec %.2f submit %.2f | prims tri %ld tex %ld semi %ld"
                  " | upload rects %ld (%ldKpx) full %ld | semigrps %ld (full-VRAM copies)%s\n",
          n, s_vp_gpu_ms / n, s_vp_upload_ms / n, s_vp_record_ms / n, s_vp_submit_ms / n,
          s_vp_tri / (long)n, s_vp_tex / (long)n, s_vp_semi / (long)n,
          s_vp_dirty_rects / (long)n, s_vp_dirty_kpx / (long)n, s_vp_full_uploads,
          s_vp_semi_grps / (long)n,
          s_tspool ? "" : "  (no GPU timestamps)");
  s_vp_frames = 0; s_vp_gpu_ms = s_vp_upload_ms = s_vp_record_ms = s_vp_submit_ms = 0;
  s_vp_tri = s_vp_tex = s_vp_semi = 0;
  s_vp_dirty_rects = s_vp_dirty_kpx = s_vp_full_uploads = 0; s_vp_semi_grps = 0;
}

// GPU VRAM image (R16_UINT 1024x512) + host-visible staging (upload) + readback
static int             s_tex_undef;
static VkImage         s_tex;
static VkDeviceMemory  s_tex_mem;
static VkImageView     s_tex_view;        // UINT view: sampling, present, copy/upload, readback (1555 bits)
static VkImageView     s_tex_blend_view;  // A1B5G5R5_UNORM alias: render-pass color attachment (blendable)
// PSXPORT_SBS: a SECOND full-resolution VRAM image. Each present renders the frame's geometry twice —
// s_tex with default OT-order depth, s_tex_b with native per-vertex depth — then the windowed present
// composites them side by side (left pane samples s_tex, right pane s_tex_b). No shared-image seam.
static VkImage         s_tex_b;
static VkDeviceMemory  s_tex_b_mem;
static VkImageView     s_tex_b_view;        // UINT view (present sampling)
static VkImageView     s_tex_b_blend_view;  // A1B5G5R5_UNORM alias (render-pass color attachment)
static VkFramebuffer   s_vram_fb_b;        // render pass framebuffer over s_tex_b (+ its own depth)
static VkDescriptorSet s_dset_b;           // present descriptor: binding0 = s_tex_b

// A render Panel: one self-contained view of the frame. It owns its PERSISTENT VRAM color image, depth
// buffer, render-pass framebuffer and present descriptor, and renders independently (its own dirty-VRAM
// upload + own depth occlusion). PSXPORT_SBS composes two panels — [0] default OT depth, [1] native
// per-vertex depth — with NO shared mutable render state (which previously made them render identically:
// a per-frame copy of panel 0 destroyed panel 1's persistent native framebuffer). `native` selects the
// depth channel via the pipeline specialization constant (s_tritex_pipe[native]).
struct Panel {
  VkImage         color; VkFramebuffer fb; VkDescriptorSet dset_present; VkImage depth;
  int             color_undef, depth_undef, native;
};
static Panel s_panels[2];
static int   s_npanels = 1;

static VkBuffer        s_stage;
static VkDeviceMemory  s_stage_mem;
static void*           s_stage_ptr;
static VkDeviceSize    s_stage_sz;
static VkBuffer        s_rb;          // readback buffer (VRAM image -> host, for VK-vs-SW diff)
static VkDeviceMemory  s_rb_mem;
static void*           s_rb_ptr;

// M2 triangle rasterizer: render pass over the VRAM image + pipeline + batched vertex buffer
static VkRenderPass    s_vram_rpass;
static VkFramebuffer   s_vram_fb;
static VkPipeline      s_tri_pipe[2];   // [0]=default OT depth, [1]=native depth (SBS_NATIVE spec const)
static VkBuffer        s_vbuf;        // host-visible vertex batch
static VkDeviceMemory  s_vbuf_mem;
static void*           s_vbuf_ptr;
typedef struct { float x, y, r, g, b, ord, ordn; } TriVtx;
#define TRI_CAP 196608                // max batched vertices (= 65536 tris)

// --- Depth-ordered semi-transparency (preserve OT submission order across the opaque/semi split) ---
// The VK rasterizer batches opaque then semi into two passes, which loses the back-to-front OT order
// between an opaque prim and a semi prim. Each prim carries its OT submission index as a depth value
// (.ord, [0,1], later = greater); opaque writes depth (compare GREATER_EQUAL), semi tests depth
// without writing — so a semi prim is rejected wherever a LATER-submitted opaque prim already drew
// (e.g. background water correctly hidden behind foreground terrain), and still blends where it is in
// front. gpu_vk_set_order(idx) is called per prim from the gp0 tee before each draw.
static VkImage         s_depth;
static VkDeviceMemory  s_depth_mem;
static VkImageView     s_depth_view;
static int             s_depth_undef = 1;
// PSXPORT_SBS panel B owns its OWN depth buffer. The two panes render in one command buffer; a SHARED
// depth image has a write-after-write hazard between the two render passes (no barrier between them), so
// the second pane inherited the first's depth/occlusion (both panes looked identical). Isolated.
static VkImage         s_depth_b;
static VkDeviceMemory  s_depth_b_mem;
static VkImageView     s_depth_b_view;
static int             s_depth_b_undef = 1;
// Textured semi-transparent pipelines: one per PSX blend MODE (0=avg, 1=add, 2=sub, 3=add/4), each ×
// native-depth channel. Depth-test GREATER_OR_EQUAL, NO depth write. The 4 modes map to fixed-function
// HW blend (no more in-shader framebuffer-snapshot blend):
//   avg  (0): 0.5*src + 0.5*dst   (CONSTANT_COLOR .5, ADD)
//   add  (1): 1*src + 1*dst       (ONE/ONE, ADD)
//   sub  (2): 1*dst - 1*src       (ONE/ONE, REVERSE_SUBTRACT)
//   add4 (3): 0.25*src + 1*dst    (CONSTANT_COLOR .25 src, ONE dst, ADD)
// Blend constants (.5 / .25) are dynamic (vkCmdSetBlendConstants) and set per mode-run at draw time.
static VkPipeline      s_tritex_semi_pipe[4][2];
// Semi sub-pass 0: the OPAQUE half of a textured-semi prim (texels with STP=0 draw opaque, no HW blend),
// depth-test / NO write, fragment SEMI_PASS=0 discarding the blending texels. Per native-depth channel.
static VkPipeline      s_tritex_semi_op_pipe[2];
// Phase 2 (PSXPORT_NATIVE_DEPTH): per-vertex REAL depth for the current triangle. When non-NULL it
// overrides the per-prim OT-order s_cur_ord, so gl_Position.z carries the native view-space depth (from
// proj_pz_to_ord) and the D32 buffer does true per-pixel occlusion instead of painter/OT order. Set by
// the gp0 tee AFTER gpu_vk_set_order (which clears it, so 2D/sprite prims fall back to OT order).
void GpuVkState::set_vd(const float* d3) { s_vd = d3; }
// PSXPORT_SBS second (native) depth channel: every vertex carries BOTH the default-mode depth (.ord)
// and the native-mode depth (.ordn) so one geometry batch renders both ways (left=ord, right=ordn).
// In a normal (non-SBS) run the native channel mirrors the default one (ordn == ord), so it is inert.
void GpuVkState::set_vd_n(const float* d3) { s_vdn = d3; }
// Sub-pixel float screen XY for the engine-owned 3D world path (vertex smoothing / issue #15). Set AFTER
// set_order (which clears it) by gpu_emit_rq_item just before a world draw; tex_emit then uses these floats
// for the vertex position instead of the integer xs/ys. NULL = snap to integer (2D/HUD/un-owned prims).
void GpuVkState::set_xyf(const float* xf, const float* yf) { s_xf = xf; s_yf = yf; }
// Single shared D32 buffer is partitioned into THREE depth bands (nearer = larger ord = wins, with a
// 0.0 clear): a 2D BACKGROUND band [0, NATIVE_3D_MIN) for non-projected backdrop layers (water/sky),
// the 3D WORLD band [NATIVE_3D_MIN, NATIVE_3D_MAX] (real per-vertex depth), and a 2D OVERLAY band
// (NATIVE_3D_MAX, 1] for HUD/UI/banners that composite OVER the 3D world. The background band is why
// the ocean no longer punches through the terrain: Tomba2's water/sky are screen-space 2D layers (no
// GTE projection — see engine_re.md), so without their own FAR band they fell into the near overlay
// band and occluded the whole world. Background vs HUD is split by OT order (a 2D prim drawn before
// any 3D prim this frame is a backdrop; after, it's HUD) — see gpu_native.c s_seen3d.
// (Interim until Phase 2 routes 3D and 2D to separate targets; not an offset to align pixels.)
#define NATIVE_3D_MIN 0.0625f
#define NATIVE_3D_MAX 0.9375f
// Map a normalized per-vertex 3D depth d in [0,1] into the 3D WORLD band [NATIVE_3D_MIN, NATIVE_3D_MAX].
static inline float ord3d(float d) { return NATIVE_3D_MIN + d * (NATIVE_3D_MAX - NATIVE_3D_MIN); }
void GpuVkState::set_order(unsigned idx) { s_cur_ord = (float)(idx + 1) / 65536.0f; if (s_cur_ord > 1.0f) s_cur_ord = 1.0f;
                                      s_cur_ordn = s_cur_ord; s_vd = 0; s_vdn = 0; s_xf = 0; s_yf = 0; }
// 2D/HUD prim under PSXPORT_NATIVE_DEPTH: OT order, biased into the overlay band above the 3D world.
void GpuVkState::set_order_2d(unsigned idx) { float t = (float)(idx + 1) / 65536.0f; if (t > 1.0f) t = 1.0f;
                                         s_cur_ord = NATIVE_3D_MAX + (1.0f - NATIVE_3D_MAX) * t; s_vd = 0; }
// Same overlay-band bias, but for the SBS native channel only (leaves the default .ord untouched).
void GpuVkState::set_order_2d_n(unsigned idx) { float t = (float)(idx + 1) / 65536.0f; if (t > 1.0f) t = 1.0f;
                                           s_cur_ordn = NATIVE_3D_MAX + (1.0f - NATIVE_3D_MAX) * t; s_vdn = 0; }
// 2D BACKGROUND prim (drawn before any 3D this frame): OT order biased into the FAR band BELOW the 3D
// world, so the backdrop (water/sky) sits behind the terrain instead of occluding it.
void GpuVkState::set_order_2d_bg(unsigned idx) { float t = (float)(idx + 1) / 65536.0f; if (t > 1.0f) t = 1.0f;
                                            s_cur_ord = NATIVE_3D_MIN * t; s_vd = 0; }
void GpuVkState::set_order_2d_bg_n(unsigned idx) { float t = (float)(idx + 1) / 65536.0f; if (t > 1.0f) t = 1.0f;
                                              s_cur_ordn = NATIVE_3D_MIN * t; s_vdn = 0; }

// --- PSXPORT_SSAO: PC-native screen-space ambient occlusion (post pass) -------------------------------
// After the geometry pass, a fullscreen pass reads the color (s_tex) + native 3-band depth (s_depth),
// darkens 3D-world creases/contacts, and writes into s_ssao_img; the result is copied back into s_tex so
// present/dump see it with no further wiring. Gated by PSXPORT_SSAO (implies the native-depth path);
// disabled under PSXPORT_SBS. Params are env-tunable for live eyeball tuning (logged at first use).
float proj_near_pz(void);                 // gte_beetle.c: near-plane view-Z (= H/2) for depth linearize
static VkImage         s_ssao_img;
static VkDeviceMemory  s_ssao_mem;
static VkImageView     s_ssao_view;
static VkRenderPass    s_ssao_rpass;
static VkFramebuffer   s_ssao_fb;
static VkDescriptorSetLayout s_ssao_dsl;
static VkPipelineLayout s_ssao_pll;
static VkDescriptorPool s_ssao_dpool;
static VkDescriptorSet s_ssao_dset;
static VkPipeline      s_ssao_pipe;

// --- Dynamic SHADOW MAPPING (cast by the engine-native directional light) ----------------------------
// A real shadow map: the opaque world geometry is captured at submit time as VIEW-SPACE triangles
// (gpu_vk_shadow_push_tri, fed (ir1,ir2,pz) per vertex from the world submitters), then rasterized into a
// D32 depth image from the DIRECTIONAL LIGHT's orthographic view (shadow_pass + shadow.vert). The deferred
// pass (ssao.frag) reconstructs each 3D pixel's view-space position, transforms it by the SAME light
// view-projection, and PCF-compares against this map to darken occluded pixels. No PSX intricacy: it is a
// plain PC light-space raster of the engine's own scene geometry. Single-instance (disabled under SBS,
// like the deferred pass). Resources created in create_shadow() once the device exists.
#define SHADOW_DIM 2048                      // shadow map resolution (square ortho)
#define SHADOW_VTX_CAP (3 * 65536)           // captured shadow-geometry vertices per frame (= 65536 tris)
typedef struct { float vx, vy, vz; } ShadowVtx;   // view-space position (x=ir1, y=ir2, z=pz)
static int             s_shadow_n;           // captured shadow vertices THIS frame (host stream)
static VkImage         s_shadow_img;         // D32 light-view depth map
static VkDeviceMemory  s_shadow_mem;
static VkImageView     s_shadow_view;
static int             s_shadow_undef = 1;
static VkRenderPass    s_shadow_rpass;
static VkFramebuffer   s_shadow_fb;
static VkPipeline      s_shadow_pipe;
static VkPipelineLayout s_shadow_pll;
static VkBuffer        s_shadow_vbuf;        // host-visible captured geometry (view-space verts)
static VkDeviceMemory  s_shadow_vbuf_mem;
static void*           s_shadow_vbuf_ptr;
static float           s_shadow_lvp[16];     // light view-projection built this frame (col-major, for the deferred sample)
static int             s_shadow_lvp_valid;   // 1 once shadow_pass built a matrix this frame
static int shadows_on(void)  { return g_mods.shadows && g_mods.light; }   // shadows are cast BY the light
// AO/light are LIVE (g_mods, overlay-toggled). proj getters feed the LIGHT normal reconstruction.
// ui_infra() = the overlay needs the deferred infra + native-depth kept on so SSAO/LIGHT can be
// toggled at runtime even if they started off.
static int ssao_on(void) { return g_mods.ssao; }
float proj_plane_h(void);
void  proj_screen_center(float* cx, float* cy);
static int light_on(void)    { return g_mods.light; }
// Lighting is now ENGINE-NATIVE (engine/engine_submit.cpp engine_shade_face — per-face normal in the
// world-quad submit), NOT this deferred pass (user directive 2026-06-21: it must be engine-native, and the
// deferred light/AO pass made semi-transparent water vanish by re-shading the geometry behind it). So the
// deferred pass runs for SSAO only; the light bit is never set in its flags.
// The deferred post pass (ssao.frag) runs when SSAO and/or shadows are on — shadows reuse it to sample the
// shadow map + darken (light remains engine-native in the submitters; the deferred pass never sets light).
static int deferred_on(void) { return ssao_on() || shadows_on(); }
int gpu_vk_shadows_active(void) { return shadows_on(); }
// Capture one opaque world triangle's view-space verts into the host shadow-geometry stream (transformed to
// light space in shadow.vert at the pass). v0/v1/v2 each point to {x=ir1, y=ir2, z=pz}. No-op when shadows
// are off / resources not yet up / the stream is full. Cheap: 9 float stores.
void gpu_vk_shadow_push_tri(Core* core, const float* v0, const float* v1, const float* v2) {
  (void)core;
  if (!shadows_on() || !s_shadow_vbuf_ptr) return;
  if (s_shadow_n + 3 > SHADOW_VTX_CAP) return;
  ShadowVtx* d = (ShadowVtx*)s_shadow_vbuf_ptr + s_shadow_n;
  d[0] = (ShadowVtx){ v0[0], v0[1], v0[2] };
  d[1] = (ShadowVtx){ v1[0], v1[1], v1[2] };
  d[2] = (ShadowVtx){ v2[0], v2[1], v2[2] };
  s_shadow_n += 3;
}
// The deferred SSAO/light infra is built whenever the overlay is available (g_mods.ui, always on) so the
// player can toggle SSAO/light live. The passes themselves stay off until toggled (ssao_on/light_on).
static int ui_infra(void)    { return g_mods.ui; }

// M3 textured rasterizer: a VRAM snapshot image the texture sampler reads (avoids render/sample feedback
// loop), its descriptor set, the textured pipeline, and a textured-vertex batch.
struct TexVtx { float x, y, u, v, r, g, b; int32_t tp[4], clut[4], tw[4], da[4]; float ord, ordn; };  // 100 bytes
#define TEX_CAP 196608
static VkImage         s_vram_tex;    // texture-source snapshot (copy of VRAM before a draw batch)
static VkDeviceMemory  s_vram_tex_mem;
static VkImageView     s_vram_tex_view;
static int             s_vram_tex_undef;
static VkDescriptorSet s_dset_tex;    // binding0 = s_vram_tex
static VkPipeline      s_tritex_pipe[2];   // [0]=default OT depth, [1]=native depth
static VkBuffer        s_tvbuf;
static VkDeviceMemory  s_tvbuf_mem;
static void*           s_tvbuf_ptr;
static VkBuffer        s_semibuf;     // SEMI-transparent prims (drawn after opaque, sampling the framebuffer)
static VkDeviceMemory  s_semibuf_mem;
static void*           s_semibuf_ptr;
// OT-order-correct semi blending: VK draws all semi prims in one pass sampling ONE pre-semi snapshot,
// so OVERLAPPING semi prims don't accumulate (each blends against the same scene; the later one
// overwrites) — unlike the sequential PSX/SW rasterizer. That broke stacked full-screen subtractive
// fade tiles (intro fade flash). Fix: partition the semi batch into GROUPS where no prim overlaps an
// earlier prim in the same group, then draw each group with its own fresh framebuffer snapshot so a
// later group sees the earlier groups' blends. Non-overlapping semis (water tiles) stay one group.
// Called once per SEMI prim (before its triangles are appended) with the prim's ABSOLUTE bbox. Starts a
// new group whenever the prim overlaps the current group's accumulated bbox.
void GpuVkState::semi_group(int x0, int y0, int x1, int y1) {
  if (!s_sg_valid) { s_sg_x0=x0; s_sg_y0=y0; s_sg_x1=x1; s_sg_y1=y1; s_sg_valid=1; return; }
  int overlap = (x0 < s_sg_x1 && s_sg_x0 < x1 && y0 < s_sg_y1 && s_sg_y0 < y1);  // strict (touching edges OK)
  if (overlap) {
    if (s_semi_grp_n < SEMI_GRP_CAP) s_semi_grp[s_semi_grp_n++] = s_semi_n;   // boundary = this prim's first vertex
    s_sg_x0=x0; s_sg_y0=y0; s_sg_x1=x1; s_sg_y1=y1;   // restart accumulated bbox at this prim
  } else {
    if (x0 < s_sg_x0) s_sg_x0=x0; if (y0 < s_sg_y0) s_sg_y0=y0;
    if (x1 > s_sg_x1) s_sg_x1=x1; if (y1 > s_sg_y1) s_sg_y1=y1;
  }
}
// Dirty VRAM regions written by SW this frame (CPU->VRAM uploads, VRAM copies, fills) — mirrored from
// s_vram into the PERSISTENT VK VRAM image at present (the framebuffer region stays VK-owned/persistent).
// Report the last frame's VK batched vertex counts (opaque-flat tris, opaque-textured tris, semi
// tris). Lets the debug server tell apart "semi prims never batched" (tee bug) from "batched but
// not visible" (semi pass / shader bug) — e.g. the missing semi-transparent puddle water.
void GpuVkState::stats(int* tri, int* tex, int* semi) {
  if (tri) *tri = s_dbg_tri; if (tex) *tex = s_dbg_tex; if (semi) *semi = s_dbg_semi;
}
void GpuVkState::dirty(int x, int y, int w, int h) {
  if (s_dirty_n >= DIRTY_CAP || w <= 0 || h <= 0) return;
  if (x < 0) { w += x; x = 0; } if (y < 0) { h += y; y = 0; }
  if (x + w > VRAM_W) w = VRAM_W - x; if (y + h > VRAM_H) h = VRAM_H - y;
  if (w <= 0 || h <= 0) return;
  s_dirty[s_dirty_n++] = (VkRect){ x, y, w, h };
}

// PC-native widescreen + NATIVE higher internal resolution. When s_wide, the tee'd geometry is
// relocated into the scratch FB (rows >=FB_Y0) at a TRUE wider horizontal FOV (no GTE squish, no
// present stretch): the native projection is kept and re-centered into a wider buffer, so MORE world
// shows on each side. PSXPORT_IRES=N renders that FB at N x (320|428)x240 by SCALING THE RASTERIZATION
// VIEWPORT — the engine's own submitted geometry, just sampled denser (crisp 3D edges + texel
// sampling), NOT the rejected supersample-and-downscale FB-cram. Width is capped by VRAM_W=1024:
// 4:3 -> ires<=3 (960), 16:9 -> ires<=2 (856). use_fb() == "render via the scaled scratch FB".
// Live widescreen + internal-res, read from g_mods each frame (the overlay toggles them at runtime).
// s_wide/s_ires are now accessors over g_mods (clamped: 4:3 -> ires<=3 (960px), 16:9 -> ires<=2 (856px),
// both within VRAM_W=1024). wide_init() just seeds g_mods from cfg once.
// Live window size (swapchain extent). 0 before the swapchain exists -> fall back to native 4:3.
static int win_w(void) { return s_extent.width  ? (int)s_extent.width  : 320; }
static int win_h(void) { return s_extent.height ? (int)s_extent.height : 240; }
// Native (pre-ires) FB width for the current aspect mode. 4:3=320; 16:9=428; 21:9=560; AUTO = the live
// window aspect (full, even, clamped to VRAM_W=1024). Height is always 240 (the FOV widens horizontally).
static int wide_native_w(void) {
  switch (g_mods.aspect) {
    case ASPECT_16_9: return 428;                         // 240*16/9 ~= 426.7 -> 428
    case ASPECT_21_9: return 560;                         // 240*21/9 == 560
    case ASPECT_AUTO: { int w = (int)((240.0 * win_w()) / win_h() + 0.5); w &= ~1;
                        if (w < 320) w = 320; if (w > VRAM_W) w = VRAM_W; return w; }
    default:          return 320;                         // ASPECT_4_3
  }
}
static int W_wide(void) { return g_mods.aspect != ASPECT_4_3; }
// Internal-res scale. Manual: g_mods.ires. Auto: ~round(window_h/240). Always clamped so the scaled FB
// width (wide_native_w*ires) stays within VRAM_W=1024 and ires in [1,3].
static int W_ires(void) {
  int nw = wide_native_w(), cap = VRAM_W / nw; if (cap < 1) cap = 1; if (cap > 3) cap = 3;
  int i = g_mods.ires_auto ? (int)((double)win_h() / 240.0 + 0.5) : g_mods.ires;
  return i < 1 ? 1 : (i > cap ? cap : i);
}
#define s_wide (W_wide())
#define s_ires (W_ires())
static int FBW(void) { return wide_native_w() * s_ires; }
static int FBH(void) { return 240 * s_ires; }
static int use_fb(void) { if (cfg_on("PSXPORT_NO_FB")) return 0;   // DIAG: hard-disable the scratch FB
                          return s_wide || s_ires > 1; }   // 3D goes through the scaled scratch FB
// Does THIS frame's content live in the scaled scratch FB? Only when hi-res/wide is configured AND the
// frame actually drew 3D geometry (which is what gets relocated into the FB). A pure-2D screen (SCEA /
// FMV / title / menu — a VRAM-resident image, no tee'd 3D) has an EMPTY scratch FB, so it must render +
// present from the native VRAM display region at 4:3 instead. Render (push_wide / FB clear) and present
// (sample region + aspect) both key off THIS so they stay consistent.
int GpuVkState::frame_via_fb() { return use_fb() && gpu_seen3d_this_frame(&game->core); }
static void wide_init(void) { mods_init(); }

// ---- Genuine engine-level widescreen ("no faking") ---------------------------------------------
// Widescreen is a REAL wider frame, not a present-time spread: the engine projects with a wider OFX
// (genuine horizontal FOV via Beetle's GTE, see ov_set_geom_offset) and the wide content is rasterized
// 1:1 into the scratch FB. There is no FB-hack centering path anymore — any non-4:3 aspect IS the
// genuine-wide path. (4:3 stays byte-identical: this returns 0, so OFX/cull/2D-scale are all untouched.)
int gpu_vk_wide_engine(void) {
  mods_init();
  return g_mods.aspect != ASPECT_4_3;
}
int gpu_vk_wide_engine_ofx(void) { return wide_native_w() / 2; }   // wide projection center (214 @16:9)
int gpu_vk_wide_engine_w(void)   { return wide_native_w(); }       // wide draw-clip width (428 @16:9)
// Effective video status for the overlay (computed values, incl. auto). Any out ptr may be NULL.
void gpu_vk_video_status(int* native_w, int* ires, int* fbw, int* fbh, int* ww, int* wh, int* ires_cap) {
  wide_init();
  int nw = wide_native_w(), cap = VRAM_W / nw; if (cap < 1) cap = 1; if (cap > 3) cap = 3;
  if (native_w) *native_w = nw;   if (ires) *ires = s_ires;  if (fbw) *fbw = FBW();
  if (fbh) *fbh = FBH();          if (ww) *ww = win_w();      if (wh) *wh = win_h();
  if (ires_cap) *ires_cap = cap;
}

int gpu_vk_enabled(void) {
  if (s_vk_on < 0) {
    // VK is THE renderer. It runs windowed (PSXPORT_GPU_WINDOW) or offscreen-headless
    // (PSXPORT_VK_HEADLESS, deterministic — the REPL screenshot harness). The SW rasterizer remains only
    // as the per-pixel fill/copy fallback for non-tee'd GP0 ops, never the present path.
    const char* w = cfg_str("PSXPORT_GPU_WINDOW");
    int win = w && atoi(w) != 0;
    s_headless = cfg_on("PSXPORT_VK_HEADLESS") ? 1 : 0;
    s_vk_on = (s_headless || win) ? 1 : 0;
  }
  return s_vk_on && !s_failed;
}

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want) {
  VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(s_phys, &mp);
  for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
    if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
  fprintf(stderr, "[gpu_vk] no memory type for 0x%x want 0x%x\n", bits, want); exit(2);
}

static int has_ext(const VkExtensionProperties* a, uint32_t n, const char* name) {
  for (uint32_t i = 0; i < n; i++) if (!strcmp(a[i].extensionName, name)) return 1;
  return 0;
}

static VkShaderModule make_shader(const uint32_t* code, unsigned len) {
  VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
  ci.codeSize = len; ci.pCode = code;
  VkShaderModule m; VKC(vkCreateShaderModule(s_dev, &ci, 0, &m)); return m;
}

static void create_vram(void);   // forward decl: defined after init_vk, called from it
static void create_ssao(void);   // PSXPORT_SSAO resources (post pass); created only when ssao_on()
static void create_shadow(void); // dynamic shadow-map resources (depth image + light-view depth pipeline)
static void make_hostbuf(VkDeviceSize sz, VkBufferUsageFlags use, VkBuffer*, VkDeviceMemory*, void**);
static void img_barrier_on(VkImage, VkImageLayout, VkImageLayout, VkPipelineStageFlags,
                           VkPipelineStageFlags, VkAccessFlags, VkAccessFlags);

// The size the swapchain SHOULD be right now. The authoritative source is the surface's currentExtent;
// but Wayland (and some other WSI) report currentExtent == 0xFFFFFFFF ("you pick"), so there we must ask
// SDL for the live drawable (pixel) size and clamp it to the surface's allowed range. This is what makes
// resize tracking driver-independent — we never depend on a possibly-undelivered SDL_WINDOWEVENT.
static VkExtent2D surface_extent_now(void) {
  VkSurfaceCapabilitiesKHR caps;
  VKC(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(s_phys, s_surf, &caps));
  if (caps.currentExtent.width != 0xFFFFFFFFu) return caps.currentExtent;
  int w = 0, h = 0;
  SDL_Vulkan_GetDrawableSize(s_win, &w, &h);
  VkExtent2D e = { (uint32_t)w, (uint32_t)h };
  if (e.width  < caps.minImageExtent.width)  e.width  = caps.minImageExtent.width;
  if (e.width  > caps.maxImageExtent.width)  e.width  = caps.maxImageExtent.width;
  if (e.height < caps.minImageExtent.height) e.height = caps.minImageExtent.height;
  if (e.height > caps.maxImageExtent.height) e.height = caps.maxImageExtent.height;
  return e;
}

static void create_swapchain(void) {
  VkSurfaceCapabilitiesKHR caps;
  VKC(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(s_phys, s_surf, &caps));
  s_extent = surface_extent_now();
  uint32_t fn = 0; vkGetPhysicalDeviceSurfaceFormatsKHR(s_phys, s_surf, &fn, 0);
  VkSurfaceFormatKHR fmts[32]; if (fn > 32) fn = 32;
  vkGetPhysicalDeviceSurfaceFormatsKHR(s_phys, s_surf, &fn, fmts);
  VkSurfaceFormatKHR pick = fmts[0];
  for (uint32_t i = 0; i < fn; i++)
    if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM) { pick = fmts[i]; break; }
  s_swap_fmt = pick.format;
  uint32_t want = caps.minImageCount + 1;
  if (caps.maxImageCount && want > caps.maxImageCount) want = caps.maxImageCount;

  VkSwapchainCreateInfoKHR ci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
  ci.surface = s_surf; ci.minImageCount = want; ci.imageFormat = pick.format;
  ci.imageColorSpace = pick.colorSpace; ci.imageExtent = s_extent; ci.imageArrayLayers = 1;
  ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ci.preTransform = caps.currentTransform;
  ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;   // always supported
  ci.clipped = VK_TRUE;
  VKC(vkCreateSwapchainKHR(s_dev, &ci, 0, &s_swap));

  vkGetSwapchainImagesKHR(s_dev, s_swap, &s_swap_n, 0);
  if (s_swap_n > MAX_SWAP) s_swap_n = MAX_SWAP;
  vkGetSwapchainImagesKHR(s_dev, s_swap, &s_swap_n, s_swap_img);
  for (uint32_t i = 0; i < s_swap_n; i++) {
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = s_swap_img[i]; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = s_swap_fmt;
    vi.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VKC(vkCreateImageView(s_dev, &vi, 0, &s_swap_view[i]));
    VkFramebufferCreateInfo fi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    fi.renderPass = s_rpass; fi.attachmentCount = 1; fi.pAttachments = &s_swap_view[i];
    fi.width = s_extent.width; fi.height = s_extent.height; fi.layers = 1;
    VKC(vkCreateFramebuffer(s_dev, &fi, 0, &s_swap_fb[i]));
  }
}

static void destroy_swapchain(void) {
  for (uint32_t i = 0; i < s_swap_n; i++) {
    vkDestroyFramebuffer(s_dev, s_swap_fb[i], 0);
    vkDestroyImageView(s_dev, s_swap_view[i], 0);
  }
  vkDestroySwapchainKHR(s_dev, s_swap, 0);
}

static void recreate_swapchain(void) {
  vkDeviceWaitIdle(s_dev);
  destroy_swapchain();
  create_swapchain();
}

static void init_vk(void) {
  s_inited = 1;
  mods_init();   // seed the live mod state from cfg before any ssao_on()/ui_infra() decision below
  // instance extensions: SDL surface exts (+ portability enumeration if the loader has it). Headless
  // (offscreen) needs no window/surface extensions at all.
  const char* exts[16]; unsigned ext_n = 0;
  if (!s_headless) {
    SDL_Init(SDL_INIT_VIDEO);
    // Windowed by default (easier to drive/inspect); opt into fullscreen with PSXPORT_FULLSCREEN=1.
    // (Legacy PSXPORT_WINDOWED=0 can still force fullscreen.)
    int fullscreen = cfg_on("PSXPORT_FULLSCREEN")
                  || (cfg_str("PSXPORT_WINDOWED") && atoi(cfg_str("PSXPORT_WINDOWED")) == 0);
    Uint32 flags = SDL_WINDOW_VULKAN | (fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : SDL_WINDOW_RESIZABLE);
    s_win = SDL_CreateWindow("Tomba! 2 (Vulkan)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             960, 720, flags);
    if (!s_win) { fprintf(stderr, "[gpu_vk] SDL_CreateWindow(VULKAN) failed: %s\n", SDL_GetError()); exit(2); }
    SDL_Vulkan_GetInstanceExtensions(s_win, &ext_n, 0);
    if (ext_n > 12) ext_n = 12;
    SDL_Vulkan_GetInstanceExtensions(s_win, &ext_n, exts);
  }
  uint32_t avail_n = 0; vkEnumerateInstanceExtensionProperties(0, &avail_n, 0);
  VkExtensionProperties* avail = (VkExtensionProperties*)malloc(sizeof *avail * avail_n);
  vkEnumerateInstanceExtensionProperties(0, &avail_n, avail);
  VkInstanceCreateFlags inst_flags = 0;
  if (has_ext(avail, avail_n, "VK_KHR_portability_enumeration")) {
    exts[ext_n++] = "VK_KHR_portability_enumeration";
    inst_flags |= 0x00000001; /* VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR */
  }
  free(avail);

  VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
  app.pApplicationName = "Tomba2Engine"; app.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
  ici.flags = inst_flags; ici.pApplicationInfo = &app;
  ici.enabledExtensionCount = ext_n; ici.ppEnabledExtensionNames = exts;
  VKC(vkCreateInstance(&ici, 0, &s_inst));

  if (!s_headless && !SDL_Vulkan_CreateSurface(s_win, s_inst, &s_surf)) {
    fprintf(stderr, "[gpu_vk] SDL_Vulkan_CreateSurface failed: %s\n", SDL_GetError()); exit(2);
  }

  // Pick a physical device with a graphics(+present, unless headless) queue family. Among the suitable
  // ones PREFER real hardware: discrete > integrated > virtual > other > CPU. This matters on systems
  // where the loader also exposes a SOFTWARE ICD (SwiftShader/llvmpipe, or a CPU MoltenVK config) — the
  // old "first suitable device" pick could silently land on a software rasterizer. We also LOG the chosen
  // device's name + type so "is it actually software rendering?" is answerable from the boot output (on
  // macOS this should read the Apple GPU via MoltenVK as INTEGRATED/DISCRETE, never CPU).
  uint32_t pn = 0; vkEnumeratePhysicalDevices(s_inst, &pn, 0);
  VkPhysicalDevice* phys = (VkPhysicalDevice*)malloc(sizeof *phys * pn);
  vkEnumeratePhysicalDevices(s_inst, &pn, phys);
  int dev_portability = 0;
  s_phys = VK_NULL_HANDLE;
  int best_score = -100; VkPhysicalDeviceProperties best_props = {};
  auto type_score = [](VkPhysicalDeviceType t) -> int {
    switch (t) {
      case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return 4;
      case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 3;
      case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return 2;
      case VK_PHYSICAL_DEVICE_TYPE_OTHER:          return 1;
      case VK_PHYSICAL_DEVICE_TYPE_CPU:            return 0;   // software rasterizer — last resort
      default:                                     return 1;
    }
  };
  for (uint32_t i = 0; i < pn; i++) {
    uint32_t qn = 0; vkGetPhysicalDeviceQueueFamilyProperties(phys[i], &qn, 0);
    VkQueueFamilyProperties* qf = (VkQueueFamilyProperties*)malloc(sizeof *qf * qn);
    vkGetPhysicalDeviceQueueFamilyProperties(phys[i], &qn, qf);
    int found_q = -1;
    for (uint32_t q = 0; q < qn; q++) {
      VkBool32 present = 0;
      if (!s_headless) vkGetPhysicalDeviceSurfaceSupportKHR(phys[i], q, s_surf, &present);
      if ((qf[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (s_headless || present)) { found_q = (int)q; break; }
    }
    free(qf);
    if (found_q < 0) continue;
    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(phys[i], &props);
    int score = type_score(props.deviceType);
    if (score > best_score) { best_score = score; s_phys = phys[i]; s_qfam = (uint32_t)found_q; best_props = props; }
  }
  free(phys);
  if (s_phys == VK_NULL_HANDLE) { fprintf(stderr, "[gpu_vk] no suitable GPU\n"); exit(2); }
  { const char* tn = "?";
    switch (best_props.deviceType) {
      case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   tn = "DISCRETE_GPU"; break;
      case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: tn = "INTEGRATED_GPU"; break;
      case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    tn = "VIRTUAL_GPU"; break;
      case VK_PHYSICAL_DEVICE_TYPE_CPU:            tn = "CPU (SOFTWARE!)"; break;
      default:                                     tn = "OTHER"; break;
    }
    fprintf(stderr, "[gpu_vk] device: %s  [%s]\n", best_props.deviceName, tn);
    if (best_props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU)
      fprintf(stderr, "[gpu_vk] WARNING: selected a CPU/software Vulkan device — no hardware GPU was available.\n");
  }

  uint32_t den = 0; vkEnumerateDeviceExtensionProperties(s_phys, 0, &den, 0);
  VkExtensionProperties* de = (VkExtensionProperties*)malloc(sizeof *de * den);
  vkEnumerateDeviceExtensionProperties(s_phys, 0, &den, de);
  dev_portability = has_ext(de, den, "VK_KHR_portability_subset");
  free(de);

  const char* dexts[2]; uint32_t dext_n = 0;
  if (!s_headless) dexts[dext_n++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;   // no swapchain offscreen
  if (dev_portability) dexts[dext_n++] = "VK_KHR_portability_subset";

  float prio = 1.0f;
  VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
  qci.queueFamilyIndex = s_qfam; qci.queueCount = 1; qci.pQueuePriorities = &prio;
  VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
  dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
  dci.enabledExtensionCount = dext_n; dci.ppEnabledExtensionNames = dexts;
  VKC(vkCreateDevice(s_phys, &dci, 0, &s_dev));
  vkGetDeviceQueue(s_dev, s_qfam, 0, &s_queue);

  // render pass (single color attachment: clear -> present)
  VkAttachmentDescription at = {0};
  // format set after we know the swapchain format; create swapchain format first:
  // (we need the format before the render pass; query it here)
  if (s_headless) { s_swap_fmt = VK_FORMAT_B8G8R8A8_UNORM; }   // unused present format; render pass needs one
  else { uint32_t fn = 0; vkGetPhysicalDeviceSurfaceFormatsKHR(s_phys, s_surf, &fn, 0);
    VkSurfaceFormatKHR fmts[32]; if (fn > 32) fn = 32;
    vkGetPhysicalDeviceSurfaceFormatsKHR(s_phys, s_surf, &fn, fmts);
    s_swap_fmt = fmts[0].format;
    for (uint32_t i = 0; i < fn; i++) if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM) { s_swap_fmt = fmts[i].format; break; } }
  at.format = s_swap_fmt; at.samples = VK_SAMPLE_COUNT_1_BIT;
  at.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; at.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  at.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; at.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  at.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; at.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  VkAttachmentReference ar = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
  VkSubpassDescription sp = {0}; sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  sp.colorAttachmentCount = 1; sp.pColorAttachments = &ar;
  VkSubpassDependency dep = {0};
  dep.srcSubpass = VK_SUBPASS_EXTERNAL; dep.dstSubpass = 0;
  dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  VkRenderPassCreateInfo rpi = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
  rpi.attachmentCount = 1; rpi.pAttachments = &at; rpi.subpassCount = 1; rpi.pSubpasses = &sp;
  rpi.dependencyCount = 1; rpi.pDependencies = &dep;
  VKC(vkCreateRenderPass(s_dev, &rpi, 0, &s_rpass));

  // descriptor set layout: binding 0 = combined image sampler (fragment)
  VkDescriptorSetLayoutBinding b = {0};
  b.binding = 0; b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  b.descriptorCount = 1; b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  VkDescriptorSetLayoutCreateInfo dli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
  dli.bindingCount = 1; dli.pBindings = &b;
  VKC(vkCreateDescriptorSetLayout(s_dev, &dli, 0, &s_dsl));
  VkPushConstantRange pcr[2] = {
    { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16 },    // present: ivec4 display rect (x,y,w,h)
    { VK_SHADER_STAGE_VERTEX_BIT, 16, 32 },     // tri/tritex: VPC wa,wb (wide/supersample transform)
  };
  VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
  pli.setLayoutCount = 1; pli.pSetLayouts = &s_dsl;
  pli.pushConstantRangeCount = 2; pli.pPushConstantRanges = pcr;
  VKC(vkCreatePipelineLayout(s_dev, &pli, 0, &s_pll));

  // graphics pipeline: fullscreen triangle, no vertex input, dynamic viewport/scissor
  VkShaderModule vs = make_shader(spv_present_vert, spv_present_vert_len);
  VkShaderModule fs = make_shader(spv_present_frag, spv_present_frag_len);
  VkPipelineShaderStageCreateInfo st[2] = {
    { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", 0 },
    { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", 0 },
  };
  VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
  VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
  ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
  vp.viewportCount = 1; vp.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
  rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState cba = {0}; cba.colorWriteMask = 0xF;
  VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
  cb.attachmentCount = 1; cb.pAttachments = &cba;
  VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
  VkPipelineDynamicStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
  ds.dynamicStateCount = 2; ds.pDynamicStates = dyn;
  VkGraphicsPipelineCreateInfo gp = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
  gp.stageCount = 2; gp.pStages = st; gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia;
  gp.pViewportState = &vp; gp.pRasterizationState = &rs; gp.pMultisampleState = &ms;
  gp.pColorBlendState = &cb; gp.pDynamicState = &ds; gp.layout = s_pll; gp.renderPass = s_rpass;
  VKC(vkCreateGraphicsPipelines(s_dev, VK_NULL_HANDLE, 1, &gp, 0, &s_pipe));
  vkDestroyShaderModule(s_dev, vs, 0); vkDestroyShaderModule(s_dev, fs, 0);

  // sampler + descriptor pool + set
  VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
  si.magFilter = si.minFilter = VK_FILTER_NEAREST;   // R16_UINT VRAM: integer texture (no linear filter)
  si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  VKC(vkCreateSampler(s_dev, &si, 0, &s_sampler));
  VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3 };
  VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
  dpi.maxSets = 3; dpi.poolSizeCount = 1; dpi.pPoolSizes = &ps;
  VKC(vkCreateDescriptorPool(s_dev, &dpi, 0, &s_dpool));
  VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
  dai.descriptorPool = s_dpool; dai.descriptorSetCount = 1; dai.pSetLayouts = &s_dsl;
  VKC(vkAllocateDescriptorSets(s_dev, &dai, &s_dset));        // binding0 = VRAM image (present)
  VKC(vkAllocateDescriptorSets(s_dev, &dai, &s_dset_tex));    // binding0 = VRAM snapshot (textured sampling)
  VKC(vkAllocateDescriptorSets(s_dev, &dai, &s_dset_b));      // binding0 = SBS native-depth VRAM image (present)

  // command pool + buffer, sync
  VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
  cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; cpi.queueFamilyIndex = s_qfam;
  VKC(vkCreateCommandPool(s_dev, &cpi, 0, &s_cpool));
  VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
  cai.commandPool = s_cpool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
  VKC(vkAllocateCommandBuffers(s_dev, &cai, &s_cmd));
  VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
  VKC(vkCreateSemaphore(s_dev, &sci, 0, &s_sem_acq));
  for (int i = 0; i < MAX_SWAP; i++) VKC(vkCreateSemaphore(s_dev, &sci, 0, &s_sem_rel[i]));
  VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, 0, VK_FENCE_CREATE_SIGNALED_BIT };
  VKC(vkCreateFence(s_dev, &fci, 0, &s_fence));

  // vkprof: GPU timestamp support (period from device limits, valid bits from the queue family).
  { VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(s_phys, &pp);
    s_ts_period_ns = pp.limits.timestampPeriod;
    uint32_t qn = 0; vkGetPhysicalDeviceQueueFamilyProperties(s_phys, &qn, 0);
    VkQueueFamilyProperties* qf = (VkQueueFamilyProperties*)malloc(sizeof *qf * qn);
    vkGetPhysicalDeviceQueueFamilyProperties(s_phys, &qn, qf);
    s_ts_validbits = (s_qfam < qn) ? (int)qf[s_qfam].timestampValidBits : 0;
    free(qf);
    if (s_ts_period_ns > 0 && s_ts_validbits > 0) {
      VkQueryPoolCreateInfo qpi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
      qpi.queryType = VK_QUERY_TYPE_TIMESTAMP; qpi.queryCount = 2;
      VKC(vkCreateQueryPool(s_dev, &qpi, 0, &s_tspool));
    } }

  create_vram();
  void create_tri_pipeline(void); create_tri_pipeline();
  void panels_init(void); panels_init();
  // Native depth is ALWAYS on now, so the deferred infra (SSAO/light) can ALWAYS be ready -> they toggle
  // live from the overlay with no launch flag (the old PSXPORT_UI gate is obsolete). Skip only under SBS.
  create_shadow();   // shadow-map resources first (its image/view feed the ssao descriptor's binding 2)
  create_ssao();     // deferred post pass (samples color + depth + the shadow map)
  if (!s_headless) create_swapchain();
  // RmlUi mod/debug overlay (windowed only; needs the swapchain + present render pass).
  if (g_mods.ui && !s_headless)
    overlay_glue_init(s_win, s_inst, s_phys, s_qfam, s_dev, s_queue, s_rpass, 2, s_swap_n);
  else fprintf(stderr, "[gpu_vk] headless offscreen render up (VRAM 1024x512 R16_UINT, no swapchain)\n");
  fprintf(stderr, "[gpu_vk] Vulkan present backend up (%ux%u, %u swap images; VRAM 1024x512 R16_UINT)\n",
          s_extent.width, s_extent.height, s_swap_n);
}

// The GPU VRAM image: R16_UINT 1024x512 = PSX VRAM (1555), created once. Render target for the GPU
// rasterizer (M2+), sampled by the present pass, transfer src/dst for upload/readback.
static void create_vram(void) {
  s_tex_undef = 1;
  // MUTABLE_FORMAT + an explicit format list (R16_UINT base + the A1B5G5R5_UNORM blend alias) so the same
  // image carries both views. The format list lets the driver lay out the image valid for both up front.
  static const VkFormat s_vram_fmt_list[2] = { VRAM_FMT, VRAM_BLEND_FMT };
  VkImageFormatListCreateInfo fl = { VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO };
  fl.viewFormatCount = 2; fl.pViewFormats = s_vram_fmt_list;
  VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
  ii.pNext = &fl;
  ii.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
  ii.imageType = VK_IMAGE_TYPE_2D; ii.format = VRAM_FMT;
  ii.extent = (VkExtent3D){ VRAM_W, IMG_H, 1 }; ii.mipLevels = 1; ii.arrayLayers = 1;
  ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
  ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
             VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VKC(vkCreateImage(s_dev, &ii, 0, &s_tex));
  VkMemoryRequirements mr; vkGetImageMemoryRequirements(s_dev, s_tex, &mr);
  VkMemoryAllocateInfo ma = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
  ma.allocationSize = mr.size; ma.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  VKC(vkAllocateMemory(s_dev, &ma, 0, &s_tex_mem));
  VKC(vkBindImageMemory(s_dev, s_tex, s_tex_mem, 0));
  VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
  vi.image = s_tex; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = VRAM_FMT;
  vi.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
  VKC(vkCreateImageView(s_dev, &vi, 0, &s_tex_view));
  // Blendable A1B5G5R5_UNORM alias of the SAME image — the render-pass color attachment.
  VkImageViewCreateInfo vib0 = vi; vib0.format = VRAM_BLEND_FMT;
  VKC(vkCreateImageView(s_dev, &vib0, 0, &s_tex_blend_view));

  s_stage_sz = (VkDeviceSize)VRAM_W * VRAM_H * 2;   // uint16 per texel
  VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
  bi.size = s_stage_sz; bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VKC(vkCreateBuffer(s_dev, &bi, 0, &s_stage));
  VkMemoryRequirements br; vkGetBufferMemoryRequirements(s_dev, s_stage, &br);
  VkMemoryAllocateInfo bm = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
  bm.allocationSize = br.size; bm.memoryTypeIndex = mem_type(br.memoryTypeBits,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  VKC(vkAllocateMemory(s_dev, &bm, 0, &s_stage_mem));
  VKC(vkBindBufferMemory(s_dev, s_stage, s_stage_mem, 0));
  VKC(vkMapMemory(s_dev, s_stage_mem, 0, s_stage_sz, 0, &s_stage_ptr));

  VkDescriptorImageInfo di = { s_sampler, s_tex_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
  VkWriteDescriptorSet wr = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
  wr.dstSet = s_dset; wr.dstBinding = 0; wr.descriptorCount = 1;
  wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr.pImageInfo = &di;
  vkUpdateDescriptorSets(s_dev, 1, &wr, 0, 0);

  // PSXPORT_SBS second image (same format/usage as s_tex) + its present descriptor.
  VKC(vkCreateImage(s_dev, &ii, 0, &s_tex_b));
  VkMemoryRequirements mrb; vkGetImageMemoryRequirements(s_dev, s_tex_b, &mrb);
  VkMemoryAllocateInfo mab = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
  mab.allocationSize = mrb.size; mab.memoryTypeIndex = mem_type(mrb.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  VKC(vkAllocateMemory(s_dev, &mab, 0, &s_tex_b_mem));
  VKC(vkBindImageMemory(s_dev, s_tex_b, s_tex_b_mem, 0));
  VkImageViewCreateInfo vib = vi; vib.image = s_tex_b;
  VKC(vkCreateImageView(s_dev, &vib, 0, &s_tex_b_view));
  VkImageViewCreateInfo vibb = vib; vibb.format = VRAM_BLEND_FMT;   // blendable alias for panel B's attachment
  VKC(vkCreateImageView(s_dev, &vibb, 0, &s_tex_b_blend_view));
  VkDescriptorImageInfo dib = { s_sampler, s_tex_b_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
  VkWriteDescriptorSet wrb = wr; wrb.dstSet = s_dset_b; wrb.pImageInfo = &dib;
  vkUpdateDescriptorSets(s_dev, 1, &wrb, 0, 0);

  // VRAM snapshot image (texture source for the textured pipeline; copied from VRAM before a batch).
  // Sampled-only R16_UINT: no blend alias needed, so drop the mutable flag / format list.
  VkImageCreateInfo ti = ii; ti.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  ti.flags = 0; ti.pNext = 0;
  VKC(vkCreateImage(s_dev, &ti, 0, &s_vram_tex));
  VkMemoryRequirements tr; vkGetImageMemoryRequirements(s_dev, s_vram_tex, &tr);
  VkMemoryAllocateInfo tm = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
  tm.allocationSize = tr.size; tm.memoryTypeIndex = mem_type(tr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  VKC(vkAllocateMemory(s_dev, &tm, 0, &s_vram_tex_mem));
  VKC(vkBindImageMemory(s_dev, s_vram_tex, s_vram_tex_mem, 0));
  VkImageViewCreateInfo tv = vi; tv.image = s_vram_tex;
  VKC(vkCreateImageView(s_dev, &tv, 0, &s_vram_tex_view));
  s_vram_tex_undef = 1;
  VkDescriptorImageInfo tdi = { s_sampler, s_vram_tex_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
  VkWriteDescriptorSet twr = wr; twr.dstSet = s_dset_tex; twr.pImageInfo = &tdi;
  vkUpdateDescriptorSets(s_dev, 1, &twr, 0, 0);
}

static void img_barrier(VkImageLayout from, VkImageLayout to, VkPipelineStageFlags ss, VkPipelineStageFlags ds,
                        VkAccessFlags sa, VkAccessFlags da) {
  VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
  b.oldLayout = from; b.newLayout = to; b.image = s_tex;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
  b.srcAccessMask = sa; b.dstAccessMask = da;
  vkCmdPipelineBarrier(s_cmd, ss, ds, 0, 0, 0, 0, 0, 1, &b);
}

static void poll_quit(void) {
  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    overlay_glue_event(&e);    // overlay mouse/keys (ESC toggles the RmlUi menu); no-op if not inited
    if (e.type == SDL_QUIT) exit(0);
    // ESC no longer quits the game — it toggles the RmlUi menu (handled in imgui_overlay_event). The
    // actual quit now lives in the "Quit game" button inside that menu (on_quit -> exit(0)).
    // Window resized: flag the swapchain for recreation. Many drivers (notably Wayland) do NOT report
    // SUBOPTIMAL/OUT_OF_DATE on resize, so relying on the present result alone leaves s_extent stale —
    // the window grows but the swapchain (and so win_w()/win_h()) don't, and ImGui (whose DisplaySize
    // tracks the live SDL window) then maps the cursor into the wrong, stale render extent. Handle the
    // SDL resize explicitly. Ignore 0-size (minimized) so we never build a zero-extent swapchain.
    if (e.type == SDL_WINDOWEVENT &&
        (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED || e.window.event == SDL_WINDOWEVENT_RESIZED) &&
        e.window.data1 > 0 && e.window.data2 > 0)
      s_swap_dirty = 1;
  }
}

// Push the vertex-stage wide/supersample transform (VPC). `enabled` lets diagnostic passes force the
// identity (non-wide) transform while still satisfying the shader's img_h dependency.
static void push_wide(int enabled) {
  // ss = internal-res scale. fb_x0 CENTERS the native 320-wide view in the (wider) FB: the world projects
  // about the faithful OFX center (160), so without this the relocation (fx = local.x*ss + fb_x0) left-
  // anchored the world and the extra wide FOV only appeared on the RIGHT. fb_x0 = margin*ss places the
  // native band centered, so the widened cull's extra geometry fills BOTH side margins symmetrically.
  // 4:3 -> margin 0 -> fb_x0 0 (byte-identical). wb.x stays reserved (0).
  int margin = (wide_native_w() - 320) / 2;              // half the extra wide width, native px
  int32_t va[8] = { enabled, FB_Y0, s_ires, IMG_H,   0 /*reserved*/, FBW(), FBH(), margin * s_ires };
  vkCmdPushConstants(s_cmd, s_pll, VK_SHADER_STAGE_VERTEX_BIT, 16, 32, va);
}

// Render this frame's tee'd geometry over the freshly uploaded VRAM in `tgt`/`fb`. `native` picks the
// depth channel (0 = default OT .ord, 1 = native .ordn) for the PSXPORT_SBS split. `tgt` must enter in
// TRANSFER_DST (uploaded VRAM) and leaves in SHADER_READ_ONLY (ready to sample). Shares depth + s_vram_tex.
// Bind each Panel's per-target resources. Panel 0 wraps the primary VRAM image (also used by the
// readback / self-test paths); Panel 1 is the SBS native-depth target. Called once after the images,
// framebuffers and descriptors exist.
void panels_init(void) {
  s_panels[0] = (Panel){ s_tex,   s_vram_fb,   s_dset,   s_depth,   1, 1, 0 };
  s_panels[1] = (Panel){ s_tex_b, s_vram_fb_b, s_dset_b, s_depth_b, 1, 1, 1 };
}

// Mirror this frame's SW-written VRAM (s_stage) into a Panel's PERSISTENT color image: a full copy on
// first use, then only the dirty regions (uploads/copies/fills) — the rendered framebuffer stays owned
// by the panel across frames. Each panel uploads independently so its persistent framebuffer is its own.
void GpuVkState::panel_upload(Panel* p) {
  img_barrier_on(p->color, p->color_undef ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
  if (p->color_undef) {
    VkBufferImageCopy bc = {0};
    bc.imageSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    bc.imageExtent = (VkExtent3D){ VRAM_W, VRAM_H, 1 };
    vkCmdCopyBufferToImage(s_cmd, s_stage, p->color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
    if (s_vkprof > 0 && p == &s_panels[0]) s_vp_full_uploads++;   // vkprof: a FULL 1MB image upload happened
  } else {
    if (s_vkprof > 0 && p == &s_panels[0]) {   // vkprof: per-frame dirty-region upload volume (panel 0 only)
      s_vp_dirty_rects += s_dirty_n;
      long px = 0; for (int d = 0; d < s_dirty_n; d++) px += (long)s_dirty[d].w * s_dirty[d].h;
      s_vp_dirty_kpx += px / 1000;
    }
    for (int d = 0; d < s_dirty_n; d++) {
      VkBufferImageCopy r = {0};
      r.bufferOffset = ((VkDeviceSize)s_dirty[d].y * VRAM_W + s_dirty[d].x) * 2;
      r.bufferRowLength = VRAM_W;
      r.imageSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
      r.imageOffset = (VkOffset3D){ s_dirty[d].x, s_dirty[d].y, 0 };
      r.imageExtent = (VkExtent3D){ (uint32_t)s_dirty[d].w, (uint32_t)s_dirty[d].h, 1 };
      vkCmdCopyBufferToImage(s_cmd, s_stage, p->color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    }
  }
  p->color_undef = 0;
}

// Render this frame's tee'd geometry into one Panel (over its freshly uploaded persistent VRAM). The
// panel's `native` picks the depth-channel pipeline variant. `p->color` enters in TRANSFER_DST (just
// uploaded) and leaves in SHADER_READ_ONLY (ready to sample). The texture/blend snapshot (s_vram_tex)
// is shared scratch, serialized by its layout barriers.
void GpuVkState::panel_render(Panel* p) {
  VkImage tgt = p->color, depth = p->depth; VkFramebuffer fb = p->fb; int native = p->native;
  VkRenderPassBeginInfo grp = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
  grp.renderPass = s_vram_rpass; grp.framebuffer = fb; grp.renderArea.extent = (VkExtent2D){ VRAM_W, IMG_H };
  VkViewport gv = { 0, 0, VRAM_W, IMG_H, 0.0f, 1.0f }; VkRect2D gs = { {0,0}, { VRAM_W, IMG_H } };
  VkDeviceSize go = 0;
  VkImageCopy ic = { { VK_IMAGE_ASPECT_COLOR_BIT,0,0,1 }, {0,0,0}, { VK_IMAGE_ASPECT_COLOR_BIT,0,0,1 }, {0,0,0}, { VRAM_W, IMG_H, 1 } };
  VkBufferImageCopy bc = {0};
  bc.imageSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
  bc.imageExtent = (VkExtent3D){ VRAM_W, VRAM_H, 1 };
  if (s_tri_n || s_tex_n || s_semi_n) {
    // snapshot the uploaded VRAM -> vram_tex (the textures) for the opaque pass
    img_barrier_on(s_vram_tex, s_vram_tex_undef ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    s_vram_tex_undef = 0;
    vkCmdCopyBufferToImage(s_cmd, s_stage, s_vram_tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
    img_barrier_on(s_vram_tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                   VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    img_barrier_on(tgt, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    if (p->depth_undef) {   // first use: UNDEFINED -> DEPTH_STENCIL_ATTACHMENT_OPTIMAL (stays there)
      VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
      b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
      b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.image = depth;
      b.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
      b.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
      vkCmdPipelineBarrier(s_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                           0, 0, 0, 0, 0, 1, &b);
      p->depth_undef = 0;
    }
    // OPAQUE pass
    vkCmdBeginRenderPass(s_cmd, &grp, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdSetViewport(s_cmd, 0, 1, &gv); vkCmdSetScissor(s_cmd, 0, 1, &gs);
    // Clear the depth buffer each frame; the render pass LOADs it so opaque->semi share it.
    { VkClearAttachment dca = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, {{{0}}} }; dca.clearValue.depthStencil.depth = 0.0f;
      VkClearRect dcr = { { { 0, 0 }, { VRAM_W, IMG_H } }, 0, 1 };
      vkCmdClearAttachments(s_cmd, 1, &dca, 1, &dcr); }
    if (frame_via_fb()) {   // clear the scratch FB (the game's clear/fill targets VRAM, not the FB) before drawing
      VkClearAttachment ca = { VK_IMAGE_ASPECT_COLOR_BIT, 0, {{{0,0,0,1}}} };
      VkClearRect cr = { { { 0, FB_Y0 }, { (uint32_t)FBW(), (uint32_t)FBH() } }, 0, 1 };
      vkCmdClearAttachments(s_cmd, 1, &ca, 1, &cr);
    }
    push_wide(frame_via_fb());   // relocate into the FB only for 3D frames; 2D screens stay 1:1 in VRAM
    vkCmdBindDescriptorSets(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pll, 0, 1, &s_dset_tex, 0, 0);
    if (s_tri_n) { vkCmdBindPipeline(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_tri_pipe[native]);
                   vkCmdBindVertexBuffers(s_cmd, 0, 1, &s_vbuf, &go); vkCmdDraw(s_cmd, s_tri_n, 1, 0, 0); }
    if (s_tex_n) { vkCmdBindPipeline(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_tritex_pipe[native]);
                   vkCmdBindVertexBuffers(s_cmd, 0, 1, &s_tvbuf, &go); vkCmdDraw(s_cmd, s_tex_n, 1, 0, 0); }
    // SEMI pass — PC-native HARDWARE BLEND. Fixed-function blend reads the LIVE color attachment as the
    // dst per fragment, so overlapping/stacked translucent prims accumulate correctly in submission order
    // with NO framebuffer snapshot (this is exactly what the old per-group vkCmdCopyImage scheme emulated;
    // HW blend makes it free). Same render pass / depth buffer as the opaque pass (depth-test, no write).
    // We draw the semi batch as contiguous RUNS of constant PSX blend mode (read from the host buffer's
    // per-vertex clut[3]), binding that mode's HW-blend pipeline + its blend constant per run. Within a run
    // submission order is preserved; blend always composites over what's already there. Textures still come
    // from the pre-opaque s_vram_tex snapshot (the texel SOURCE); the blend DST is the attachment itself.
    if (s_semi_n) {
      vkCmdBindDescriptorSets(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pll, 0, 1, &s_dset_tex, 0, 0);
      vkCmdBindVertexBuffers(s_cmd, 0, 1, &s_semibuf, &go);
      const TexVtx* sv = (const TexVtx*)s_semibuf_ptr;
      int i = 0;
      while (i < s_semi_n) {
        int m = sv[i].clut[3] & 3;                       // PSX blend mode of this prim (per-vertex, uniform/prim)
        int j = i + 3;
        while (j < s_semi_n && (sv[j].clut[3] & 3) == m) j += 3;   // extend the run while the mode matches
        // 1) OPAQUE sub-pass: a textured-semi texel with STP=0 draws OPAQUE (mode-independent), so one
        //    non-blend draw over the run handles them (the shader discards the blending texels).
        vkCmdBindPipeline(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_tritex_semi_op_pipe[native]);
        vkCmdDraw(s_cmd, j - i, 1, i, 0);
        // 2) BLEND sub-pass: STP=1 / untextured texels blend per the run's PSX mode via HW fixed-function.
        float bc[4] = { 0,0,0,1 };
        if (m == 0)      { bc[0]=bc[1]=bc[2]=0.5f; }     // avg:  .5 src + .5 dst
        else if (m == 3) { bc[0]=bc[1]=bc[2]=0.25f; }    // add4: .25 src + 1 dst
        vkCmdSetBlendConstants(s_cmd, bc);               // add(1)/sub(2): constants unused but set harmlessly
        vkCmdBindPipeline(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_tritex_semi_pipe[m][native]);
        vkCmdDraw(s_cmd, j - i, 1, i, 0);
        i = j;
      }
    }
    vkCmdEndRenderPass(s_cmd);
    // AO runs AFTER the translucent composite (#13 fix): darkening the OPAQUE buffer before the semi pass
    // poisoned the framebuffer the water samples (the puddle is a depth concavity -> max AO -> near-black
    // ground -> the blended water read as invisible). Applied to the FINAL composited frame instead, every
    // 3D-world pixel (water included) is AO-shaded uniformly from the opaque depth, so translucent surfaces
    // keep their hue and stay visible. s_tex is in COLOR_ATTACHMENT here (semi loop / opaque pass left it
    // there); ssao_pass round-trips it through shader-read and returns it to COLOR_ATTACHMENT.
    if (cfg_dbg("fps60shadow")) fprintf(stderr, "[fps60shadow] panel_render tgt==s_tex=%d shadows_on=%d deferred_on=%d s_shadow_n=%d\n",
                         tgt == s_tex, shadows_on(), deferred_on(), s_shadow_n);
    if (shadows_on() && tgt == s_tex) shadow_pass();   // light-view depth map (sampled by the deferred pass)
    if (deferred_on() && tgt == s_tex) ssao_pass();
    img_barrier_on(tgt, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
  } else {
    img_barrier_on(tgt, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
  }
}

// Build the PSXPORT_SSAO resources: a second R16_UINT image (the post-pass target), a color-only render
// pass over it, a 2-binding descriptor (color = s_tex, depth = s_depth), the pipeline (present.vert
// fullscreen tri + ssao.frag), and the framebuffer. Created once when ssao_on().
static void create_ssao(void) {
  // s_ssao_img: same R16_UINT geometry-buffer format; we render the AO'd color into it then copy back.
  VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
  ii.imageType = VK_IMAGE_TYPE_2D; ii.format = VK_FORMAT_R16_UINT;
  ii.extent = (VkExtent3D){ VRAM_W, IMG_H, 1 }; ii.mipLevels = 1; ii.arrayLayers = 1;
  ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
  ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VKC(vkCreateImage(s_dev, &ii, 0, &s_ssao_img));
  VkMemoryRequirements mr; vkGetImageMemoryRequirements(s_dev, s_ssao_img, &mr);
  VkMemoryAllocateInfo ma = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
  ma.allocationSize = mr.size; ma.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  VKC(vkAllocateMemory(s_dev, &ma, 0, &s_ssao_mem));
  VKC(vkBindImageMemory(s_dev, s_ssao_img, s_ssao_mem, 0));
  VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
  vi.image = s_ssao_img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = VK_FORMAT_R16_UINT;
  vi.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
  VKC(vkCreateImageView(s_dev, &vi, 0, &s_ssao_view));

  // render pass: write every pixel (DONT_CARE load), end in TRANSFER_SRC for the copy-back.
  VkAttachmentDescription at = {0};
  at.format = VK_FORMAT_R16_UINT; at.samples = VK_SAMPLE_COUNT_1_BIT;
  at.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; at.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  at.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; at.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  at.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; at.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  VkAttachmentReference ar = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
  VkSubpassDescription sp = {0}; sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  sp.colorAttachmentCount = 1; sp.pColorAttachments = &ar;
  VkSubpassDependency dep[2] = {{0},{0}};
  dep[0].srcSubpass = VK_SUBPASS_EXTERNAL; dep[0].dstSubpass = 0;        // wait for the sampled inputs
  dep[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT; dep[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dep[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT; dep[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  dep[1].srcSubpass = 0; dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;        // make the copy-back wait for the write
  dep[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT; dep[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
  dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; dep[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  VkRenderPassCreateInfo rpi = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
  rpi.attachmentCount = 1; rpi.pAttachments = &at; rpi.subpassCount = 1; rpi.pSubpasses = &sp;
  rpi.dependencyCount = 2; rpi.pDependencies = dep;
  VKC(vkCreateRenderPass(s_dev, &rpi, 0, &s_ssao_rpass));

  VkFramebufferCreateInfo fi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
  fi.renderPass = s_ssao_rpass; fi.attachmentCount = 1; fi.pAttachments = &s_ssao_view;
  fi.width = VRAM_W; fi.height = IMG_H; fi.layers = 1;
  VKC(vkCreateFramebuffer(s_dev, &fi, 0, &s_ssao_fb));

  // descriptor set layout: binding0 = color (usampler2D), binding1 = depth (sampler2D), binding2 = shadow
  // map (sampler2D), all fragment.
  VkDescriptorSetLayoutBinding b[3] = {{0},{0},{0}};
  for (int i = 0; i < 3; i++) { b[i].binding = i; b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                                b[i].descriptorCount = 1; b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT; }
  VkDescriptorSetLayoutCreateInfo dli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
  dli.bindingCount = 3; dli.pBindings = b;
  VKC(vkCreateDescriptorSetLayout(s_dev, &dli, 0, &s_ssao_dsl));
  // p0..p6 (7 × 16B = 112) + light view-projection mat4 (64B) = 176B for AO + LIGHT + SHADOW params.
  VkPushConstantRange pcr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 176 };
  VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
  pli.setLayoutCount = 1; pli.pSetLayouts = &s_ssao_dsl; pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pcr;
  VKC(vkCreatePipelineLayout(s_dev, &pli, 0, &s_ssao_pll));

  VkDescriptorPoolSize psz = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3 };
  VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
  dpi.maxSets = 1; dpi.poolSizeCount = 1; dpi.pPoolSizes = &psz;
  VKC(vkCreateDescriptorPool(s_dev, &dpi, 0, &s_ssao_dpool));
  VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
  dai.descriptorPool = s_ssao_dpool; dai.descriptorSetCount = 1; dai.pSetLayouts = &s_ssao_dsl;
  VKC(vkAllocateDescriptorSets(s_dev, &dai, &s_ssao_dset));
  VkDescriptorImageInfo cdi = { s_sampler, s_tex_view,    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
  VkDescriptorImageInfo ddi = { s_sampler, s_depth_view,  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
  VkDescriptorImageInfo sdi = { s_sampler, s_shadow_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
  VkWriteDescriptorSet wr[3] = {{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET },{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET },{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET }};
  wr[0].dstSet = s_ssao_dset; wr[0].dstBinding = 0; wr[0].descriptorCount = 1; wr[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr[0].pImageInfo = &cdi;
  wr[1].dstSet = s_ssao_dset; wr[1].dstBinding = 1; wr[1].descriptorCount = 1; wr[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr[1].pImageInfo = &ddi;
  wr[2].dstSet = s_ssao_dset; wr[2].dstBinding = 2; wr[2].descriptorCount = 1; wr[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr[2].pImageInfo = &sdi;
  vkUpdateDescriptorSets(s_dev, 3, wr, 0, 0);

  VkShaderModule vs = make_shader(spv_present_vert, spv_present_vert_len);   // fullscreen tri (no vtx input)
  VkShaderModule fs = make_shader(spv_ssao_frag, spv_ssao_frag_len);
  VkPipelineShaderStageCreateInfo st[2] = {
    { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", 0 },
    { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", 0 },
  };
  VkPipelineVertexInputStateCreateInfo vin = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
  VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
  ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
  vp.viewportCount = 1; vp.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
  rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState cba = {0}; cba.colorWriteMask = 0xF;
  VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
  cb.attachmentCount = 1; cb.pAttachments = &cba;
  VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
  VkPipelineDynamicStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
  ds.dynamicStateCount = 2; ds.pDynamicStates = dyn;
  VkGraphicsPipelineCreateInfo gp = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
  gp.stageCount = 2; gp.pStages = st; gp.pVertexInputState = &vin; gp.pInputAssemblyState = &ia;
  gp.pViewportState = &vp; gp.pRasterizationState = &rs; gp.pMultisampleState = &ms;
  gp.pColorBlendState = &cb; gp.pDynamicState = &ds; gp.layout = s_ssao_pll; gp.renderPass = s_ssao_rpass;
  VKC(vkCreateGraphicsPipelines(s_dev, VK_NULL_HANDLE, 1, &gp, 0, &s_ssao_pipe));
  vkDestroyShaderModule(s_dev, vs, 0); vkDestroyShaderModule(s_dev, fs, 0);
  fprintf(stderr, "[gpu_vk] PSXPORT_SSAO post pass up (R16_UINT %ux%u, 2-tap-ring depth AO)\n", VRAM_W, IMG_H);
}

// Build the dynamic shadow-map resources: a D32 depth image (the shadow map), a depth-only render pass +
// framebuffer over it, a depth-only pipeline (shadow.vert transforms captured VIEW-SPACE world verts by the
// light view-projection push constant; shadow.frag is empty), and a host-visible vertex buffer for the
// per-frame captured geometry. Built once; the pass runs only when shadows_on(). The map's render pass ends
// in SHADER_READ_ONLY so the deferred pass samples it directly.
static void create_shadow(void) {
  // D32 depth image, usable both as a depth attachment (shadow_pass) and as a sampled texture (ssao.frag).
  VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
  ii.imageType = VK_IMAGE_TYPE_2D; ii.format = VK_FORMAT_D32_SFLOAT;
  ii.extent = (VkExtent3D){ SHADOW_DIM, SHADOW_DIM, 1 }; ii.mipLevels = 1; ii.arrayLayers = 1;
  ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
  ii.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VKC(vkCreateImage(s_dev, &ii, 0, &s_shadow_img));
  VkMemoryRequirements mr; vkGetImageMemoryRequirements(s_dev, s_shadow_img, &mr);
  VkMemoryAllocateInfo ma = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
  ma.allocationSize = mr.size; ma.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  VKC(vkAllocateMemory(s_dev, &ma, 0, &s_shadow_mem));
  VKC(vkBindImageMemory(s_dev, s_shadow_img, s_shadow_mem, 0));
  VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
  vi.image = s_shadow_img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = VK_FORMAT_D32_SFLOAT;
  vi.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
  VKC(vkCreateImageView(s_dev, &vi, 0, &s_shadow_view));

  // depth-only render pass: clear -> store -> finalLayout SHADER_READ_ONLY (sampled by the deferred pass).
  VkAttachmentDescription at = {0};
  at.format = VK_FORMAT_D32_SFLOAT; at.samples = VK_SAMPLE_COUNT_1_BIT;
  at.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; at.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  at.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; at.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  at.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; at.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  VkAttachmentReference ar = { 0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
  VkSubpassDescription sp = {0}; sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  sp.pDepthStencilAttachment = &ar;
  VkSubpassDependency dep[2] = {{0},{0}};
  dep[0].srcSubpass = VK_SUBPASS_EXTERNAL; dep[0].dstSubpass = 0;   // prior frame's sample -> this write
  dep[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT; dep[0].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
  dep[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT; dep[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  dep[1].srcSubpass = 0; dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;   // this write -> the deferred sample
  dep[1].srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT; dep[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  dep[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT; dep[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  VkRenderPassCreateInfo rpi = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
  rpi.attachmentCount = 1; rpi.pAttachments = &at; rpi.subpassCount = 1; rpi.pSubpasses = &sp;
  rpi.dependencyCount = 2; rpi.pDependencies = dep;
  VKC(vkCreateRenderPass(s_dev, &rpi, 0, &s_shadow_rpass));

  VkFramebufferCreateInfo fi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
  fi.renderPass = s_shadow_rpass; fi.attachmentCount = 1; fi.pAttachments = &s_shadow_view;
  fi.width = SHADOW_DIM; fi.height = SHADOW_DIM; fi.layers = 1;
  VKC(vkCreateFramebuffer(s_dev, &fi, 0, &s_shadow_fb));

  // pipeline layout: one push constant = the light view-projection mat4 (64B), vertex stage.
  VkPushConstantRange pcr = { VK_SHADER_STAGE_VERTEX_BIT, 0, 64 };
  VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
  pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pcr;
  VKC(vkCreatePipelineLayout(s_dev, &pli, 0, &s_shadow_pll));

  VkShaderModule vs = make_shader(spv_shadow_vert, spv_shadow_vert_len);
  VkShaderModule fs = make_shader(spv_shadow_frag, spv_shadow_frag_len);
  VkPipelineShaderStageCreateInfo st[2] = {
    { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", 0 },
    { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", 0 },
  };
  VkVertexInputBindingDescription vbd = { 0, sizeof(ShadowVtx), VK_VERTEX_INPUT_RATE_VERTEX };
  VkVertexInputAttributeDescription vad = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 };   // in_vpos
  VkPipelineVertexInputStateCreateInfo vin = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
  vin.vertexBindingDescriptionCount = 1; vin.pVertexBindingDescriptions = &vbd;
  vin.vertexAttributeDescriptionCount = 1; vin.pVertexAttributeDescriptions = &vad;
  VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
  ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
  vp.viewportCount = 1; vp.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
  rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.0f;
  // A small depth slope bias in the raster helps peter-panning/acne on top of the shader's constant bias.
  rs.depthBiasEnable = VK_TRUE; rs.depthBiasConstantFactor = 1.5f; rs.depthBiasSlopeFactor = 2.0f;
  VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineDepthStencilStateCreateInfo dss = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
  dss.depthTestEnable = VK_TRUE; dss.depthWriteEnable = VK_TRUE; dss.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
  VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
  cb.attachmentCount = 0;   // depth-only, no color attachment
  VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
  VkPipelineDynamicStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
  ds.dynamicStateCount = 2; ds.pDynamicStates = dyn;
  VkGraphicsPipelineCreateInfo gp = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
  gp.stageCount = 2; gp.pStages = st; gp.pVertexInputState = &vin; gp.pInputAssemblyState = &ia;
  gp.pViewportState = &vp; gp.pRasterizationState = &rs; gp.pMultisampleState = &ms;
  gp.pDepthStencilState = &dss; gp.pColorBlendState = &cb; gp.pDynamicState = &ds;
  gp.layout = s_shadow_pll; gp.renderPass = s_shadow_rpass;
  VKC(vkCreateGraphicsPipelines(s_dev, VK_NULL_HANDLE, 1, &gp, 0, &s_shadow_pipe));
  vkDestroyShaderModule(s_dev, vs, 0); vkDestroyShaderModule(s_dev, fs, 0);

  make_hostbuf(sizeof(ShadowVtx) * SHADOW_VTX_CAP, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
               &s_shadow_vbuf, &s_shadow_vbuf_mem, &s_shadow_vbuf_ptr);
  fprintf(stderr, "[gpu_vk] dynamic shadow map up (D32 %ux%u, captured view-space geometry)\n", SHADOW_DIM, SHADOW_DIM);
}

// Build the light view-projection (column-major, 16 floats) for this frame from the directional light dir
// (view space) + an orthographic volume covering the visible scene. The light looks ALONG -light_dir
// (light_dir is the to-light vector); we frame a fixed cube around the view origin (the camera looks down
// +Z in view space, so the scene sits ahead). v1: a fixed half-extent — robust for Tomba's side-scroll
// field; a scene-fitted volume is a follow-up. Returns the matrix in `out16` and the world half-size used.
static void build_light_vp(float out16[16]) {
  // light forward (view-space) = -to_light. Build an orthonormal basis (fwd, right, up).
  float lx = g_mods.light_dir[0], ly = g_mods.light_dir[1], lz = g_mods.light_dir[2];
  float ll = sqrtf(lx*lx + ly*ly + lz*lz); if (ll < 1e-6f) { lx = -0.4f; ly = -0.7f; lz = -0.5f; ll = sqrtf(lx*lx+ly*ly+lz*lz); }
  float fx = -lx/ll, fy = -ly/ll, fz = -lz/ll;                 // forward = along -to_light
  float ux = 0.0f, uy = 1.0f, uz = 0.0f;                       // world-ish up (view +Y is down, but only used to seed the basis)
  if (fabsf(fy) > 0.95f) { ux = 1.0f; uy = 0.0f; uz = 0.0f; }  // avoid degeneracy when the light is near-vertical
  // right = normalize(cross(up, fwd)); up = cross(fwd, right)
  float rx = uy*fz - uz*fy, ry = uz*fx - ux*fz, rz = ux*fy - uy*fx;
  float rl = sqrtf(rx*rx+ry*ry+rz*rz);
  if (rl < 1e-4f) { rx = 1.0f; ry = 0.0f; rz = 0.0f; rl = 1.0f; }   // up nearly ‖ fwd -> safe right (no NaN)
  rx/=rl; ry/=rl; rz/=rl;
  ux = fy*rz - fz*ry; uy = fz*rx - fx*rz; uz = fx*ry - fy*rx;  // already unit (fwd,right unit & orthogonal)
  // The visible field sits ahead of the camera in view space. Frame a box around a center a bit into the
  // scene; HALF = ortho half-extent in world units, DEPTH the near..far span along the light.
  const float HALF = 6000.0f, DEPTH = 12000.0f;
  float cxv = 0.0f, cyv = 0.0f, czv = 4000.0f;                 // view-space center of the shadowed volume
  // light-space view matrix rows (R = [right; up; fwd]); eye = center - fwd*DEPTH*0.5 so the box is centered.
  float ex = cxv - fx*DEPTH*0.5f, ey = cyv - fy*DEPTH*0.5f, ez = czv - fz*DEPTH*0.5f;
  // view matrix (world/view-space point -> light space): L = R * (p - eye)
  float tx = -(rx*ex + ry*ey + rz*ez);
  float ty = -(ux*ex + uy*ey + uz*ez);
  float tz = -(fx*ex + fy*ey + fz*ez);
  // ortho: x,y -> [-1,1] over [-HALF,HALF]; z -> [0,1] over [0,DEPTH] (Vulkan clip z in [0,1]).
  float sx = 1.0f/HALF, sy = 1.0f/HALF, sz = 1.0f/DEPTH;
  // Compose P*V into a column-major mat4. Row r of (P*V) = scale[r] * Vrow[r] (+ z offset handled in tz term).
  // V rows: [rx ry rz tx; ux uy uz ty; fx fy fz tz; 0 0 0 1].
  float m[4][4];
  m[0][0]=sx*rx; m[0][1]=sx*ry; m[0][2]=sx*rz; m[0][3]=sx*tx;
  m[1][0]=sy*ux; m[1][1]=sy*uy; m[1][2]=sy*uz; m[1][3]=sy*ty;
  m[2][0]=sz*fx; m[2][1]=sz*fy; m[2][2]=sz*fz; m[2][3]=sz*tz;
  m[3][0]=0;     m[3][1]=0;     m[3][2]=0;     m[3][3]=1;
  // GLSL mat4 is column-major: out16[col*4 + row] = m[row][col].
  for (int col=0; col<4; col++) for (int row=0; row<4; row++) out16[col*4+row] = m[row][col];
}

// Rasterize the captured view-space world geometry from the light's view into the shadow depth map. Runs
// BEFORE the deferred ssao_pass (which samples the map). No-op when nothing was captured. Builds the light
// view-projection for this frame (also stashed in s_shadow_lvp for the deferred sample).
void GpuVkState::shadow_pass() {
  build_light_vp(s_shadow_lvp); s_shadow_lvp_valid = 1;
  // PSXPORT_DEBUG=fps60shadow — mechanical strobe check: report the captured shadow-vertex count the
  // shadow map is built from on EACH present-pass. With fps60 ON this fires twice per logic frame (the
  // interpolated in-between, then the real frame); both MUST show the same non-zero count (= the shadow
  // is rasterized identically on both displayed frames -> no 30Hz strobe). A second-pass count of 0 means
  // the interp pass consumed/zeroed the stream (the bug this fix addresses).
  if (cfg_dbg("fps60shadow")) fprintf(stderr, "[fps60shadow] shadow_pass: s_shadow_n=%d (verts)\n", s_shadow_n);
  // ALWAYS run the pass (even with no captured geometry): the render pass CLEARs the map to far=1.0 and its
  // finalLayout transitions s_shadow_img to SHADER_READ_ONLY. Skipping it would leave the image UNDEFINED
  // while the deferred pass (do_shadow set) still samples binding 2 -> validation error / garbage. A cleared
  // (all-far) map reads back as "everything lit", which is the correct result when nothing casts.
  VkRenderPassBeginInfo rp = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
  rp.renderPass = s_shadow_rpass; rp.framebuffer = s_shadow_fb;
  rp.renderArea.extent = (VkExtent2D){ SHADOW_DIM, SHADOW_DIM };
  VkClearValue cv = {}; cv.depthStencil.depth = 1.0f;   // far = 1.0 (LESS_OR_EQUAL test, closer wins)
  rp.clearValueCount = 1; rp.pClearValues = &cv;
  vkCmdBeginRenderPass(s_cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
  if (s_shadow_n) {
    VkViewport vpt = { 0, 0, SHADOW_DIM, SHADOW_DIM, 0.0f, 1.0f }; VkRect2D sc = { {0,0}, { SHADOW_DIM, SHADOW_DIM } };
    vkCmdSetViewport(s_cmd, 0, 1, &vpt); vkCmdSetScissor(s_cmd, 0, 1, &sc);
    vkCmdPushConstants(s_cmd, s_shadow_pll, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, s_shadow_lvp);
    vkCmdBindPipeline(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_shadow_pipe);
    VkDeviceSize off = 0; vkCmdBindVertexBuffers(s_cmd, 0, 1, &s_shadow_vbuf, &off);
    vkCmdDraw(s_cmd, s_shadow_n, 1, 0, 0);
  }
  vkCmdEndRenderPass(s_cmd);   // s_shadow_img -> SHADER_READ_ONLY (render pass finalLayout)
  s_shadow_undef = 0;
}

// Depth-aspect image barrier (img_barrier_on is color-aspect only).
static void depth_barrier(VkImageLayout from, VkImageLayout to, VkPipelineStageFlags ss,
                          VkPipelineStageFlags ds, VkAccessFlags sa, VkAccessFlags da) {
  VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
  b.oldLayout = from; b.newLayout = to; b.image = s_depth;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
  b.srcAccessMask = sa; b.dstAccessMask = da;
  vkCmdPipelineBarrier(s_cmd, ss, ds, 0, 0, 0, 0, 0, 1, &b);
}

// DEFERRED shading pass (PSXPORT_SSAO and/or PSXPORT_LIGHT), recorded into s_cmd by panel_render BETWEEN
// the opaque and semi passes — AO + lighting are properties of the OPAQUE geometry; the translucent pass
// (UI/water) must composite OVER the shaded world, not be shaded by it (the menu's semi fill writes no
// depth, so the depth buffer under it is the 3D terrain — shading after it would wrongly darken the UI).
// Entry: s_tex in COLOR_ATTACHMENT, s_depth in DEPTH_STENCIL_ATTACHMENT. Reads color+depth, writes the
// shaded color into s_ssao_img, copies it back into s_tex. Exit: s_tex back in COLOR_ATTACHMENT, s_depth
// back in DEPTH_STENCIL (for the semi pass).
void GpuVkState::ssao_pass() {
  // s_tex: color attachment (opaque output) -> shader-read (sample as the AO source)
  img_barrier_on(s_tex, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
  // depth: attachment -> shader-read (sample it in the AO pass)
  depth_barrier(VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
  // push constants: depth-linearize + AO + directional-light params (env-tunable; logged once).
  float nearp = proj_near_pz(), H = proj_plane_h();
  float inv_near = 1.0f / (nearp < 1.0f ? 1.0f : nearp), inv_far = 1.0f / 65535.0f;
  float cx, cy; proj_screen_center(&cx, &cy);
  // Params are LIVE from g_mods (the overlay tunes them at runtime). Radius scales with internal-res so a
  // pixel radius covers a consistent world area. Light dir = the TO-LIGHT vector in VIEW space (x right,
  // y DOWN, z into screen). Log once, after the engine has set a real projection plane (early frames have
  // H/cx/cy at defaults; logging then would mislead).
  float strength = g_mods.ssao_strength, radius = g_mods.ssao_radius * (float)s_ires;
  float bias_frac = g_mods.ssao_bias, range_frac = g_mods.ssao_range;
  float lx = g_mods.light_dir[0], ly = g_mods.light_dir[1], lz = g_mods.light_dir[2];
  float ambient = g_mods.light_ambient, diffuse = g_mods.light_diffuse;
  static int s_logged = 0;
  if (!s_logged && H > 1.5f) {
    fprintf(stderr, "[deferred] ssao=%d light=%d | ssao strength=%.2f radius=%.1fpx bias=%.3f range=%.3f"
            " | light dir=(%.2f,%.2f,%.2f) amb=%.2f diff=%.2f | near pz=%.1f H=%.0f cx=%.1f cy=%.1f\n",
            ssao_on(), light_on(), strength, radius, bias_frac, range_frac, lx, ly, lz, ambient, diffuse,
            nearp, H, cx, cy);
    s_logged = 1;
  }
  // screen map: VRAM pixel -> PSX screen coord. Faithful = (vram - present_origin); wide/hi-res FB =
  // (vram - fb_origin)/ires — inverse of the tritex.vert relocation (no wide_off: content is placed 1:1).
  float org_x, org_y, off_x = 0.0f, off_y = 0.0f, inv_scale = 1.0f;
  if (frame_via_fb()) { org_x = 0.0f; org_y = (float)FB_Y0; inv_scale = 1.0f / (float)s_ires; }
  else          { org_x = (float)s_present_sx; org_y = (float)s_present_sy; }
  int viz = cfg_int("PSXPORT_SSAO_VIZ", 0);   // 1=AO factor; 2=normal; 3=lit; 4=shadow (any deferred-viz)
  // flags: bit0 SSAO, bit1 LIGHT (engine-native, never set here), bit2 SHADOW.
  int flags = (ssao_on() ? 1 : 0) | (shadows_on() ? 4 : 0);
  // shadow params: shadow_strength (overlay), shadow texel size (1/dim) for PCF, and a constant depth bias.
  float sh_strength = g_mods.shadow_strength, sh_texel = 1.0f / (float)SHADOW_DIM, sh_bias = 0.0015f;
  struct { float p0[4], p1[4]; int32_t p2[4]; float p3[4], p4[4], p5[4], p6[4]; float lvp[16]; } pc;
  pc.p0[0] = inv_near; pc.p0[1] = inv_far; pc.p0[2] = strength; pc.p0[3] = radius;
  pc.p1[0] = bias_frac; pc.p1[1] = range_frac; pc.p1[2] = NATIVE_3D_MIN; pc.p1[3] = NATIVE_3D_MAX;
  pc.p2[0] = VRAM_W; pc.p2[1] = IMG_H; pc.p2[2] = viz; pc.p2[3] = flags;
  pc.p3[0] = lx; pc.p3[1] = ly; pc.p3[2] = lz; pc.p3[3] = diffuse;
  pc.p4[0] = ambient; pc.p4[1] = cx; pc.p4[2] = cy; pc.p4[3] = H;
  pc.p5[0] = org_x; pc.p5[1] = org_y; pc.p5[2] = off_x; pc.p5[3] = off_y;
  pc.p6[0] = inv_scale; pc.p6[1] = sh_strength; pc.p6[2] = sh_texel; pc.p6[3] = sh_bias;
  // light view-projection (shadow_pass built it this frame; identity-ish fallback if it somehow didn't).
  if (s_shadow_lvp_valid) for (int i=0;i<16;i++) pc.lvp[i] = s_shadow_lvp[i];
  else { for (int i=0;i<16;i++) pc.lvp[i] = 0.0f; pc.lvp[0]=pc.lvp[5]=pc.lvp[10]=pc.lvp[15]=1.0f; }

  VkRenderPassBeginInfo rp = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
  rp.renderPass = s_ssao_rpass; rp.framebuffer = s_ssao_fb; rp.renderArea.extent = (VkExtent2D){ VRAM_W, IMG_H };
  vkCmdBeginRenderPass(s_cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
  VkViewport vpt = { 0, 0, VRAM_W, IMG_H, 0.0f, 1.0f }; VkRect2D sc = { {0,0}, { VRAM_W, IMG_H } };
  vkCmdSetViewport(s_cmd, 0, 1, &vpt); vkCmdSetScissor(s_cmd, 0, 1, &sc);
  vkCmdPushConstants(s_cmd, s_ssao_pll, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
  vkCmdBindDescriptorSets(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_ssao_pll, 0, 1, &s_ssao_dset, 0, 0);
  vkCmdBindPipeline(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_ssao_pipe);
  vkCmdDraw(s_cmd, 3, 1, 0, 0);
  vkCmdEndRenderPass(s_cmd);   // s_ssao_img -> TRANSFER_SRC (render pass finalLayout)

  // copy the AO'd color back into s_tex (the panel's render target), then resume rendering on it.
  img_barrier_on(s_tex, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                 VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
  VkImageCopy ic = { { VK_IMAGE_ASPECT_COLOR_BIT,0,0,1 }, {0,0,0}, { VK_IMAGE_ASPECT_COLOR_BIT,0,0,1 }, {0,0,0}, { VRAM_W, IMG_H, 1 } };
  vkCmdCopyImage(s_cmd, s_ssao_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s_tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
  // s_tex: transfer-dst -> color attachment (the semi pass / final present barrier continues from here)
  img_barrier_on(s_tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
  // depth: shader-read -> attachment (the semi pass depth-tests against it)
  depth_barrier(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
}

// Present the display region [sx,sy .. +w,h] of `src` (s_vram) via Vulkan, fit to aspect.
void GpuVkState::present(const uint16_t* src, int sx, int sy, int w, int h) {
  if (!gpu_vk_enabled()) return;
  if (!s_inited) init_vk();
  wide_init();
  s_present_sx = sx; s_present_sy = sy;   // faithful display origin (pre use_fb override) for the LIGHT screen map
  overlay_glue_frame_begin(&game->core);   // latch the world readout + run the overlay's CPU update
  // vkprof: re-check the channel each frame (present runs from boot, before the REPL `debug` is processed).
  int vp = cfg_dbg("vkprof");
  s_vkprof = vp;   // share with panel_upload (dirty-rect upload-volume counters)
  // Mirror the whole CPU VRAM (s_vram) into the GPU R16_UINT image (M1: SW still rasterizes;
  // M2+ will draw into this image directly and skip the upload for drawn regions). The display region
  // [sx,sy,w,h] is selected at sample time via the present push constant.
  double t_up = vp ? now_ms() : 0;
  memcpy(s_stage_ptr, src, (size_t)VRAM_W * VRAM_H * 2);
  if (vp) s_vp_upload_ms += now_ms() - t_up;

  VKC(vkWaitForFences(s_dev, 1, &s_fence, VK_TRUE, UINT64_MAX));
  // vkprof: the fence wait guarantees the PREVIOUS frame's GPU work (and its timestamps) is complete.
  if (vp && s_tspool && s_ts_primed) {
    uint64_t ts[2] = {0, 0};
    if (vkGetQueryPoolResults(s_dev, s_tspool, 0, 2, sizeof ts, ts, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
      uint64_t mask = (s_ts_validbits >= 64) ? ~0ull : ((1ull << s_ts_validbits) - 1);
      s_vp_gpu_ms += (double)((ts[1] & mask) - (ts[0] & mask)) * s_ts_period_ns / 1e6;
    }
  }
  uint32_t idx = 0;
  if (!s_headless) {
    // Rebuild the swapchain whenever the window's real size no longer matches s_extent, so s_extent /
    // win_w() / win_h() (and the overlay's window readout) track the live window this frame onward. We
    // poll the authoritative surface size rather than trust an SDL_WINDOWEVENT or a SUBOPTIMAL/OUT_OF_DATE
    // present result — Wayland delivers neither reliably, which is why resize was being missed entirely.
    // Skip zero-size (minimized) so we never build a zero-extent swapchain.
    VkExtent2D now = surface_extent_now();
    if (s_swap_dirty || ((now.width != s_extent.width || now.height != s_extent.height) && now.width && now.height)) {
      s_swap_dirty = 0; recreate_swapchain(); return;
    }
    VkResult acq = vkAcquireNextImageKHR(s_dev, s_swap, UINT64_MAX, s_sem_acq, VK_NULL_HANDLE, &idx);
    if (acq == VK_ERROR_OUT_OF_DATE_KHR) { recreate_swapchain(); return; }
    if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) { fprintf(stderr, "[gpu_vk] acquire %d\n", acq); exit(2); }
  }
  VKC(vkResetFences(s_dev, 1, &s_fence));

  VKC(vkResetCommandBuffer(s_cmd, 0));
  VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  VKC(vkBeginCommandBuffer(s_cmd, &bi));
  if (vp && s_tspool) { vkCmdResetQueryPool(s_cmd, s_tspool, 0, 2);
                        vkCmdWriteTimestamp(s_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, s_tspool, 0); }

  // The Panel uploads its persistent VRAM then renders (own depth occlusion). Single PC-native panel.
  s_npanels = 1;
  s_dbg_tri = s_tri_n; s_dbg_tex = s_tex_n; s_dbg_semi = s_semi_n;   // snapshot for gpu_vk_stats (vkstats probe)
  if (vp) { s_vp_tri += s_tri_n; s_vp_tex += s_tex_n; s_vp_semi += s_semi_n; }
  double t_rec = vp ? now_ms() : 0;
  for (int i = 0; i < s_npanels; i++) { panel_upload(&s_panels[i]); panel_render(&s_panels[i]); }
  if (vp) s_vp_record_ms += now_ms() - t_rec;

  // Headless (offscreen): the frame is fully rendered into s_tex; there is no swapchain to present to.
  // End + submit the geometry command buffer (signaling s_fence) and return; vk_dump/readback reads s_tex.
  if (s_headless) {
    if (vp && s_tspool) { vkCmdWriteTimestamp(s_cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s_tspool, 1); s_ts_primed = 1; }
    VKC(vkEndCommandBuffer(s_cmd));
    VkSubmitInfo su = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    su.commandBufferCount = 1; su.pCommandBuffers = &s_cmd;
    double t_sub = vp ? now_ms() : 0;
    VKC(vkQueueSubmit(s_queue, 1, &su, s_fence));
    if (vp) { s_vp_submit_ms += now_ms() - t_sub; vkprof_tick(); }
    // Headless readback (vkshot / gpu_vk_shot) reads s_last_*; when wide/hi-res the geometry rendered into
    // the scaled scratch FB, so report THAT region (mirrors the windowed use_fb() override below and
    // gpu_vk_dump). Without this, a headless wide shot crops to the 4:3 (320) display region and the
    // widescreen margin is invisible even though it's rendered.
    if (frame_via_fb()) { sx = 0; sy = FB_Y0; w = FBW(); h = FBH(); }
    s_last_sx = sx; s_last_sy = sy; s_last_w = w; s_last_h = h;
    return;
  }

  VkClearValue clear = {{{0, 0, 0, 1}}};
  VkRenderPassBeginInfo rp = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
  rp.renderPass = s_rpass; rp.framebuffer = s_swap_fb[idx];
  rp.renderArea.extent = s_extent; rp.clearValueCount = 1; rp.pClearValues = &clear;
  vkCmdBeginRenderPass(s_cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);

  // A 3D frame's content is in the scaled scratch FB (rendered denser at native FOV/scale) -> present that
  // FB region 1:1 at the SELECTED aspect. A 2D-only frame (SCEA/FMV/title/menu - VRAM-resident, no 3D) has
  // an EMPTY scratch FB -> present the native PSX display region [sx,sy,w,h] at 4:3. This is the PC-game
  // behavior and is what keeps hi-res/wide from mangling those fullscreen-2D screens.
  int via_fb = frame_via_fb();
  if (via_fb) { sx = 0; sy = FB_Y0; w = FBW(); h = FBH(); }
  s_last_sx = sx; s_last_sy = sy; s_last_w = w; s_last_h = h;   // for on-demand gpu_vk_shot (debug server)
  // Fit the sampled region into the window: a 2D screen always letterboxes to 4:3; a 3D frame uses the
  // selected aspect (AUTO fills the window; wide uses its native wide width). aw:ah = the letterbox aspect.
  int aw, ah;
  if (!via_fb)                      { aw = 4; ah = 3; }                   // fullscreen-2D screen -> 4:3 pillarbox
  else if (g_mods.aspect == ASPECT_AUTO) { aw = win_w(); ah = win_h(); }
  else if (s_wide)                  { aw = wide_native_w(); ah = 240; }   // 16:9 -> 428:240, 21:9 -> 560:240
  else                              { aw = 4; ah = 3; }
  int ow = s_extent.width, oh = s_extent.height;
  int npanes = 1;
  VkRect2D sc = { {0, 0}, s_extent };
  vkCmdSetScissor(s_cmd, 0, 1, &sc);
  vkCmdBindPipeline(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipe);
  int32_t disp[4] = { sx, sy, w, h };   // VRAM display region the present pass samples (same for both panes)
  vkCmdPushConstants(s_cmd, s_pll, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, disp);
  for (int p = 0; p < npanes; p++) {
    int paneW = ow / npanes, paneX = p * paneW, dw, dh;   // fit display aspect within each pane
    if (paneW * ah >= oh * aw) { dh = oh; dw = oh * aw / ah; } else { dw = paneW; dh = paneW * ah / aw; }
    VkViewport vpt = { (float)(paneX + (paneW - dw) / 2), (float)((oh - dh) / 2), (float)dw, (float)dh, 0.0f, 1.0f };
    vkCmdSetViewport(s_cmd, 0, 1, &vpt);
    vkCmdBindDescriptorSets(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pll, 0, 1, &s_panels[p].dset_present, 0, 0);
    vkCmdDraw(s_cmd, 3, 1, 0, 0);
  }
  overlay_glue_record(s_cmd, idx, s_extent);   // draw the RmlUi overlay on top (no-op if hidden)
  vkCmdEndRenderPass(s_cmd);
  if (vp && s_tspool) { vkCmdWriteTimestamp(s_cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s_tspool, 1); s_ts_primed = 1; }
  VKC(vkEndCommandBuffer(s_cmd));
  double t_sub = vp ? now_ms() : 0;

  VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  VkSubmitInfo su = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
  su.waitSemaphoreCount = 1; su.pWaitSemaphores = &s_sem_acq; su.pWaitDstStageMask = &wait;
  su.commandBufferCount = 1; su.pCommandBuffers = &s_cmd;
  su.signalSemaphoreCount = 1; su.pSignalSemaphores = &s_sem_rel[idx];
  VKC(vkQueueSubmit(s_queue, 1, &su, s_fence));

  VkPresentInfoKHR pr = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
  pr.waitSemaphoreCount = 1; pr.pWaitSemaphores = &s_sem_rel[idx];
  pr.swapchainCount = 1; pr.pSwapchains = &s_swap; pr.pImageIndices = &idx;
  VkResult pres = vkQueuePresentKHR(s_queue, &pr);
  if (pres == VK_ERROR_OUT_OF_DATE_KHR || pres == VK_SUBOPTIMAL_KHR) recreate_swapchain();
  else if (pres != VK_SUCCESS) { fprintf(stderr, "[gpu_vk] present %d\n", pres); exit(2); }
  if (vp) { s_vp_submit_ms += now_ms() - t_sub; vkprof_tick(); }

  poll_quit();
}

// ---- M2: GPU triangle rasterizer (flat/gouraud) into the VRAM image --------------------
static void make_hostbuf(VkDeviceSize sz, VkBufferUsageFlags use, VkBuffer* buf, VkDeviceMemory* mem, void** ptr) {
  VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
  bi.size = sz; bi.usage = use; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VKC(vkCreateBuffer(s_dev, &bi, 0, buf));
  VkMemoryRequirements mr; vkGetBufferMemoryRequirements(s_dev, *buf, &mr);
  VkMemoryAllocateInfo ma = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
  ma.allocationSize = mr.size; ma.memoryTypeIndex = mem_type(mr.memoryTypeBits,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  VKC(vkAllocateMemory(s_dev, &ma, 0, mem));
  VKC(vkBindBufferMemory(s_dev, *buf, *mem, 0));
  VKC(vkMapMemory(s_dev, *mem, 0, sz, 0, ptr));
}

void create_tri_pipeline(void) {
  // Depth image (D32) for OT-order depth: same extent as the VRAM image. DEPTH_STENCIL attachment +
  // TRANSFER_DST (so it can be cleared via the render pass). Persists across the opaque->semi passes.
  { VkImageCreateInfo di = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    di.imageType = VK_IMAGE_TYPE_2D; di.format = VK_FORMAT_D32_SFLOAT;
    di.extent = (VkExtent3D){ VRAM_W, IMG_H, 1 }; di.mipLevels = 1; di.arrayLayers = 1;
    di.samples = VK_SAMPLE_COUNT_1_BIT; di.tiling = VK_IMAGE_TILING_OPTIMAL;
    // SAMPLED so the PSXPORT_SSAO post pass can read the depth buffer (harmless when SSAO is off).
    di.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    di.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VKC(vkCreateImage(s_dev, &di, 0, &s_depth));
    VkMemoryRequirements dr; vkGetImageMemoryRequirements(s_dev, s_depth, &dr);
    VkMemoryAllocateInfo dm = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    dm.allocationSize = dr.size; dm.memoryTypeIndex = mem_type(dr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VKC(vkAllocateMemory(s_dev, &dm, 0, &s_depth_mem));
    VKC(vkBindImageMemory(s_dev, s_depth, s_depth_mem, 0));
    VkImageViewCreateInfo dv = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    dv.image = s_depth; dv.viewType = VK_IMAGE_VIEW_TYPE_2D; dv.format = VK_FORMAT_D32_SFLOAT;
    dv.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
    VKC(vkCreateImageView(s_dev, &dv, 0, &s_depth_view));
    // PSXPORT_SBS panel B's own depth buffer (same create info).
    VKC(vkCreateImage(s_dev, &di, 0, &s_depth_b));
    VkMemoryRequirements drb; vkGetImageMemoryRequirements(s_dev, s_depth_b, &drb);
    VkMemoryAllocateInfo dmb = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    dmb.allocationSize = drb.size; dmb.memoryTypeIndex = mem_type(drb.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VKC(vkAllocateMemory(s_dev, &dmb, 0, &s_depth_b_mem));
    VKC(vkBindImageMemory(s_dev, s_depth_b, s_depth_b_mem, 0));
    VkImageViewCreateInfo dvb = dv; dvb.image = s_depth_b;
    VKC(vkCreateImageView(s_dev, &dvb, 0, &s_depth_b_view)); }

  // render pass over the VRAM image: LOAD existing contents, draw, store; stays COLOR_ATTACHMENT layout.
  // Depth attachment LOADs too (it is explicitly cleared at the start of the opaque pass via
  // vkCmdClearAttachments, so the opaque->semi passes share one persistent depth buffer).
  VkAttachmentDescription at[2] = {{0},{0}};
  at[0].format = VRAM_BLEND_FMT;   // blendable A1B5G5R5 alias (same bits as the R16_UINT image)
  at[0].samples = VK_SAMPLE_COUNT_1_BIT;
  at[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; at[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  at[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; at[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  at[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  at[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  at[1].format = VK_FORMAT_D32_SFLOAT; at[1].samples = VK_SAMPLE_COUNT_1_BIT;
  at[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; at[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  at[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; at[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  at[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  at[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  VkAttachmentReference ar = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
  VkAttachmentReference dr = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
  VkSubpassDescription sp = {0}; sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  sp.colorAttachmentCount = 1; sp.pColorAttachments = &ar; sp.pDepthStencilAttachment = &dr;
  VkRenderPassCreateInfo rpi = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
  rpi.attachmentCount = 2; rpi.pAttachments = at; rpi.subpassCount = 1; rpi.pSubpasses = &sp;
  VKC(vkCreateRenderPass(s_dev, &rpi, 0, &s_vram_rpass));

  VkImageView fbviews[2] = { s_tex_blend_view, s_depth_view };   // blendable color alias + depth
  VkFramebufferCreateInfo fi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
  fi.renderPass = s_vram_rpass; fi.attachmentCount = 2; fi.pAttachments = fbviews;
  fi.width = VRAM_W; fi.height = IMG_H; fi.layers = 1;
  VKC(vkCreateFramebuffer(s_dev, &fi, 0, &s_vram_fb));
  VkImageView fbviews_b[2] = { s_tex_b_blend_view, s_depth_b_view };   // PSXPORT_SBS: s_tex_b alias + its OWN depth
  VkFramebufferCreateInfo fib = fi; fib.pAttachments = fbviews_b;
  VKC(vkCreateFramebuffer(s_dev, &fib, 0, &s_vram_fb_b));

  // PSXPORT_SBS depth-channel select as a vertex-stage SPECIALIZATION CONSTANT (SBS_NATIVE, constant_id
  // 0): bake it into the pipeline so each Panel binds its OWN variant — no shared push-constant state to
  // bleed between the two panes (which silently made them render identically).
  static const int32_t sbs_val[2] = { 0, 1 };
  static const VkSpecializationMapEntry sme = { 0, 0, sizeof(int32_t) };
  VkSpecializationInfo spec[2] = {
    { 1, &sme, sizeof(int32_t), &sbs_val[0] },
    { 1, &sme, sizeof(int32_t), &sbs_val[1] },
  };
  VkShaderModule vs = make_shader(spv_tri_vert, spv_tri_vert_len);
  VkShaderModule fs = make_shader(spv_tri_frag, spv_tri_frag_len);
  VkPipelineShaderStageCreateInfo st[2] = {
    { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", 0 },
    { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", 0 },
  };
  VkVertexInputBindingDescription vbd = { 0, sizeof(TriVtx), VK_VERTEX_INPUT_RATE_VERTEX };
  VkVertexInputAttributeDescription vad[4] = {
    { 0, 0, VK_FORMAT_R32G32_SFLOAT, 0 },            // pos (VRAM coords)
    { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, 8 },         // rgb 0..1
    { 2, 0, VK_FORMAT_R32_SFLOAT, 20 },              // ord (OT submission order -> depth)
    { 3, 0, VK_FORMAT_R32_SFLOAT, 24 },              // ordn (PSXPORT_SBS native depth)
  };
  VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
  vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &vbd;
  vi.vertexAttributeDescriptionCount = 4; vi.pVertexAttributeDescriptions = vad;
  VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
  ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
  vp.viewportCount = 1; vp.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
  rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState cba = {0}; cba.colorWriteMask = 0xF;   // opaque (no blend)
  VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
  cb.attachmentCount = 1; cb.pAttachments = &cba;
  VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
  VkPipelineDynamicStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
  ds.dynamicStateCount = 2; ds.pDynamicStates = dyn;
  // --- semi-transparent blend: one color-blend-attachment per PSX mode (HW fixed-function), + a dynamic
  // state that ADDS blend constants (the .5 / .25 factors set per draw). RGB blends per the mode; alpha is
  // irrelevant (A=STP, never read by present), kept = src so the stored STP bit stays the source's.
  VkPipelineColorBlendAttachmentState semi_cba[4];
  for (int m = 0; m < 4; m++) {
    VkPipelineColorBlendAttachmentState a = {0};
    a.blendEnable = VK_TRUE; a.colorWriteMask = 0xF;
    a.alphaBlendOp = VK_BLEND_OP_ADD; a.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; a.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    switch (m) {
      case 0: a.colorBlendOp = VK_BLEND_OP_ADD;                                     // avg: .5*s + .5*d
              a.srcColorBlendFactor = VK_BLEND_FACTOR_CONSTANT_COLOR; a.dstColorBlendFactor = VK_BLEND_FACTOR_CONSTANT_COLOR; break;
      case 1: a.colorBlendOp = VK_BLEND_OP_ADD;                                     // add: s + d
              a.srcColorBlendFactor = VK_BLEND_FACTOR_ONE; a.dstColorBlendFactor = VK_BLEND_FACTOR_ONE; break;
      case 2: a.colorBlendOp = VK_BLEND_OP_REVERSE_SUBTRACT;                        // sub: d - s
              a.srcColorBlendFactor = VK_BLEND_FACTOR_ONE; a.dstColorBlendFactor = VK_BLEND_FACTOR_ONE; break;
      default:a.colorBlendOp = VK_BLEND_OP_ADD;                                     // add4: .25*s + d
              a.srcColorBlendFactor = VK_BLEND_FACTOR_CONSTANT_COLOR; a.dstColorBlendFactor = VK_BLEND_FACTOR_ONE; break;
    }
    semi_cba[m] = a;
  }
  VkPipelineColorBlendStateCreateInfo semi_cb[4];
  for (int m = 0; m < 4; m++) { semi_cb[m] = (VkPipelineColorBlendStateCreateInfo){ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    semi_cb[m].attachmentCount = 1; semi_cb[m].pAttachments = &semi_cba[m]; }
  VkDynamicState semi_dyn[3] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS };
  VkPipelineDynamicStateCreateInfo semi_ds = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
  semi_ds.dynamicStateCount = 3; semi_ds.pDynamicStates = semi_dyn;
  // Depth state: OT order is the depth. Opaque writes depth, compare GREATER_OR_EQUAL (later prim =
  // greater ord wins, matching painter's order). The semi variant tests the same way but does NOT
  // write, so semi is rejected behind later opaque yet never occludes anything itself.
  VkPipelineDepthStencilStateCreateInfo dpo = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
  dpo.depthTestEnable = VK_TRUE; dpo.depthWriteEnable = VK_TRUE; dpo.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
  VkPipelineDepthStencilStateCreateInfo dps = dpo; dps.depthWriteEnable = VK_FALSE;  // semi: test, no write
  VkGraphicsPipelineCreateInfo gp = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
  gp.stageCount = 2; gp.pStages = st; gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia;
  gp.pViewportState = &vp; gp.pRasterizationState = &rs; gp.pMultisampleState = &ms;
  gp.pColorBlendState = &cb; gp.pDynamicState = &ds; gp.pDepthStencilState = &dpo;
  gp.layout = s_pll; gp.renderPass = s_vram_rpass;
  for (int nv2 = 0; nv2 < 2; nv2++) { st[0].pSpecializationInfo = &spec[nv2];   // [0]=default, [1]=native
    VKC(vkCreateGraphicsPipelines(s_dev, VK_NULL_HANDLE, 1, &gp, 0, &s_tri_pipe[nv2])); }
  st[0].pSpecializationInfo = 0;
  vkDestroyShaderModule(s_dev, vs, 0); vkDestroyShaderModule(s_dev, fs, 0);

  make_hostbuf(sizeof(TriVtx) * TRI_CAP, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &s_vbuf, &s_vbuf_mem, &s_vbuf_ptr);
  make_hostbuf((VkDeviceSize)VRAM_W * IMG_H * 2, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &s_rb, &s_rb_mem, &s_rb_ptr);

  // textured pipeline (TexVtx input, samples the VRAM snapshot, renders to the VRAM render pass)
  VkShaderModule tvs = make_shader(spv_tritex_vert, spv_tritex_vert_len);
  VkShaderModule tfs = make_shader(spv_tritex_frag, spv_tritex_frag_len);
  VkPipelineShaderStageCreateInfo tst[2] = {
    { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_VERTEX_BIT, tvs, "main", 0 },
    { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_FRAGMENT_BIT, tfs, "main", 0 },
  };
  VkVertexInputBindingDescription tvbd = { 0, sizeof(TexVtx), VK_VERTEX_INPUT_RATE_VERTEX };
  VkVertexInputAttributeDescription tvad[9] = {
    { 0, 0, VK_FORMAT_R32G32_SFLOAT,       0 },   // pos
    { 1, 0, VK_FORMAT_R32G32_SFLOAT,       8 },   // uv
    { 2, 0, VK_FORMAT_R32G32B32_SFLOAT,    16 },  // col
    { 3, 0, VK_FORMAT_R32G32B32A32_SINT,   28 },  // tp (tpx,tpy,mode,raw)
    { 4, 0, VK_FORMAT_R32G32B32A32_SINT,   44 },  // clut (clutx,cluty,-,-)
    { 5, 0, VK_FORMAT_R32G32B32A32_SINT,   60 },  // tw (mask_x,mask_y,off_x,off_y)
    { 6, 0, VK_FORMAT_R32G32B32A32_SINT,   76 },  // da (x0,y0,x1,y1)
    { 7, 0, VK_FORMAT_R32_SFLOAT,          92 },  // ord (OT submission order -> depth)
    { 8, 0, VK_FORMAT_R32_SFLOAT,          96 },  // ordn (PSXPORT_SBS native depth)
  };
  VkPipelineVertexInputStateCreateInfo tvi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
  tvi.vertexBindingDescriptionCount = 1; tvi.pVertexBindingDescriptions = &tvbd;
  tvi.vertexAttributeDescriptionCount = 9; tvi.pVertexAttributeDescriptions = tvad;
  gp.pStages = tst; gp.pVertexInputState = &tvi;   // reuse ia/vp/rs/ms/cb/ds/layout/renderPass from above
  // FRAGMENT spec constant SEMI_PASS (id 1): 0 = opaque sub-pass of a semi prim (STP=0 texels), 1 = blend
  // sub-pass (STP=1 / untextured), 2 = plain opaque prim (no STP gating). Lives in the fragment stage, so
  // it doesn't collide with the vertex stage's SBS_NATIVE (id 0).
  static const int32_t semipass_val[3] = { 0, 1, 2 };
  static const VkSpecializationMapEntry sme_f = { 1, 0, sizeof(int32_t) };
  VkSpecializationInfo fspec[3] = {
    { 1, &sme_f, sizeof(int32_t), &semipass_val[0] },
    { 1, &sme_f, sizeof(int32_t), &semipass_val[1] },
    { 1, &sme_f, sizeof(int32_t), &semipass_val[2] },
  };
  for (int nv2 = 0; nv2 < 2; nv2++) { tst[0].pSpecializationInfo = &spec[nv2];   // [0]=default, [1]=native
    gp.pDepthStencilState = &dpo;                   // opaque-textured: depth test + write
    gp.pColorBlendState = &cb; gp.pDynamicState = &ds;    // no blend
    tst[1].pSpecializationInfo = &fspec[2];         // plain opaque prim
    VKC(vkCreateGraphicsPipelines(s_dev, VK_NULL_HANDLE, 1, &gp, 0, &s_tritex_pipe[nv2]));
    gp.pDepthStencilState = &dps;                   // semi prim: depth test, NO write
    tst[1].pSpecializationInfo = &fspec[0];         // semi OPAQUE sub-pass (STP=0 texels), no blend
    VKC(vkCreateGraphicsPipelines(s_dev, VK_NULL_HANDLE, 1, &gp, 0, &s_tritex_semi_op_pipe[nv2]));
    gp.pDynamicState = &semi_ds;                    // semi blend: + dynamic blend constants
    tst[1].pSpecializationInfo = &fspec[1];         // semi BLEND sub-pass (STP=1 / untextured)
    for (int m = 0; m < 4; m++) { gp.pColorBlendState = &semi_cb[m];   // one HW-blend pipeline per PSX mode
      VKC(vkCreateGraphicsPipelines(s_dev, VK_NULL_HANDLE, 1, &gp, 0, &s_tritex_semi_pipe[m][nv2])); }
    gp.pColorBlendState = &cb; gp.pDynamicState = &ds; }   // restore for the next iteration's opaque create
  tst[0].pSpecializationInfo = 0; tst[1].pSpecializationInfo = 0;
  vkDestroyShaderModule(s_dev, tvs, 0); vkDestroyShaderModule(s_dev, tfs, 0);
  make_hostbuf(sizeof(TexVtx) * TEX_CAP, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &s_tvbuf, &s_tvbuf_mem, &s_tvbuf_ptr);
  make_hostbuf(sizeof(TexVtx) * TEX_CAP, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &s_semibuf, &s_semibuf_mem, &s_semibuf_ptr);
}

// Append one triangle (VRAM coords + per-vertex RGB 0..255) to the batch.
void GpuVkState::draw_tri(int x0,int y0,int r0,int g0,int b0, int x1,int y1,int r1,int g1,int b1,
                     int x2,int y2,int r2,int g2,int b2) {
  if (!s_inited || s_tri_n + 3 > TRI_CAP) return;
  TriVtx* v = (TriVtx*)s_vbuf_ptr + s_tri_n;
  v[0] = (TriVtx){ (float)x0, (float)y0, r0/255.f, g0/255.f, b0/255.f, s_vd ? ord3d(s_vd[0]) : s_cur_ord, s_vdn ? ord3d(s_vdn[0]) : s_cur_ordn };
  v[1] = (TriVtx){ (float)x1, (float)y1, r1/255.f, g1/255.f, b1/255.f, s_vd ? ord3d(s_vd[1]) : s_cur_ord, s_vdn ? ord3d(s_vdn[1]) : s_cur_ordn };
  v[2] = (TriVtx){ (float)x2, (float)y2, r2/255.f, g2/255.f, b2/255.f, s_vd ? ord3d(s_vd[2]) : s_cur_ord, s_vdn ? ord3d(s_vdn[2]) : s_cur_ordn };
  s_tri_n += 3;
}

// Append one TEXTURED triangle: per-vertex pos/uv/color (rgb 0..255) + shared page/CLUT/mode/raw state.
void GpuVkState::tex_emit(TexVtx* t, const int* xs, const int* ys, const int* us, const int* vs,
                     const unsigned char* rs, const unsigned char* gs, const unsigned char* bs,
                     int tpx, int tpy, int mode, int raw, int clutx, int cluty,
                     int twmx, int twmy, int twox, int twoy, int dax0, int day0, int dax1, int day1,
                     int semi, int blend) {
  for (int i = 0; i < 3; i++) {
    // Vertex smoothing (#15): the world path supplies SUB-PIXEL float screen XY (draw offset already
    // applied in float) — use it directly instead of the rounded integer xs/ys, so geometry no longer
    // snaps pixel-to-pixel (PS1 wobble). 2D/HUD/un-owned prims leave s_xf NULL and keep the integer XY.
    t[i].x = s_xf ? s_xf[i] : (float)xs[i];
    t[i].y = s_yf ? s_yf[i] : (float)ys[i];
    t[i].u = us[i]; t[i].v = vs[i];
    t[i].r = rs[i]/255.f; t[i].g = gs[i]/255.f; t[i].b = bs[i]/255.f;
    t[i].tp[0] = tpx; t[i].tp[1] = tpy; t[i].tp[2] = mode; t[i].tp[3] = raw;
    t[i].clut[0] = clutx; t[i].clut[1] = cluty; t[i].clut[2] = semi; t[i].clut[3] = blend;
    t[i].tw[0] = twmx; t[i].tw[1] = twmy; t[i].tw[2] = twox; t[i].tw[3] = twoy;
    t[i].da[0] = dax0; t[i].da[1] = day0; t[i].da[2] = dax1; t[i].da[3] = day1;
    t[i].ord = s_vd ? ord3d(s_vd[i]) : s_cur_ord;   // per-vertex real depth (3D band) else OT-order band
    t[i].ordn = s_vdn ? ord3d(s_vdn[i]) : s_cur_ordn;   // PSXPORT_SBS native channel (right half)
  }
}
void GpuVkState::draw_tritri(const int* xs, const int* ys, const int* us, const int* vs,
                        const unsigned char* rs, const unsigned char* gs, const unsigned char* bs,
                        int tpx, int tpy, int mode, int raw, int clutx, int cluty,
                        int twmx, int twmy, int twox, int twoy,
                        int dax0, int day0, int dax1, int day1) {
  if (!s_inited || s_tex_n + 3 > TEX_CAP) return;
  tex_emit((TexVtx*)s_tvbuf_ptr + s_tex_n, xs, ys, us, vs, rs, gs, bs, tpx, tpy, mode, raw, clutx, cluty,
           twmx, twmy, twox, twoy, dax0, day0, dax1, day1, 0, 0);
  s_tex_n += 3;
}
// Semi-transparent triangle (mode 3 = untextured flat). Drawn AFTER opaque, blending against the
// framebuffer snapshot, per `blend` (0=avg,1=add,2=sub,3=add/4).
void GpuVkState::draw_semi(const int* xs, const int* ys, const int* us, const int* vs,
                      const unsigned char* rs, const unsigned char* gs, const unsigned char* bs,
                      int tpx, int tpy, int mode, int raw, int clutx, int cluty,
                      int twmx, int twmy, int twox, int twoy,
                      int dax0, int day0, int dax1, int day1, int blend) {
  if (!s_inited || s_semi_n + 3 > TEX_CAP) return;
  tex_emit((TexVtx*)s_semibuf_ptr + s_semi_n, xs, ys, us, vs, rs, gs, bs, tpx, tpy, mode, raw, clutx, cluty,
           twmx, twmy, twox, twoy, dax0, day0, dax1, day1, 1, blend);
  s_semi_n += 3;
}

// Self-test: clear VRAM, draw batched tris into it, read back. Returns the readback (uint16 VRAM).
void GpuVkState::tri_render_and_readback(uint16_t* out) {
  VKC(vkWaitForFences(s_dev, 1, &s_fence, VK_TRUE, UINT64_MAX));
  VKC(vkResetFences(s_dev, 1, &s_fence));
  VKC(vkResetCommandBuffer(s_cmd, 0));
  VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  VKC(vkBeginCommandBuffer(s_cmd, &bi));

  img_barrier(s_tex_undef ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
              VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
  s_tex_undef = 0;
  VkClearColorValue ccv = { .uint32 = {0,0,0,0} };
  VkImageSubresourceRange rng = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
  vkCmdClearColorImage(s_cmd, s_tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &ccv, 1, &rng);
  img_barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
              VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
              VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);

  VkRenderPassBeginInfo rp = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
  rp.renderPass = s_vram_rpass; rp.framebuffer = s_vram_fb;
  rp.renderArea.extent = (VkExtent2D){ VRAM_W, VRAM_H };
  vkCmdBeginRenderPass(s_cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
  VkViewport vpt = { 0, 0, VRAM_W, VRAM_H, 0.0f, 1.0f };
  VkRect2D sc = { {0,0}, { VRAM_W, VRAM_H } };
  vkCmdSetViewport(s_cmd, 0, 1, &vpt); vkCmdSetScissor(s_cmd, 0, 1, &sc);
  push_wide(0);
  vkCmdBindPipeline(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_tri_pipe[0]);
  VkDeviceSize off = 0; vkCmdBindVertexBuffers(s_cmd, 0, 1, &s_vbuf, &off);
  if (s_tri_n) vkCmdDraw(s_cmd, s_tri_n, 1, 0, 0);
  vkCmdEndRenderPass(s_cmd);

  img_barrier(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
              VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
  VkBufferImageCopy bc = {0};
  bc.imageSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
  bc.imageExtent = (VkExtent3D){ VRAM_W, VRAM_H, 1 };
  vkCmdCopyImageToBuffer(s_cmd, s_tex, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s_rb, 1, &bc);
  // leave it TRANSFER_SRC; next present's barrier expects SHADER_READ — mark undef so it re-barriers.
  s_tex_undef = 1;

  VKC(vkEndCommandBuffer(s_cmd));
  VkSubmitInfo su = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
  su.commandBufferCount = 1; su.pCommandBuffers = &s_cmd;
  VKC(vkQueueSubmit(s_queue, 1, &su, s_fence));
  VKC(vkWaitForFences(s_dev, 1, &s_fence, VK_TRUE, UINT64_MAX));
  memcpy(out, s_rb_ptr, (size_t)VRAM_W * VRAM_H * 2);
}

static void img_barrier_on(VkImage im, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags ss,
                           VkPipelineStageFlags ds, VkAccessFlags sa, VkAccessFlags da) {
  VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
  b.oldLayout = from; b.newLayout = to; b.image = im;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
  b.srcAccessMask = sa; b.dstAccessMask = da;
  vkCmdPipelineBarrier(s_cmd, ss, ds, 0, 0, 0, 0, 0, 1, &b);
}

// Render the batched tris (untextured + textured) on top of `bg` (the SW VRAM) and read the result back.
// Textured prims sample a SNAPSHOT of `bg` (s_vram_tex) — same textures SW used — so where VK's
// rasterization/sampling matches SW, out==bg; mismatches reveal rule deltas. Avoids render/sample loop.
void GpuVkState::tri_over_bg_readback(const uint16_t* bg, uint16_t* out) {
  memcpy(s_stage_ptr, bg, (size_t)VRAM_W * VRAM_H * 2);
  VKC(vkWaitForFences(s_dev, 1, &s_fence, VK_TRUE, UINT64_MAX));
  VKC(vkResetFences(s_dev, 1, &s_fence));
  VKC(vkResetCommandBuffer(s_cmd, 0));
  VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  VKC(vkBeginCommandBuffer(s_cmd, &bi));
  VkBufferImageCopy bc = {0};
  bc.imageSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
  bc.imageExtent = (VkExtent3D){ VRAM_W, VRAM_H, 1 };

  // upload bg into the render target
  img_barrier(s_tex_undef ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
              VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
  s_tex_undef = 0;
  vkCmdCopyBufferToImage(s_cmd, s_stage, s_tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
  img_barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
              VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
              VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
  // snapshot bg into the texture-source image (sampled by the textured pipeline)
  img_barrier_on(s_vram_tex, s_vram_tex_undef ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
  s_vram_tex_undef = 0;
  vkCmdCopyBufferToImage(s_cmd, s_stage, s_vram_tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
  img_barrier_on(s_vram_tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);

  VkRenderPassBeginInfo rp = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
  rp.renderPass = s_vram_rpass; rp.framebuffer = s_vram_fb; rp.renderArea.extent = (VkExtent2D){ VRAM_W, VRAM_H };
  vkCmdBeginRenderPass(s_cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
  VkViewport vpt = { 0, 0, VRAM_W, VRAM_H, 0.0f, 1.0f }; VkRect2D sc = { {0,0}, { VRAM_W, VRAM_H } };
  vkCmdSetViewport(s_cmd, 0, 1, &vpt); vkCmdSetScissor(s_cmd, 0, 1, &sc);
  push_wide(0);
  vkCmdBindDescriptorSets(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pll, 0, 1, &s_dset_tex, 0, 0);
  VkDeviceSize off = 0;
  if (s_tri_n) { vkCmdBindPipeline(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_tri_pipe[0]);
                 vkCmdBindVertexBuffers(s_cmd, 0, 1, &s_vbuf, &off); vkCmdDraw(s_cmd, s_tri_n, 1, 0, 0); }
  if (s_tex_n) { vkCmdBindPipeline(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_tritex_pipe[0]);
                 vkCmdBindVertexBuffers(s_cmd, 0, 1, &s_tvbuf, &off); vkCmdDraw(s_cmd, s_tex_n, 1, 0, 0); }
  vkCmdEndRenderPass(s_cmd);

  img_barrier(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
              VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
  vkCmdCopyImageToBuffer(s_cmd, s_tex, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s_rb, 1, &bc);
  s_tex_undef = 1;
  VKC(vkEndCommandBuffer(s_cmd));
  VkSubmitInfo su = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
  su.commandBufferCount = 1; su.pCommandBuffers = &s_cmd;
  VKC(vkQueueSubmit(s_queue, 1, &su, s_fence));
  VKC(vkWaitForFences(s_dev, 1, &s_fence, VK_TRUE, UINT64_MAX));
  memcpy(out, s_rb_ptr, (size_t)VRAM_W * VRAM_H * 2);
}

// Read back the live VK-rendered VRAM (display/FB region) and write it to `path` as a PPM.
// Read the full VK VRAM image (s_tex) back into the host buffer s_rb_ptr (uint16, VRAM_W x IMG_H).
VkImage s_rb_img;   // which image vk_readback_to_rb copies (0 = s_tex); set per dump (SBS diag)
static void vk_readback_to_rb(void) {
  VkImage img = s_rb_img ? s_rb_img : s_tex;
  VKC(vkWaitForFences(s_dev, 1, &s_fence, VK_TRUE, UINT64_MAX)); VKC(vkResetFences(s_dev, 1, &s_fence));
  VKC(vkResetCommandBuffer(s_cmd, 0));
  VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT; VKC(vkBeginCommandBuffer(s_cmd, &bi));
  img_barrier_on(img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
              VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT);
  VkBufferImageCopy bc = {0}; bc.imageSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT,0,0,1 };
  bc.imageExtent = (VkExtent3D){ VRAM_W, IMG_H, 1 };   // include the scratch FB rows (>=512)
  vkCmdCopyImageToBuffer(s_cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s_rb, 1, &bc);
  img_barrier_on(img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
              VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
              VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT);
  VKC(vkEndCommandBuffer(s_cmd));
  VkSubmitInfo su = { VK_STRUCTURE_TYPE_SUBMIT_INFO }; su.commandBufferCount = 1; su.pCommandBuffers = &s_cmd;
  VKC(vkQueueSubmit(s_queue, 1, &su, s_fence)); VKC(vkWaitForFences(s_dev, 1, &s_fence, VK_TRUE, UINT64_MAX));
}
static void vk_dump_to(const char* path, int sx, int sy, int w, int h) {
  vk_readback_to_rb();
  const uint16_t* vram = (const uint16_t*)s_rb_ptr;
  FILE* f = fopen(path, "wb"); if (!f) return;
  fprintf(f, "P6\n%d %d\n255\n", w, h);
  for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { uint16_t p = vram[((sy+y)%IMG_H)*VRAM_W + ((sx+x)&1023)];
    unsigned char c[3] = { (unsigned char)((p&31)<<3), (unsigned char)(((p>>5)&31)<<3), (unsigned char)(((p>>10)&31)<<3) };
    fwrite(c, 1, 3, f); }
  fclose(f);
}
// On-demand VK readback of an EXPLICIT VRAM region (the gpu_native display region) — used by the REPL
// `shot` command, which runs headless where s_last_* (the windowed-present region) is never set.
void gpu_vk_shot_region(Core* core, const char* path, int sx, int sy, int w, int h) {
  if (!gpu_vk_enabled()) return;
  // Under hi-res/wide the present samples the scaled scratch FB (rows >=FB_Y0), NOT the display region —
  // capture THAT so the shot matches what's on screen (else hi-res/wide shots show the empty 4:3 region).
  if (core->game->gpu_vk.frame_via_fb()) { sx = 0; sy = FB_Y0; w = FBW(); h = FBH(); }
  vk_dump_to(path, sx, sy, w, h);
  fprintf(stderr, "[vk_shot] wrote %s (%dx%d @ %d,%d)\n", path, w, h, sx, sy);
}
void GpuVkState::shot(const char* path) {
  if (!gpu_vk_enabled() || !s_inited) { fprintf(stderr, "[vk_shot] VK not active\n"); return; }
  vk_dump_to(path, s_last_sx, s_last_sy, s_last_w, s_last_h);
  fprintf(stderr, "[vk_shot] wrote %s (%dx%d @ %d,%d)\n", path, s_last_w, s_last_h, s_last_sx, s_last_sy);
}
// Read back an ARBITRARY VK VRAM region (e.g. a texture atlas page) to a PPM — to verify a texture
// is actually present in the VK image (s_tex) where the semi pass samples it. (debug server `vkvram`)
void gpu_vk_vram_region(const char* path, int x, int y, int w, int h) {
  if (!gpu_vk_enabled() || !s_inited) { fprintf(stderr, "[vk_vram] VK not active\n"); return; }
  vk_dump_to(path, x, y, w, h);
  fprintf(stderr, "[vk_vram] wrote %s (%dx%d @ %d,%d)\n", path, w, h, x, y);
}
// Dump the FULL 1024x512 VRAM as RAW little-endian uint16 words (1MB, no header) — for offline
// tooling that needs the actual 16bpp/8bpp/4bpp index words (the PPM dump is RGB555-decoded and so
// destroys paletted index data). Used to extract a CLUT-paletted cel sheet (e.g. the walking dust).
void gpu_vk_vram_raw(const char* path) {
  if (!gpu_vk_enabled() || !s_inited) { fprintf(stderr, "[vk_vram] VK not active\n"); return; }
  vk_readback_to_rb();
  const uint16_t* vram = (const uint16_t*)s_rb_ptr;
  FILE* f = fopen(path, "wb"); if (!f) { perror(path); return; }
  for (int y = 0; y < VRAM_H; y++) fwrite(&vram[y * VRAM_W], 2, VRAM_W, f);
  fclose(f);
  fprintf(stderr, "[vk_vram] wrote RAW %s (%dx%d u16)\n", path, VRAM_W, VRAM_H);
}

// Per-PRESENT reset of the host-side geometry batch (draw prims + shadow stream).
// One pipeline: each present pass emits the WHOLE render queue (color prims AND, via gpu_emit_rq_item, the
// shadow tris carried on each opaque world item — render_queue.h sh_cast), then presents through the full
// pipeline (panel_render: opaque/semi + shadow_pass + ssao_pass), then resets EVERYTHING here. The next
// pass refills both streams by re-emitting the same queue, so the shadow map rebuilds identically on every
// pass with no side-channel. This is why the old keep_shadow flag is gone — there is no asymmetric pass to
// preserve a stream for. (Per the user's 60fps design shadows are not interpolated: build_lerp leaves the
// view-space sh_v* at B positions, so both passes cast the same shadow.)
void GpuVkState::frame_end(const uint16_t* svram, int frame) {
  (void)svram; (void)frame;
  if (!gpu_vk_enabled() || !s_inited) { s_tri_n = 0; s_shadow_n = 0; return; }
  s_tri_n = 0; s_tex_n = 0; s_semi_n = 0; s_dirty_n = 0;
  s_semi_grp_n = 0; s_sg_valid = 0;
  s_shadow_n = 0; s_shadow_lvp_valid = 0;   // per-pass host geometry stream (refilled by the next queue emit)
}

// PSXPORT_VK_TRITEST=1: headless-ish self-test of the triangle rasterizer. Draws a known flat tri and a
// gouraud tri, reads back, checks expected pixels, prints PASS/FAIL, exits. Validates the GPU raster path.
void GpuVkState::tritest() {
  if (!cfg_on("PSXPORT_VK_TRITEST")) return;
  if (!s_inited) init_vk();
  s_tri_n = 0;
  draw_tri( 10,10, 255,0,0,  200,10, 255,0,0,  10,200, 255,0,0);    // flat red, big right-triangle
  draw_tri(300,300, 255,0,0, 460,300, 0,255,0, 300,460, 0,0,255);   // gouraud r/g/b corners
  static uint16_t vram[VRAM_W * VRAM_H];
  tri_render_and_readback(vram);
  uint16_t inside_flat = vram[40 * VRAM_W + 40];        // inside flat tri -> red 0x001F
  uint16_t outside     = vram[400 * VRAM_W + 50];       // background -> 0
  uint16_t g_corner    = vram[305 * VRAM_W + 445];      // inside, near green vertex (460,300) -> green-dominant
  int gr = g_corner & 31, gg = (g_corner >> 5) & 31, gb = (g_corner >> 10) & 31;
  int ok = (inside_flat == 0x001F) && (outside == 0x0000) && (gg > gr && gg > gb && gg > 16);
  fprintf(stderr, "[vk_tritest] flat(40,40)=0x%04x (want 0x001f)  bg(50,400)=0x%04x (want 0)  "
          "gouraud(445,305)=0x%04x r=%d g=%d b=%d  => %s\n",
          inside_flat, outside, g_corner, gr, gg, gb, ok ? "PASS" : "FAIL");
  // dump the GPU-rendered region (0,0)-(480,480) as a PPM for visual confirmation
  FILE* f = fopen("scratch/screenshots/vk_tritest.ppm", "wb");
  if (f) { int W = 480, H = 480; fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
      uint16_t p = vram[y * VRAM_W + x];
      unsigned char rgb[3] = { (unsigned char)(((p)&31)<<3), (unsigned char)(((p>>5)&31)<<3), (unsigned char)(((p>>10)&31)<<3) };
      fwrite(rgb, 1, 3, f); }
    fclose(f); fprintf(stderr, "[vk_tritest] wrote scratch/screenshots/vk_tritest.ppm\n"); }
  exit(ok ? 0 : 3);
}

// ---- Public API: thin free-function wrappers over the per-instance GpuVkState methods. Keep the
// C-style call sites stable; each forwards to core->game->gpu_vk (de-globalization R2, 2026-06-19). ----
void gpu_vk_set_vd(Core* core, const float* d3) { core->game->gpu_vk.set_vd(d3); }
void gpu_vk_set_vd_n(Core* core, const float* d3) { core->game->gpu_vk.set_vd_n(d3); }
void gpu_vk_set_xyf(Core* core, const float* xf, const float* yf) { core->game->gpu_vk.set_xyf(xf, yf); }
void gpu_vk_set_order(Core* core, unsigned idx) { core->game->gpu_vk.set_order(idx); }
void gpu_vk_set_order_2d(Core* core, unsigned idx) { core->game->gpu_vk.set_order_2d(idx); }
void gpu_vk_set_order_2d_n(Core* core, unsigned idx) { core->game->gpu_vk.set_order_2d_n(idx); }
void gpu_vk_set_order_2d_bg(Core* core, unsigned idx) { core->game->gpu_vk.set_order_2d_bg(idx); }
void gpu_vk_set_order_2d_bg_n(Core* core, unsigned idx) { core->game->gpu_vk.set_order_2d_bg_n(idx); }
void gpu_vk_semi_group(Core* core, int x0, int y0, int x1, int y1) { core->game->gpu_vk.semi_group(x0, y0, x1, y1); }
void gpu_vk_stats(Core* core, int* tri, int* tex, int* semi) { core->game->gpu_vk.stats(tri, tex, semi); }
void gpu_vk_dirty(Core* core, int x, int y, int w, int h) { core->game->gpu_vk.dirty(x, y, w, h); }
void gpu_vk_present(Core* core, const uint16_t* src, int sx, int sy, int w, int h) { core->game->gpu_vk.present(src, sx, sy, w, h); }
void gpu_vk_draw_tri(Core* core, int x0,int y0,int r0,int g0,int b0, int x1,int y1,int r1,int g1,int b1, int x2,int y2,int r2,int g2,int b2) { core->game->gpu_vk.draw_tri(x0,y0,r0,g0,b0,x1,y1,r1,g1,b1,x2,y2,r2,g2,b2); }
void gpu_vk_draw_tritri(Core* core, const int* xs, const int* ys, const int* us, const int* vs, const unsigned char* rs, const unsigned char* gs, const unsigned char* bs, int tpx, int tpy, int mode, int raw, int clutx, int cluty, int twmx, int twmy, int twox, int twoy, int dax0, int day0, int dax1, int day1) { core->game->gpu_vk.draw_tritri(xs,ys,us,vs,rs,gs,bs,tpx,tpy,mode,raw,clutx,cluty,twmx,twmy,twox,twoy,dax0,day0,dax1,day1); }
void gpu_vk_draw_semi(Core* core, const int* xs, const int* ys, const int* us, const int* vs, const unsigned char* rs, const unsigned char* gs, const unsigned char* bs, int tpx, int tpy, int mode, int raw, int clutx, int cluty, int twmx, int twmy, int twox, int twoy, int dax0, int day0, int dax1, int day1, int blend) { core->game->gpu_vk.draw_semi(xs,ys,us,vs,rs,gs,bs,tpx,tpy,mode,raw,clutx,cluty,twmx,twmy,twox,twoy,dax0,day0,dax1,day1,blend); }
void gpu_vk_shot(Core* core, const char* path) { core->game->gpu_vk.shot(path); }
void gpu_vk_frame_end(Core* core, const uint16_t* svram, int frame) { core->game->gpu_vk.frame_end(svram, frame); }
void gpu_vk_tritest(Core* core) { core->game->gpu_vk.tritest(); }
#else
#include <stdint.h>
int  gpu_vk_enabled(void) { return 0; }
void gpu_vk_present(Core* core, const uint16_t* src, int sx, int sy, int w, int h) { (void)core;(void)src;(void)sx;(void)sy;(void)w;(void)h; }
void gpu_vk_tritest(Core* core) { (void)core; }
void gpu_vk_frame_end(Core* core, const uint16_t* svram, int frame) { (void)core;(void)svram; (void)frame; }
void gpu_vk_shot(Core* core, const char* path) { (void)core;(void)path; }
void gpu_vk_set_order(Core* core, unsigned idx) { (void)core;(void)idx; }
void gpu_vk_set_order_2d(Core* core, unsigned idx) { (void)core;(void)idx; }
void gpu_vk_set_order_2d_n(Core* core, unsigned idx) { (void)core;(void)idx; }
void gpu_vk_set_order_2d_bg(Core* core, unsigned idx) { (void)core;(void)idx; }
void gpu_vk_set_order_2d_bg_n(Core* core, unsigned idx) { (void)core;(void)idx; }
void gpu_vk_set_vd(Core* core, const float* d3) { (void)core;(void)d3; }
void gpu_vk_set_vd_n(Core* core, const float* d3) { (void)core;(void)d3; }
void gpu_vk_set_xyf(Core* core, const float* xf, const float* yf) { (void)core;(void)xf;(void)yf; }
void gpu_vk_rawdump_arm(const char* path, int frame) { (void)path;(void)frame; }
void gpu_vk_vram_region(const char* path, int x, int y, int w, int h) { (void)path;(void)x;(void)y;(void)w;(void)h; }
void gpu_vk_stats(Core* core, int* tri, int* tex, int* semi) { (void)core; if(tri)*tri=0; if(tex)*tex=0; if(semi)*semi=0; }
void gpu_vk_dirty(Core* core, int x, int y, int w, int h) { (void)core;(void)x;(void)y;(void)w;(void)h; }
void gpu_vk_semi_group(Core* core, int x0, int y0, int x1, int y1) { (void)core;(void)x0;(void)y0;(void)x1;(void)y1; }
void gpu_vk_draw_tri(Core* core, int a,int b,int c,int d,int e,int f,int g,int h,int i,int j,int k,int l,int m,int n,int o) {
  (void)core;(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h;(void)i;(void)j;(void)k;(void)l;(void)m;(void)n;(void)o;
}
void gpu_vk_draw_tritri(Core* core, const int* xs, const int* ys, const int* us, const int* vs,
                        const unsigned char* rs, const unsigned char* gs, const unsigned char* bs,
                        int tpx, int tpy, int mode, int raw, int clutx, int cluty,
                        int twmx, int twmy, int twox, int twoy,
                        int dax0, int day0, int dax1, int day1) {
  (void)core;(void)xs;(void)ys;(void)us;(void)vs;(void)rs;(void)gs;(void)bs;(void)tpx;(void)tpy;(void)mode;(void)raw;
  (void)clutx;(void)cluty;(void)twmx;(void)twmy;(void)twox;(void)twoy;(void)dax0;(void)day0;(void)dax1;(void)day1;
}
void gpu_vk_draw_semi(Core* core, const int* xs, const int* ys, const int* us, const int* vs, const unsigned char* rs,
                      const unsigned char* gs, const unsigned char* bs, int tpx, int tpy, int mode, int raw,
                      int clutx, int cluty, int twmx, int twmy, int twox, int twoy,
                      int dax0, int day0, int dax1, int day1, int blend) {
  (void)core;(void)xs;(void)ys;(void)us;(void)vs;(void)rs;(void)gs;(void)bs;(void)tpx;(void)tpy;(void)mode;(void)raw;
  (void)clutx;(void)cluty;(void)twmx;(void)twmy;(void)twox;(void)twoy;(void)dax0;(void)day0;(void)dax1;(void)day1;(void)blend;
}
#endif
