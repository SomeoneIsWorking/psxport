#include "core.h"
#include "game.h"    // Game / GpuVkState (per-instance render state)
#include "gpu_vk.h"  // public Core*-threaded API decls (wrappers below forward to core->game->gpu_vk)
#include "wide_margin_plan.h"      // renderer-only coverage for host-visible VRAM extension
#include "gpu_vk_present_policy.h"   // present_rebuild_decision — when a present must rebuild the composite
#include "gpu_vk_present_mode.h"     // preferred_present_mode — the sink must not stall the guest thread
#include "sbs_pane_layout.h"         // pane_letterbox / sbs_pane_rect — where each frame lands in the window
#include "present_plan.h"            // plan_present — the presented picture, decided identically in both legs
#include "fs_util.h"                 // Fs::ensureParentDirs — a capture must not silently write nothing
#include <errno.h>                   // strerror on a failed capture: the reason rides with the failure
#include <lucent/log.h>              // diagnostics: lucent::debug (channel-gated internally — never guard it)
#include "render_substrate.h"                    // Render::stats (RenderStats — was g_dbg_world_quads)
// gpu_vk.cpp — SDL3 GPU API present backend for the Tomba2Engine port.
//
// This is the PC-native renderer re-expressed on the SDL3 GPU API (SDL_gpu.h), replacing gpu_vk.cpp
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
// State model: the SDL_GPU device/window/pipelines live on class GpuDevice (gpu_vk_device.h),
// ONE per process — the first Game constructed claims it (GpuDevice::sInstance); the wrappers
// ignore Core* exactly as before. The per-frame batch state lives on GpuVkState (game.h).
#include <sys/stat.h>   // mkdir — the user-level GPU-fault marker dir
#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include "cfg.h"
#include "mods.h"             // Mods: per-Game mod toggles (wide/ires) — reached via game->mods
#include "video_plan.h"       // video_wide_native_w / video_ires_scale — resolution from the SINK, not a window
#include "overlay_glue.h"     // RmlUi mod/debug overlay hooks (init / event / per-frame / record)
#include "gpu_vk_shaders.h"  // generated: spv_g_present_{vert,frag} / spv_g_image_{vert,frag}
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#define VRAM_W 1024
#define VRAM_H 512

// ires_cap memory budget: the 3D geometry pass (when ires>1) renders into a SEPARATE VRAM_W*i x VRAM_H*i
// target set (GpuVkState::s_ires_color/s_ires_depth/s_ires_rgba — see ensure_ires_targets/render_geom
// below), not into the fixed 1024x512 VRAM canvas. Cap `i` by what that target set actually costs in GPU
// memory, not by whether a scaled FB would fit inside VRAM_W (the old clamp predates the ires target ever
// being built — docs/findings/render.md "ires modifier is a NO-OP"; that rationale no longer applies since
// nothing about the scaled render touches the fixed VRAM texture's width). Per-pixel cost: RG8 color (2B) +
// D32 depth (4B) + RGBA16F semi-blend intermediate (8B) = 14B/px, at (VRAM_W*i)*(VRAM_H*i) pixels.
#define IRES_MEM_BUDGET_BYTES (128u * 1024u * 1024u)   // per Game (SBS's two cores each own a set)
#define IRES_BYTES_PER_PX     14u                       // RG8(2) + D32(4) + RGBA16F(8)

// ---- device / window state — class GpuDevice (gpu_vk_device.h), one per process ---------------------
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
#define s_ires_downsample_pipe (gdev().s_ires_downsample_pipe)
#define s_semi_cover_pipe (gdev().s_semi_cover_pipe)
#define s_painter_tex_pipe (gdev().s_painter_tex_pipe)
#define s_painter_tri_pipe (gdev().s_painter_tri_pipe)
#define s_painter_composite_pipe (gdev().s_painter_composite_pipe)

// Painter GPU selftest boundary captures. These are null in shipping operation; the selftest owns the
// transfer buffers and asks render_geom to snapshot the local D32 immediately after authored replay and
// the main D32 immediately after composite, before later bands are allowed to reuse/clear main depth.
static SDL_GPUTransferBuffer* s_painter_test_local_depth = nullptr;
static SDL_GPUTransferBuffer* s_painter_test_main_depth = nullptr;
static SDL_GPUTransferBuffer* s_painter_test_local_color = nullptr;
#define s_sbs_tex      (gdev().s_sbs_tex)
#define s_sbs_xfer     (gdev().s_sbs_xfer)
#define s_sbs_w        (gdev().s_sbs_w)
#define s_sbs_h        (gdev().s_sbs_h)

// (Engine-owned screen fade moved to class ScreenFade at game/render/screen_fade.h. State lives in guest
// memory so it's per-Core / SBS-clean without needing per-instance C++ fields. Native present path
// reads ScreenFade::get(core) directly — see readback + PresentPC uniform builders below.)

// Present-pass fragment uniform: matches present.frag's `PC { ivec4 disp; ivec4 fade; }`.
struct PresentPC { int32_t disp[4]; int32_t fade[4]; int32_t fmt[4]; };   // fmt.x = 1 when the display is 24bpp

// ---- Pass 2: native 3D / textured raster (depth-ordered, REAL HW blend for semi) ----------------------
// The engine draws the world AND the 2D menus/HUD/sprites as textured/flat prims through the tee
// (gpu_vk_draw_tri/tritri/semi). Each present: upload CPU VRAM into the packed-1555 COLOR-TARGET image
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
// 2D (non-world) batch caps — bug #55: HUD/menu/2D-layer content is a small fraction of the 3D world's
// vertex count per frame; generous headroom without doubling the 3D buffers' GPU memory footprint.
#define TRI2D_CAP 32768
#define TEX2D_CAP 32768
#define NUM_BLEND_MODES GGS_NUM_BLEND_MODES   // PSX semi blend modes (0=avg 1=add 2=sub 3=add4)
// da[] = the guest draw-area clip, carried per-vertex exactly as TexVtx does. It is NOT optional
// padding: tri.frag discards outside it, so a vertex written without one clips to nothing.
// Every initializer below therefore sets it explicitly.
struct TriVtx { float x, y, r, g, b, ord, gouraud, dither; int32_t da[4]; };                   // 48 bytes
struct TexVtx { float x, y, u, v, r, g, b; int32_t tp[4], clut[4], tw[4], da[4]; float ord; };   // 96 bytes
// Batch buffers + counts moved onto GpuVkState (per-Core) — reach as `this->s_tri_buf` (cast from
// void* to TriVtx*) inside the methods. The `render_geom` free function below takes a `GpuVkState&`
// so it can pull the right instance's batches at present time.
// (raster/pipeline resources live on GpuDevice — see the shadow macros above)
static void create_3d_pipelines(void);
static void init_gpu(Game* game);
static void poll_quit(Game* game);

// ---- enable / windowed gates (mirror gpu_vk.cpp) ----------------------------------------------------
int gpu_vk_enabled(void) {
  if (s_gpu_on < 0) {
    // HEADLESS BY DEFAULT (2026-07-15): a window opens ONLY on an explicit PSXPORT_VK_WINDOW=1 (set by
    // run.sh, the user's interactive entry point). Every other invocation — agent gates, SBS smoke, probes,
    // CI — is headless without needing to remember PSXPORT_VK_HEADLESS. Rationale: agents kept forgetting the
    // flag and popping an intrusive window on the user's screen (and a windowed run auto-records over the
    // user's pad_session.pad). A forgotten flag now fails SAFE (headless), not intrusive. PSXPORT_VK_HEADLESS
    // still forces headless (back-compat) and wins over PSXPORT_VK_WINDOW.
    s_headless = (cfg_on("PSXPORT_VK_WINDOW") && !cfg_on("PSXPORT_VK_HEADLESS")) ? 0 : 1;
    s_gpu_on = 1;
  }
  return s_gpu_on;
}
extern "C" int gpu_windowed(void)   { return gpu_vk_enabled() && !s_headless; }

// Live window size in pixels (swapchain extent), used ONLY to answer "how big is the sink" in the
// windowed leg — see sink_size() below, which is what every consumer must ask.
//
// DO NOT REACH FOR THESE TO SIZE ANYTHING. They fall back to 320x240 when no window exists, and that
// fallback silently became the RESOLUTION INPUT for the AUTO internal-resolution scale and for
// ASPECT_AUTO's widened framebuffer: headless computed ires = round(240/240) = 1 where the same
// build in its window computed round(720/240) = 3, so a headless capture was not the user's picture.
// Both decisions now live in video_plan.h and take the SINK, which exists in both legs.
// (`gpu_has_window()` is gone with the same defect: its only caller was the frame pacer's
// `if (!gpu_has_window()) return`, which made headless unpaced. PSXPORT_NOPACE is the switch for
// that, and it always was.)
static int win_w(void) { int w = 320, h = 240; if (s_win) SDL_GetWindowSizeInPixels(s_win, &w, &h); return w > 0 ? w : 320; }
static int win_h(void) { int w = 320, h = 240; if (s_win) SDL_GetWindowSizeInPixels(s_win, &w, &h); return h > 0 ? h : 240; }

// The presented-picture target's format. RGBA8 rather than the swapchain's format so it exists
// identically in both legs (headless has no swapchain and therefore no swapchain format) and so the
// readback decode is one fixed rule.
static const SDL_GPUTextureFormat PRESENT_IMG_FMT = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;

// The window's creation size, defined ONCE and used both at SDL_CreateWindow and as the headless
// sink default, so the two cannot drift. (They were two hand-copied 960x720 literals; changing the
// window would silently have desynchronised the headless sink from it.)
enum { PRESENT_WINDOW_W = 960, PRESENT_WINDOW_H = 720 };

// ---- THE SINK SIZE — the only leg-dependent input to the presented picture, and deliberately so ------
// Windowed it is the live drawable; headless there is no drawable, so it is a configured size
// (PSXPORT_PRESENT_SINK=WxH) defaulting to the window's own creation size. A SIZE is a legitimate leg
// parameter — the composite is the same code either way, fed a number — whereas a leg-dependent code
// PATH is what made headless measurements describe a program the user never runs.
//
// COMPARABILITY, stated honestly rather than promised: the headless default matches the window's
// LOGICAL creation size, so a headless and a windowed present shot line up pixel for pixel only in a
// windowed, non-fullscreen, 1x-display-scale run. Under HiDPI the drawable is scaled (a 960x720
// window has a 1920x1440 drawable), PSXPORT_FULLSCREEN makes the sink the monitor, and the window is
// resizable — in all three the two shots differ in size. present_shot logs its sink size and leg on
// every capture precisely so that mismatch is visible in the output instead of assumed away.
//
// Resolved ONCE PER PROCESS. It used to re-read and re-validate the env var on every call, and it is
// called three times per present (once for the plan, twice via ensure_present_img) — so a malformed
// override emitted three warns per frame: MEASURED at 7990 of 8020 lines in a 45 s run, burying every
// other diagnostic in the log. The value cannot change during a run, so parsing it per frame bought
// nothing and cost the log.
static void sink_size(int* w, int* h) {
  if (!s_headless) { *w = win_w(); *h = win_h(); return; }
  static int cw = 0, ch = 0;
  if (!cw) {
    cw = PRESENT_WINDOW_W; ch = PRESENT_WINDOW_H;
    const char* e = cfg_str("PSXPORT_PRESENT_SINK");
    int pw = 0, ph = 0;
    if (e && sscanf(e, "%dx%d", &pw, &ph) == 2 && pw > 0 && ph > 0) { cw = pw; ch = ph; }
    else if (e)
      // A malformed override must not silently become the default: a run measured at 960x720 while
      // the operator believed it was measuring 1920x1080 is a false negative with true numbers. Once
      // per process is enough to say so — and quiet enough to still be readable.
      lucent::warn("gpu_vk", "PSXPORT_PRESENT_SINK=\"{}\" is not WxH — using the default {}x{}", e, cw, ch);
  }
  *w = cw; *h = ch;
}

// ---- PC-native widescreen accessors (kept; the engine projection reads these) -----------------------
//
// THE WIDE WIDTH SCALES FROM THE GAME'S OWN 4:3 WIDTH, not from a hardcoded 320. The fixed returns
// here were 320-based, which silently assumed every PSX game renders a 320-wide framebuffer. Plenty
// do not: Spyro the Dragon runs 512x240, so asking for 16:9 returned 428 — NARROWER than the game's
// own 4:3 frame. Widescreen would have cropped the picture instead of widening it, and the failure
// would have looked like a broken renderer rather than a wrong constant.
//
// The 320 case is unchanged BY CONSTRUCTION: the aspect entries are expressed as the same targets
// they always returned for a 320-wide game, and everything else scales by (native/320). 16:9 keeps
// 428 rather than the arithmetic 426.67 because that is the value this framework has always used and
// consumers are tuned to it; scaling preserves that choice instead of silently re-deriving it.
// The GAME'S OWN 4:3 framebuffer width — the denominator every widescreen ratio scales from. One
// definition, because "320" open-coded at each consumer is exactly the defect a0b88136 / 94e52472 /
// 2c54ce71 / 6dda8528 each had to fix separately.
int gpu_vk_native_w(Core* c) { const Game* g = c->game; return g->gpu.s_disp_w > 0 ? g->gpu.s_disp_w : 320; }

// The inputs the resolution decisions (video_plan.h) take. THE SINK, never the window — see the
// banner on win_w()/win_h(). `iresCap` is filled by the one caller that needs it.
static VideoInputs video_inputs(const Game* game, int iresCap) {
  VideoInputs v;
  sink_size(&v.sinkW, &v.sinkH);
  v.nativeW  = game->gpu.s_disp_w;
  // THE ENHANCEMENT GATE, at the one place aspect + internal resolution enter the video plan. On a pure
  // render path (RenderPath::Gte / Psx) the picture is 4:3 at 1x no matter what the user's saved settings
  // say — USER 2026-08-11: the guest render stays pure, and fps60/wide/ires are native-only. Gating the
  // READ, rather than mutating Mods the way Game::setOracle's forceNeutral() does, is what lets the path
  // be toggled live at the REPL and give the user their own settings back on the way out.
  const bool enh = game->core.rsub.mode.enhancementsAllowed();
  v.aspect   = enh ? game->mods.aspect : ASPECT_4_3;
  v.modsIres = enh ? game->mods.ires   : 1;
  v.iresCap  = iresCap;
  v.vramW    = VRAM_W;
  return v;
}

static int wide_native_w(const Game* game) { return video_wide_native_w(video_inputs(game, 1)); }
// Per-core: widescreen is a PC enhancement, so it exists only on a path that allows one. This used to
// read `!g->oracle` — the oracle BUNDLE (substrate gameplay + pure picture) standing in for the precise
// question, which meant `PSXPORT_RENDER_PATH=gte` alone (no ORACLE) would have widened a picture that is
// supposed to be pure. RenderMode::enhancementsAllowed IS the question, and it is per-Core, so one
// process still holds a wide user core beside a pure oracle core (SBS honesty).
int gpu_vk_wide_engine(Core* c)     { return c->game->mods.aspect != ASPECT_4_3 && c->rsub.mode.enhancementsAllowed(); }
int gpu_vk_wide_engine_ofx(Core* c) { return wide_native_w(c->game) / 2; }
int gpu_vk_wide_engine_w(Core* c)   { return wide_native_w(c->game); }
void gpu_vk_video_status(Core* c, int* native_w, int* ires, int* fbw, int* fbh, int* ww, int* wh, int* ires_cap) {
  // Cap = largest i whose ires target set (VRAM_W*i x VRAM_H*i, IRES_BYTES_PER_PX/px) stays under
  // IRES_MEM_BUDGET_BYTES — a real memory budget, independent of aspect (the scaled render is a standalone
  // target now, not squeezed into the fixed VRAM canvas — see the ires_cap comment above). 8x is a hard
  // ceiling regardless of budget: past that the internal resolution exceeds any plausible display, so more
  // headroom just wastes GPU memory for no visible gain.
  double budget_px = (double)IRES_MEM_BUDGET_BYTES / IRES_BYTES_PER_PX;
  int cap = (int)sqrt(budget_px / ((double)VRAM_W * VRAM_H));
  if (cap < 1) cap = 1; if (cap > 8) cap = 8;
  // mods.ires: 0 = AUTO (derive the scale from the SINK's height, ~round(h/240)), 1..cap = fixed.
  // It used to derive from win_h(), which is 240 when there is no window — so a headless run rendered
  // at 1x where the same build in its window rendered at 3x, and headless captures were not the
  // user's picture. Both decisions moved to video_plan.h and take the sink, which exists in both legs.
  const VideoInputs v = video_inputs(c->game, cap);
  const int nw = video_wide_native_w(v);
  const int i  = video_ires_scale(v);
  if (native_w) *native_w = nw; if (ires) *ires = i;   if (fbw) *fbw = nw * i;
  if (fbh) *fbh = PRESENT_NATIVE_LINES * i;
  // The SINK, reported as the sink, in both legs — the overlay's "window size" row used to read
  // 320x240 headless, which is neither the window's size nor the size anything was composed for.
  if (ww) *ww = v.sinkW;        if (wh) *wh = v.sinkH;
  if (ires_cap) *ires_cap = cap;
}
// Unused stub (no call sites) — the ires-scaled 3D target that actually exists now (GpuVkState::
// s_ires_color/ensure_ires_targets, render_geom below) is a per-present in/out blit around Pass A/B, not a
// standing "frame renders via a scratch FB" mode this accessor implies. Left at 0; not wired to anything.
int GpuVkState::frame_via_fb() { return 0; }

// ---- SDL_GPU helpers --------------------------------------------------------------------------------
#define GPUCHK(p, what) do { if (!(p)) { lucent::error("gpu_vk", "{} failed: {}", what, SDL_GetError()); exit(2); } } while (0)

// ── GPU-FAULT LATCH — a failed submit ENDS this process's GPU work, permanently ────────────────────
//
// A GPU hang is not a crash you retry. The fault belongs to the whole card: the kernel resets it, and
// the process that loses is the one drawing the USER'S DESKTOP. The global rule
// (~/.claude/CLAUDE.md, 2026-08-12) was written after a render path hung the graphics ring seven times
// in one session and took the desktop down twice, the second time hard enough to need a reboot — and
// every reset after the FIRST was avoidable, caused by continuing to submit into a card being reset.
//
// Before this, all NINE plain SDL_SubmitGPUCommandBuffer call sites in this file DISCARDED the return
// value, so nothing could stop the frame loop from submitting again — and the four fence-returning
// submits then waited on the result with NO wall-clock bound, which holds the process open straight
// through a kernel reset. That is the exact "fifteen consecutive
// failed submits" shape the rule names, and it was one bad frame away from happening here.
//
// WHY A LATCH RATHER THAN A RETRY: the rule is that the first device loss stops all GPU work, not that
// it be handled gracefully. There is no recovery a game process can perform that is worth the risk of a
// second ring reset, so this is deliberately one-way — once tripped it never clears for the lifetime of
// the process. It does NOT exit(): the run continues without submitting so the caller can still write
// its diagnostics, and gpu_submit_failed() lets a gate report an honest "the GPU stopped" instead of a
// silent black capture that reads like a rendering bug.
static bool s_gpu_faulted = false;

bool gpu_submit_failed() { return s_gpu_faulted; }

// Submit and WAIT, with a wall-clock bound. Returns false (already latched) if the submit failed or the
// wait timed out; the caller must then treat its readback as unavailable rather than reading stale bytes.
//
// WHY THE POLL LOOP: SDL_WaitForGPUFences takes no timeout, so calling it is an UNBOUNDED wait — exactly
// what the rule forbids, because a process sitting in one holds itself open through the card's reset.
// SDL_QueryGPUFence is the non-blocking form, so the bound is built from it. The budget is generous
// (seconds, not milliseconds): a legitimate readback of a full VRAM image on a loaded machine can take
// far longer than a frame, and a timeout that fires on slowness rather than death would latch the
// renderer off during normal use — a false positive here costs the user their picture.
// ── CROSS-PROCESS GPU-FAULT MARKER — the NEXT process refuses to start ─────────────────────────────
//
// The in-process latch above protects ONE run. The recorded damage came from the runs AFTER the first
// fault: seven ring resets in one session, two of them taking the desktop down, because each new process
// started clean and submitted into a card the kernel was still resetting. So the fault has to outlive the
// process that saw it.
//
// USER-LEVEL, NOT PER-REPO, and that is the whole point: the fault belongs to the CARD, not to a game.
// A marker under one game's scratch/ would let the next launch of a DIFFERENT game walk into it — and the
// workspace has four of them sharing one GPU.
//
// CLEARING IT IS THE USER'S DECISION, never ours. The rule says diagnose statically and ask the user,
// whose machine it is, so the refusal prints the exact command to clear and a knob to override for one
// run. It must never self-clear on a timer or after "looks fine now": that is the retry this exists to
// prevent, one level up.
static const char* gpu_fault_marker_path() {
  static char path[512];
  static bool built = false;
  if (built) return path;
  built = true;
  const char* xdg = getenv("XDG_STATE_HOME");
  const char* home = getenv("HOME");
  if (xdg && *xdg)       snprintf(path, sizeof path, "%s/psxport", xdg);
  else if (home && *home) snprintf(path, sizeof path, "%s/.local/state/psxport", home);
  else                    { path[0] = 0; return path; }   // no writable home: nothing to persist to
  // Best-effort mkdir -p of the two levels we may have added; failure is reported by the open() below.
  { char tmp[512]; snprintf(tmp, sizeof tmp, "%s", path);
    for (char* q = tmp + 1; *q; q++) if (*q == '/') { *q = 0; mkdir(tmp, 0755); *q = '/'; }
    mkdir(tmp, 0755); }
  const size_t n = strlen(path);
  snprintf(path + n, sizeof path - n, "/gpu_fault");
  return path;
}

// Record the fault for the next process. Called from the latch; never from anywhere else.
static void gpu_fault_persist(const char* where, const char* detail) {
  const char* path = gpu_fault_marker_path();
  if (!path || !*path) {
    lucent::warn("gpu_vk", "cannot persist the GPU fault (no XDG_STATE_HOME or HOME) — this run is "
                           "latched off, but the NEXT process will start clean and can submit into the "
                           "same card. Do not launch another GPU run until the machine is known good.");
    return;
  }
  FILE* f = fopen(path, "wb");
  if (!f) {
    lucent::warn("gpu_vk", "could not write the GPU-fault marker {} — the next process will NOT refuse to "
                           "start. Treat the machine as suspect manually.", path);
    return;
  }
  fprintf(f, "psxport GPU fault\nwhere: %s\ndetail: %s\n", where ? where : "?", detail ? detail : "?");
  fclose(f);
  lucent::error("gpu_vk", "GPU fault PERSISTED to {} — every psxport process will now REFUSE to bring up "
                          "the GPU until you clear it: rm {}", path, path);
}

// Preflight: refuse to create a device while a previous fault stands. Returns false if we must not start.
static bool gpu_fault_preflight() {
  const char* path = gpu_fault_marker_path();
  if (!path || !*path) return true;
  FILE* f = fopen(path, "rb");
  if (!f) return true;                       // no marker: normal start
  char body[512] = {0};
  const size_t n = fread(body, 1, sizeof body - 1, f);
  body[n] = 0;
  fclose(f);
  if (cfg_on("PSXPORT_GPU_FAULT_OVERRIDE")) {
    lucent::warn("gpu_vk", "a GPU fault is on record at {} and PSXPORT_GPU_FAULT_OVERRIDE is set — starting "
                           "anyway ON YOUR SAY-SO. If the card is still unhealthy this run can reset the "
                           "graphics ring again. Recorded fault:\n{}", path, body);
    return true;
  }
  lucent::error("gpu_vk",
                "REFUSING TO START THE GPU: a previous run recorded a GPU fault and it has not been "
                "cleared. Continuing is what turns one lost frame into a desktop-killing ring reset — the "
                "recorded incident was seven resets in one session, and every one after the first came "
                "from a process that started clean.\n"
                "  marker : {}\n{}"
                "  clear it (your call, your machine): rm {}\n"
                "  or override for a single run:       PSXPORT_GPU_FAULT_OVERRIDE=1\n"
                "  check the kernel first:             journalctl -k -b | grep -iE 'gpu reset|ring|drm'",
                path, body, path);
  return false;
}

static bool gpu_submit_and_wait(SDL_GPUCommandBuffer* cmd, const char* where) {
  if (!cmd) return false;
  SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
  if (!fence) {
    if (!s_gpu_faulted) {
      s_gpu_faulted = true;
      lucent::error("gpu_vk", "GPU SUBMIT (fenced) FAILED at {}: {} — LATCHING THE RENDERER OFF for the "
                              "rest of this process; see the latch banner above.", where, SDL_GetError());
      gpu_fault_persist(where, SDL_GetError());
    }
    return false;
  }
  static const Uint64 kBudgetMs = 5000;
  const Uint64 t0 = SDL_GetTicks();
  for (;;) {
    if (SDL_QueryGPUFence(s_dev, fence)) { SDL_ReleaseGPUFence(s_dev, fence); return true; }
    const Uint64 waited = SDL_GetTicks() - t0;
    if (waited >= kBudgetMs) {
      SDL_ReleaseGPUFence(s_dev, fence);
      if (!s_gpu_faulted) {
        s_gpu_faulted = true;
        lucent::error("gpu_vk",
                      "GPU FENCE at {} DID NOT SIGNAL within {} ms — treating the device as hung and "
                      "LATCHING THE RENDERER OFF. Waiting longer is what keeps this process alive through "
                      "a kernel ring reset; the readback that wanted this fence is NOT available and its "
                      "caller must not read the transfer buffer.", where, (unsigned long long)kBudgetMs);
        gpu_fault_persist(where, "fence did not signal within the wall-clock budget");
      }
      return false;
    }
    SDL_Delay(1);
  }
}

// Submit, and latch on failure. EVERY submit in this file goes through here — a raw
// SDL_SubmitGPUCommandBuffer is the defect this exists to remove, so do not add one back.
static bool gpu_submit(SDL_GPUCommandBuffer* cmd, const char* where) {
  if (!cmd) return false;
  if (SDL_SubmitGPUCommandBuffer(cmd)) return true;
  if (!s_gpu_faulted) {
    s_gpu_faulted = true;
    lucent::error("gpu_vk",
                  "GPU SUBMIT FAILED at {}: {} — LATCHING THE RENDERER OFF for the rest of this "
                  "process. A submit failure means the device is gone or being reset by the kernel, and "
                  "continuing to submit into it is what escalates one lost frame into a desktop-killing "
                  "ring reset. This run will keep executing WITHOUT drawing; any capture taken from here "
                  "on is not a picture of the game.", where, SDL_GetError());
    gpu_fault_persist(where, SDL_GetError());
  }
  return false;
}


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

// A fullscreen-triangle pipeline (no vertex input) sampling fragment texture(s) into an OFFSCREEN target
// of `fmt` — used for the decode (1555 -> float RGBA) and encode (float RGBA -> 1555) passes around the
// real-HW-blend semi step, and (with num_uniforms=1, num_samplers=2 — color + depth, bug #55 coverage
// gate) the ires box-filter downsample.
static SDL_GPUGraphicsPipeline* make_fullscreen_offscreen_pipeline(const uint32_t* vs_code, unsigned vs_len,
                                                                    const uint32_t* fs_code, unsigned fs_len,
                                                                    SDL_GPUTextureFormat fmt, Uint32 num_uniforms = 0,
                                                                    Uint32 num_samplers = 1) {
  SDL_GPUShader* vs = make_shader(vs_code, vs_len, SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
  SDL_GPUShader* fs = make_shader(fs_code, fs_len, SDL_GPU_SHADERSTAGE_FRAGMENT, num_samplers, num_uniforms);
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
// depth target. `depth_write` distinguishes opaque (test+write) from semi (test, no write). `depth_only`
// (bug #55 part 3, s_semi_cover_pipe) drops the color target entirely — a pure depth-marking pass.
static SDL_GPUGraphicsPipeline* make_geom_pipeline(const uint32_t* vs_code, unsigned vs_len,
    const uint32_t* fs_code, unsigned fs_len, Uint32 pitch,
    const SDL_GPUVertexAttribute* attrs, Uint32 n_attr, Uint32 num_samplers, bool depth_write,
    Uint32 num_uniforms = 0, bool depth_only = false,
    SDL_GPUCompareOp compare = SDL_GPU_COMPAREOP_GREATER_OR_EQUAL) {
  SDL_GPUShader* vs = make_shader(vs_code, vs_len, SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
  SDL_GPUShader* fs = make_shader(fs_code, fs_len, SDL_GPU_SHADERSTAGE_FRAGMENT, num_samplers, num_uniforms);
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
  gp.depth_stencil_state.compare_op = compare;
  gp.target_info.color_target_descriptions = depth_only ? nullptr : &ct;
  gp.target_info.num_color_targets = depth_only ? 0 : 1;
  gp.target_info.has_depth_stencil_target = true;
  gp.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
  SDL_GPUGraphicsPipeline* p = SDL_CreateGPUGraphicsPipeline(s_dev, &gp);
  GPUCHK(p, "CreateGPUGraphicsPipeline(geom)");
  SDL_ReleaseGPUShader(s_dev, vs); SDL_ReleaseGPUShader(s_dev, fs);
  return p;
}

static SDL_GPUGraphicsPipeline* make_painter_composite_pipeline() {
  SDL_GPUShader* vs=make_shader(spv_g_fsq_vert,spv_g_fsq_vert_len,SDL_GPU_SHADERSTAGE_VERTEX,0,0);
  SDL_GPUShader* fs=make_shader(spv_g_painter_composite_frag,spv_g_painter_composite_frag_len,SDL_GPU_SHADERSTAGE_FRAGMENT,2,0);
  SDL_GPUColorTargetDescription ct={}; ct.format=SDL_GPU_TEXTUREFORMAT_R8G8_UNORM;
  SDL_GPUGraphicsPipelineCreateInfo gp={}; gp.vertex_shader=vs; gp.fragment_shader=fs;
  gp.primitive_type=SDL_GPU_PRIMITIVETYPE_TRIANGLELIST; gp.rasterizer_state.fill_mode=SDL_GPU_FILLMODE_FILL;
  gp.rasterizer_state.cull_mode=SDL_GPU_CULLMODE_NONE; gp.multisample_state.sample_count=SDL_GPU_SAMPLECOUNT_1;
  gp.depth_stencil_state.enable_depth_test=true; gp.depth_stencil_state.enable_depth_write=true;
  gp.depth_stencil_state.compare_op=SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;
  gp.target_info.color_target_descriptions=&ct; gp.target_info.num_color_targets=1;
  gp.target_info.has_depth_stencil_target=true; gp.target_info.depth_stencil_format=SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
  SDL_GPUGraphicsPipeline* p=SDL_CreateGPUGraphicsPipeline(s_dev,&gp); GPUCHK(p,"painter composite pipeline");
  SDL_ReleaseGPUShader(s_dev,vs); SDL_ReleaseGPUShader(s_dev,fs); return p;
}

// One real-HW-blend semi pipeline per PSX blend mode, targeting the float RGBA intermediate (s_color_rgba).
// Shares trisemi_hw.frag (and tritex.vert's vertex layout) across all 4 — only the blend state differs.
// See trisemi_hw.frag's header comment for the derivation: src_color_factor=ONE always; dst_color_factor=
// SRC_ALPHA reads the shader's own per-fragment STP output (0=opaque, 1=real PSX blend); the op is ADD for
// avg/add/add4 and REVERSE_SUBTRACT for sub. Depth: test against the opaque pass's depth, never write/clear.
static SDL_GPUGraphicsPipeline* make_semi_pipeline(int mode,
    const SDL_GPUVertexAttribute* attrs, Uint32 n_attr, Uint32 pitch) {
  SDL_GPUShader* vs = make_shader(spv_g_tritex_vert, spv_g_tritex_vert_len, SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
  SDL_GPUShader* fs = make_shader(spv_g_trisemi_hw_frag, spv_g_trisemi_hw_frag_len, SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);   // +1 fragment uniform: ires scale
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
void GpuVkState::ensure_targets() {
  if (s_have_3d) return;
  s_have_3d = 1;
  SDL_GPUTextureCreateInfo vi = {};
  vi.type = SDL_GPU_TEXTURETYPE_2D; vi.format = SDL_GPU_TEXTUREFORMAT_R8G8_UNORM;
  vi.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
  vi.width = VRAM_W; vi.height = VRAM_H; vi.layer_count_or_depth = 1; vi.num_levels = 1;
  s_vram_tex = SDL_CreateGPUTexture(s_dev, &vi); GPUCHK(s_vram_tex, "CreateGPUTexture(VRAM)");
  // A brand-new texture holds nothing we know about, so the first present uploads all of it. After
  // that the composite is persistent and only guest writes touch it (vram_dirty.h). setCanvas() also
  // arms the full upload, so the order of these two lines does not matter — say both anyway.
  s_dirty.setCanvas(VRAM_W, VRAM_H);
  s_dirty.markAll();
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
  mkbuf(sizeof(TexVtx) * TEX_CAP, &s_painter_tex_vbuf, &s_painter_tex_xfer);
  mkbuf(sizeof(TriVtx) * TRI_CAP, &s_painter_tri_vbuf, &s_painter_tri_xfer);
  for (int m = 0; m < NUM_BLEND_MODES; m++) {
    mkbuf(sizeof(TexVtx) * TEX_CAP, &s_semi_vbuf[m], &s_semi_xfer[m]);
  }
  // 2D (non-world) buffers — bug #55: one independent set per band (GGS_2D_BG/GGS_2D_FG) so 2D content
  // never shares a vertex buffer with 3D-world geometry (see gpu_vk_internal.h).
  for (int band = 0; band < GGS_NUM_2D_BANDS; band++) {
    mkbuf(sizeof(TriVtx) * TRI2D_CAP, &s_tri2d_vbuf[band], &s_tri2d_xfer[band]);
    mkbuf(sizeof(TexVtx) * TEX2D_CAP, &s_tex2d_vbuf[band], &s_tex2d_xfer[band]);
    for (int m = 0; m < NUM_BLEND_MODES; m++) {
      mkbuf(sizeof(TexVtx) * TEX2D_CAP, &s_semi2d_vbuf[band][m], &s_semi2d_xfer[band][m]);
    }
  }
  // CPU-side batch buffers stay lazily allocated per draw call (ggs_alloc_batches).
  // Float RGBA semi-blend intermediate (decode target / real-HW-blend target / encode source).
  SDL_GPUTextureCreateInfo cti = {}; cti.type = SDL_GPU_TEXTURETYPE_2D; cti.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
  cti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
  cti.width = VRAM_W; cti.height = VRAM_H; cti.layer_count_or_depth = 1; cti.num_levels = 1;
  s_color_rgba = SDL_CreateGPUTexture(s_dev, &cti); GPUCHK(s_color_rgba, "color_rgba tex");
}

void GpuVkState::ensure_painter_targets(int w,int h) {
  if (s_painter_color && s_painter_w==w && s_painter_h==h) return;
  if (s_painter_color) SDL_ReleaseGPUTexture(s_dev,s_painter_color);
  if (s_painter_depth) SDL_ReleaseGPUTexture(s_dev,s_painter_depth);
  SDL_GPUTextureCreateInfo ci={}; ci.type=SDL_GPU_TEXTURETYPE_2D; ci.format=SDL_GPU_TEXTUREFORMAT_R8G8_UNORM;
  ci.usage=SDL_GPU_TEXTUREUSAGE_COLOR_TARGET|SDL_GPU_TEXTUREUSAGE_SAMPLER; ci.width=w; ci.height=h; ci.layer_count_or_depth=1; ci.num_levels=1;
  s_painter_color=SDL_CreateGPUTexture(s_dev,&ci); GPUCHK(s_painter_color,"painter color");
  SDL_GPUTextureCreateInfo di={}; di.type=SDL_GPU_TEXTURETYPE_2D; di.format=SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
  di.usage=SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET|SDL_GPU_TEXTUREUSAGE_SAMPLER; di.width=w; di.height=h; di.layer_count_or_depth=1; di.num_levels=1;
  s_painter_depth=SDL_CreateGPUTexture(s_dev,&di); GPUCHK(s_painter_depth,"painter depth"); s_painter_w=w; s_painter_h=h;
}

// ires (internal resolution) scaled 3D target: lazily (re)built to VRAM_W*i x VRAM_H*i whenever the live
// scale changes (RmlUi's ires toggle mutates mods.ires mid-run — see rmlui_overlay.cpp id=="ires"). i<=1
// tears any existing targets down and holds nothing (render_geom's i==1 path never reaches these fields —
// the direct-to-s_vram_tex bypass is unconditional). Same release-then-recreate shape as sbs_make_tex /
// img_make_tex above.
void GpuVkState::ensure_ires_targets(int i) {
  if (i < 1) i = 1;
  if (s_ires_scale == i) return;
  if (s_ires_color) { SDL_ReleaseGPUTexture(s_dev, s_ires_color); s_ires_color = nullptr; }
  if (s_ires_depth) { SDL_ReleaseGPUTexture(s_dev, s_ires_depth); s_ires_depth = nullptr; }
  if (s_ires_rgba)  { SDL_ReleaseGPUTexture(s_dev, s_ires_rgba);  s_ires_rgba  = nullptr; }
  s_ires_scale = i;
  if (i <= 1) return;   // 1x: no scaled target needed — render_geom stays on the direct s_vram_tex path
  int w = VRAM_W * i, h = VRAM_H * i;
  SDL_GPUTextureCreateInfo ci = {}; ci.type = SDL_GPU_TEXTURETYPE_2D; ci.format = SDL_GPU_TEXTUREFORMAT_R8G8_UNORM;
  ci.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
  ci.width = w; ci.height = h; ci.layer_count_or_depth = 1; ci.num_levels = 1;
  s_ires_color = SDL_CreateGPUTexture(s_dev, &ci); GPUCHK(s_ires_color, "ires color tex");
  SDL_GPUTextureCreateInfo di = {}; di.type = SDL_GPU_TEXTURETYPE_2D; di.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
  // SAMPLER (in addition to DEPTH_STENCIL_TARGET): bug #55's coverage-gated composite-back
  // (ires_downsample.frag's u_depth) reads this target as a texture to decide which destination pixels
  // the 3D pass actually touched this frame — see render_geom's composite-back call site.
  di.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
  di.width = w; di.height = h; di.layer_count_or_depth = 1; di.num_levels = 1;
  s_ires_depth = SDL_CreateGPUTexture(s_dev, &di); GPUCHK(s_ires_depth, "ires depth tex");
  SDL_GPUTextureCreateInfo rti = {}; rti.type = SDL_GPU_TEXTURETYPE_2D; rti.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
  rti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
  rti.width = w; rti.height = h; rti.layer_count_or_depth = 1; rti.num_levels = 1;
  s_ires_rgba = SDL_CreateGPUTexture(s_dev, &rti); GPUCHK(s_ires_rgba, "ires rgba tex");
  lucent::info("gpu_vk", "ires targets (re)built: {}x{} (scale={})", w, h, i);
}

// Build the 3D raster PIPELINES (shared device objects; the render targets are per-Game). Once.
static void create_3d_pipelines(void) {
  if (gdev().s_pipes_3d) return;
  static const SDL_GPUVertexAttribute tri_attr[] = {
    { 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 0 },
    { 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 8 },
    { 2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, 20 },
    { 3, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, 24 },
    { 4, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, 28 },
    { 5, 0, SDL_GPU_VERTEXELEMENTFORMAT_INT4,  32 },   // draw-area clip (tri.frag discards outside)
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
                                     sizeof(TriVtx), tri_attr, 6, 0, true, 1);   // +1 fragment uniform: ires scale (the draw-area clip needs it)
  s_tritex_pipe = make_geom_pipeline(spv_g_tritex_vert, spv_g_tritex_vert_len, spv_g_tritex_frag, spv_g_tritex_frag_len,
                                     sizeof(TexVtx), tex_attr, 8, 1, true, 1);   // +1 fragment uniform: ires scale (PC.scale)
  for (int m = 0; m < NUM_BLEND_MODES; m++) s_semi_pipe[m] = make_semi_pipeline(m, tex_attr, 8, sizeof(TexVtx));
  // bug #55 (part 3): depth-only stamp so translucent-only 3D coverage still marks the depth buffer the
  // ires composite-back's coverage gate reads (see semi_cover.frag). depth_write=true, same GREATER_OR_EQUAL
  // compare as Pass A/opaque; no color target at all (depth_only=true).
  s_semi_cover_pipe = make_geom_pipeline(spv_g_tritex_vert, spv_g_tritex_vert_len, spv_g_semi_cover_frag, spv_g_semi_cover_frag_len,
                                     sizeof(TexVtx), tex_attr, 8, 1, true, 1, true);   // +1 fragment uniform: ires scale; depth_only
  s_painter_tex_pipe = make_geom_pipeline(spv_g_tritex_vert,spv_g_tritex_vert_len,spv_g_tritex_frag,spv_g_tritex_frag_len,
                                     sizeof(TexVtx),tex_attr,8,1,true,1,false,SDL_GPU_COMPAREOP_ALWAYS);
  s_painter_tri_pipe = make_geom_pipeline(spv_g_tri_vert,spv_g_tri_vert_len,spv_g_painter_tri_frag,spv_g_painter_tri_frag_len,
                                     sizeof(TriVtx),tri_attr,6,0,true,1,false,SDL_GPU_COMPAREOP_ALWAYS);
  s_painter_composite_pipe = make_painter_composite_pipeline();
  s_decode_pipe = make_fullscreen_offscreen_pipeline(spv_g_fsq_vert, spv_g_fsq_vert_len,
                                                      spv_g_decode_frag, spv_g_decode_frag_len,
                                                      SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT);
  s_encode_pipe = make_fullscreen_offscreen_pipeline(spv_g_fsq_vert, spv_g_fsq_vert_len,
                                                      spv_g_encode_frag, spv_g_encode_frag_len,
                                                      SDL_GPU_TEXTUREFORMAT_R8G8_UNORM);
  s_ires_downsample_pipe = make_fullscreen_offscreen_pipeline(spv_g_fsq_vert, spv_g_fsq_vert_len,
                                                      spv_g_ires_downsample_frag, spv_g_ires_downsample_frag_len,
                                                      SDL_GPU_TEXTUREFORMAT_R8G8_UNORM, 1, 1);   // +1 uniform: box side `n`; 1 sampler: the composite C (plain box downsample for the headless shot)
  gdev().s_pipes_3d = 1;
  lucent::info("gpu_vk", "3D raster up (RG8 color target + D32 depth, real HW-blend semi via float intermediate; per-Game targets)");
}

static void init_gpu(Game* game) {
  s_inited = 1;
  // SDL_GPU requires the video subsystem even headless (the device is created against it; we just don't
  // open a window or claim a swapchain).
  if (!SDL_Init(SDL_INIT_VIDEO)) { lucent::error("gpu_vk", "SDL_Init(VIDEO) failed: {}", SDL_GetError()); exit(2); }
  if (!s_headless) {
    int fullscreen = cfg_on("PSXPORT_FULLSCREEN")
                  || (cfg_str("PSXPORT_WINDOWED") && atoi(cfg_str("PSXPORT_WINDOWED")) == 0);
    SDL_WindowFlags flags = fullscreen ? SDL_WINDOW_FULLSCREEN : SDL_WINDOW_RESIZABLE;
    // GameConfig::windowTitle — never a game name in the framework. The fallback is deliberately
    // self-evidently wrong: a port that forgets to set it must look untitled, not look like Tomba!2.
    const char* title = (game->core.cfg && game->core.cfg->windowTitle) ? game->core.cfg->windowTitle
                                                                       : "psxport (untitled game)";
    s_win = SDL_CreateWindow(title, PRESENT_WINDOW_W, PRESENT_WINDOW_H, flags);
    GPUCHK(s_win, "SDL_CreateWindow");
  }
  // Create the GPU device (SPIR-V shaders; let SDL pick the optimal driver — Vulkan on Linux, Metal on Mac).
  // A recorded fault from a PREVIOUS process stops us before we ever touch the device.
  if (!gpu_fault_preflight()) { s_gpu_faulted = true; exit(3); }
  s_dev = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, cfg_on("PSXPORT_GPU_DEBUG") ? true : false, NULL);
  GPUCHK(s_dev, "SDL_CreateGPUDevice");
  // SDL_GetGPUDeviceDriver returns NULL on an invalid device — a null const char* is UB for std::format.
  { const char* drv = SDL_GetGPUDeviceDriver(s_dev);
    lucent::info("gpu_vk", "SDL_GPU device up (driver: {})", drv ? drv : "(null)"); }
  if (!s_headless) {
    GPUCHK(SDL_ClaimWindowForGPUDevice(s_dev, s_win), "SDL_ClaimWindowForGPUDevice");
    // The swapchain must NOT stall the guest thread. A freshly claimed window keeps SDL's DEFAULT
    // present mode, VSYNC, under which SDL_WaitAndAcquireGPUSwapchainTexture (show_present_image) sleeps
    // until the next vblank — on the one thread that runs the guest, the CD pump, MDEC and the DMA
    // completions. Ask for a non-blocking mode instead; see gpu_vk_present_mode.h for the measurement.
    const SDL_GPUPresentMode want =
        preferred_present_mode(SDL_WindowSupportsGPUPresentMode(s_dev, s_win, SDL_GPU_PRESENTMODE_MAILBOX),
                               SDL_WindowSupportsGPUPresentMode(s_dev, s_win, SDL_GPU_PRESENTMODE_IMMEDIATE));
    const bool set_ok = SDL_SetGPUSwapchainParameters(s_dev, s_win, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, want);
    if (!set_ok)
      lucent::warn("gpu_vk", "SDL_SetGPUSwapchainParameters({}) failed: {}", present_mode_name(want), SDL_GetError());
    // The mode actually IN EFFECT, not the one asked for: on failure the swapchain keeps SDL's default,
    // which is VSYNC. Unguarded info — a normal windowed run must state whether its sink blocks.
    const SDL_GPUPresentMode got = set_ok ? want : SDL_GPU_PRESENTMODE_VSYNC;
    lucent::info("gpu_vk", "swapchain present mode: {}{}", present_mode_name(got),
                 present_mode_blocks_caller(got) ? " (BLOCKING — every present stalls the guest thread until vblank)" : "");
    s_swap_fmt = SDL_GetGPUSwapchainTextureFormat(s_dev, s_win);
  }

  // (the guest-VRAM image + its upload/download staging are PER-GAME now — GpuVkState::ensure_targets.
  //  VRAM is stored R8G8_UNORM, not R16_UINT: SDL_GPU forbids SAMPLER usage on integer formats, so the
  //  uint16 LE 1555 word rides as two 8-bit channels and the shaders reconstruct it.)
  SDL_GPUSamplerCreateInfo si = {};
  si.min_filter = SDL_GPU_FILTER_NEAREST; si.mag_filter = SDL_GPU_FILTER_NEAREST;
  si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
  s_samp_nearest = SDL_CreateGPUSampler(s_dev, &si); GPUCHK(s_samp_nearest, "CreateGPUSampler(nearest)");
  si.min_filter = SDL_GPU_FILTER_LINEAR; si.mag_filter = SDL_GPU_FILTER_LINEAR;
  s_samp_linear = SDL_CreateGPUSampler(s_dev, &si); GPUCHK(s_samp_linear, "CreateGPUSampler(linear)");

  // The PRESENT pipeline targets s_present_img, whose format is fixed and leg-independent, so it is
  // created in BOTH legs — headless composites the same picture, it just never blits it to a window.
  // (It used to be windowed-only, on the reasoning that "headless never runs a render pass". Headless
  // not running the present pass was the bug, not a premise.)
  s_present_pipe = make_fullscreen_pipeline(spv_g_present_vert, spv_g_present_vert_len, spv_g_present_frag, spv_g_present_frag_len, PRESENT_IMG_FMT);
  // The IMAGE pipeline draws into the swapchain (the s_present_img blit, and gpu_vk_present_image), so it
  // needs the swapchain format and is genuinely windowed-only.
  if (!s_headless)
    s_image_pipe = make_fullscreen_pipeline(spv_g_image_vert, spv_g_image_vert_len, spv_g_image_frag, spv_g_image_frag_len, s_swap_fmt);
  create_3d_pipelines();   // the native 3D/textured raster pipelines — windowed AND headless
  lucent::info("gpu_vk", "{} renderer up (VRAM {}x{} RG8 = PSX 1555)", s_headless ? "headless" : "windowed", VRAM_W, VRAM_H);
  // RmlUi mod/debug overlay. Brought up in BOTH legs: this used to be `if (!s_headless)`, which made
  // the overlay's very EXISTENCE a property of the window — so every headless instrument was
  // structurally blind to it, and a user-reported dead overlay could not be diagnosed at all without
  // taking the user's screen (spyro issue #52). The window is an output sink, not a mode
  // (docs/workspace/PROTOCOL.md), so the overlay takes the SINK's size and the format of the pass it will
  // record into, and `s_win` (NULL headless) is passed only for input translation.
  int ow = 0, oh = 0;
  sink_size(&ow, &oh);
  overlay_glue_init(game, s_win, s_dev, s_headless ? PRESENT_IMG_FMT : s_swap_fmt, ow, oh);
}

static void poll_quit(Game* game) {
  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    overlay_glue_event(game, &e);   // RmlUi overlay: ESC toggle + mouse/keyboard nav (no-op if not inited)
    if (e.type == SDL_EVENT_QUIT) exit(0);
  }
}

// Copy CPU VRAM (src, 1024*512 uint16) into THIS Game's VRAM image — but only over `regions`.
//
// THE COMPOSITE IS A PERSISTENT FRAMEBUFFER, so WHICH REGIONS is the whole content of this function.
// It used to upload all 1024x512 unconditionally, and under vk_path() the guest's polygons never reach
// CPU VRAM (they go to the VK rasterizer) — so every present erased every rasterized pixel, including
// the entire buffer the guest was about to display. That is the measured every-other-frame flicker; see
// vram_dirty.h for the numbers and tests/test_vram_persistence.cpp for the property.
//
// The STAGING memcpy stays whole-VRAM: it writes the transfer buffer, not the composite, so it destroys
// nothing, and keeping it whole means each region's GPU copy can address the transfer buffer with the
// full 1024-pixel row stride (srci.offset + pixels_per_row) instead of needing its own packing.
// `regions` MUST be explicit at every call site — a caller that genuinely wants the whole canvas says
// so, rather than getting it because nobody thought about it.
static void upload_vram(GpuVkState& g, SDL_GPUCommandBuffer* cmd, const uint16_t* src,
                        const VramDirtyRect* regions, int nregions) {
  g.ensure_targets();
  if (nregions <= 0) return;   // nothing the guest wrote — the composite already shows it
  void* p = SDL_MapGPUTransferBuffer(s_dev, g.s_vram_xfer, true);
  memcpy(p, src, (size_t)VRAM_W * VRAM_H * 2);
  SDL_UnmapGPUTransferBuffer(s_dev, g.s_vram_xfer);
  SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
  for (int i = 0; i < nregions; i++) {
    const VramDirtyRect& r = regions[i];
    if (r.w <= 0 || r.h <= 0) continue;
    SDL_GPUTextureTransferInfo srci = {}; srci.transfer_buffer = g.s_vram_xfer;
    srci.offset = (Uint32)(((size_t)r.y * VRAM_W + r.x) * 2);   // the rect's first pixel...
    srci.pixels_per_row = VRAM_W; srci.rows_per_layer = VRAM_H; // ...read with the full VRAM row stride
    SDL_GPUTextureRegion dst = {}; dst.texture = g.s_vram_tex;
    dst.x = (Uint32)r.x; dst.y = (Uint32)r.y; dst.w = (Uint32)r.w; dst.h = (Uint32)r.h; dst.d = 1;
    SDL_UploadToGPUTexture(cp, &srci, &dst, false);
  }
  SDL_EndGPUCopyPass(cp);
}

// The whole canvas, as a region list — for the callers that really do mean all of it (the GPU selftest
// pattern, and the SBS pane readback, which rebuilds each core's picture from that core's CPU VRAM by
// design). Spelled out so those call sites read as a decision rather than an omission.
static const VramDirtyRect kWholeVram[1] = { { 0, 0, VRAM_W, VRAM_H } };

// SDL_GPUViewport for a PaneRect (sbs_pane_layout.h owns the geometry; this only adds SDL's depth range).
static SDL_GPUViewport viewport_of(PaneRect r) {
  return SDL_GPUViewport{ (float)r.x, (float)r.y, (float)r.w, (float)r.h, 0.0f, 1.0f };
}
// Compute the letterboxed viewport for an aspect aw:ah within the window (ow x oh).
static SDL_GPUViewport letterbox(int aw, int ah, int ow, int oh) {
  return viewport_of(pane_letterbox(aw, ah, ow, oh));
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
//
// ires (internal resolution): when the live scale `i` (mods.ires, resolved via gpu_vk_video_status — same
// AUTO-derivation + cap the RmlUi readout uses) is >1, the 3D-WORLD band's Pass A/decode/Pass B/encode
// (render_pass_set below) targets the SEPARATE ires-scaled surfaces (GpuVkState::s_ires_*, VRAM_W*i x
// VRAM_H*i) instead of s_vram_tex/s_depth/s_color_rgba — same shaders, same vertex data (still absolute
// VRAM pixel coords; tri.vert's fixed /512,/256 NDC divisors are unchanged), just a viewport that's i
// times as large, so rasterization of the SAME clip-space geometry lands at i times the pixel density
// (literally "the viewport scaled by i"). Two blits (LINEAR filter, SDL_BlitGPUTexture, both outside any
// render pass) bracket this: seed the display sub-rect of the ires target with an upsampled copy of the
// current s_vram_tex content (so Pass A's LOAD blends against real background) before, and downsample the
// SAME sub-rect back into s_vram_tex after. Both blits are scoped to exactly [sx,sy,disp_w,h] — the region
// the un-scaled path would have written directly — so every VRAM-space 2D consumer (texture pages, CLUTs,
// sprite blits, readback, SBS) never sees the ires target and stays pixel-exact. At i==1 (the overwhelmingly
// common case) `ires` is false below: colorTgt/depthTgt/rgbaTgt alias the plain s_vram_tex/s_depth/
// s_color_rgba fields and neither blit runs — the GPU command stream is byte-for-byte the pre-ires code
// path (no extra copy/blit cost, no behavior change).
//
// bug #55 (ires blur): 2D content (RQ_OM_2D_BG/RQ_OM_2D_FG — HUD, menus, dialog/fade panels) used to share
// the SAME tri/tex/semi batches as the 3D world above, so at ires>1 it got rasterized into the ires-scaled
// target alongside the world geometry and suffered the SAME seed-upsample (LINEAR, lossy for sharp pixel
// art/text) + box-downsample round trip — even though 2D has nothing to do with the ires scale and the
// design intent ("every VRAM-space 2D op stays on the original canvas, pixel-exact") never actually held
// for it. Root-caused with pixel evidence: a pause-menu capture (Options/Load data/Quit game) diffed
// non-zero between ires=1 and ires=4 despite being pixel content that never changes with the 3D scale
// (docs/findings/render.md "ires 2D/HUD blur (bug #55)").
// Fix: 2D content is now batched SEPARATELY per band (GpuVkState::s_tri2d_buf/s_tex2d_buf/s_semi2d_buf,
// indexed GGS_2D_BG/GGS_2D_FG — see draw_tri/draw_tritri/draw_semi's ggs_is_3d/ggs_2d_band routing) and
// rendered by render_geom below in three ordered passes that never share a target with the ires-scaled one:
//   1. 2D_BG  -> straight onto s_vram_tex/s_depth/s_color_rgba at NATIVE resolution (scale=1), BEFORE the
//      3D world — matches the existing order-band invariant (2D_BG always behind the 3D world).
//   2. 3D world -> the (possibly ires-scaled) target + seed/composite-back, UNCHANGED from before the split.
//   3. 2D_FG (HUD/menus) -> straight onto s_vram_tex at NATIVE resolution, AFTER the 3D composite-back, so
//      it is NEVER touched by the ires round trip — provably pixel-exact for ANY RQ_OM_2D_FG content,
//      regardless of what the 3D band does, since nothing runs after it.
// The band split alone is NOT sufficient for RQ_OM_2D_BG content specifically: band 2's composite-back
// still overwrites the ENTIRE display sub-rect unconditionally, including band 1's own pixels wherever
// this frame's 3D geometry didn't rasterize a fragment there (the ORIGINAL single-pass code got per-pixel
// occlusion for free from one shared real depth test; splitting into sequential passes across different
// targets lost it). Narrowed — not fully closed — by a per-pixel 3D-coverage gate: the composite-back
// (ires_downsample.frag) samples the ires depth target and, for any source sub-texel with no OPAQUE 3D
// fragment (Pass A, depth-tested), substitutes u_native — a native-res snapshot of s_vram_tex taken right
// after band 1 (GpuVkState::s_ires_bg_snap) — instead of the lossy upsampled seed. A second pass
// (semi_cover.frag, s_semi_cover_pipe) re-rasterizes the semi buckets depth-only so TRANSLUCENT 3D
// coverage also registers (Pass B itself never writes depth by design, so overlapping semi quads can all
// blend against each other).
// KNOWN RESIDUAL (disclosed, not silently patched): the pause-menu capture used to verify this fix has its
// text classified RQ_OM_2D_BG (node_is_bg/sprite_is_bg_texpage provenance — not RQ_OM_2D_FG as first
// assumed), and the "ghosted" 3D world visible behind the paused menu still measurably blurs it at ires>1
// even with both coverage gates active — instrumentation (PSXPORT_DEBUG=ires depth-visualization) showed
// the composite-back's OWN opaque+semi coverage tests read near-zero coverage across most of that region,
// yet the real (correct, i==1) picture is unmistakably drawn there. This gap was not root-caused within
// this change's scope — it needs further RE into WHICH draw path actually paints that content (the
// tri/tex/semi counts logged for the frame don't obviously account for it) before the coverage gate can
// close it. RQ_OM_2D_FG content (the majority of real HUD/menu use — see e.g. any pure-2D dialog with no
// world visible, RQ_OM_2D_BG behind a scene the 3D world does NOT overlap, and the reported "world edges
// sharper at higher ires" case) is fixed and verified; RQ_OM_2D_BG overlapping dense/ghosted 3D coverage
// is a narrower follow-up.
static void render_pass_set(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* colorTgt, SDL_GPUTexture* depthTgt,
                             SDL_GPUTexture* rgbaTgt, SDL_GPUTexture* vramSnap, const SDL_GPUViewport& vp,
                             const SDL_Rect& sc, int scale,
                             SDL_GPUBuffer* triVbuf, int triN, SDL_GPUBuffer* texVbuf, int texN,
                             SDL_GPUBuffer* const semiVbuf[GGS_NUM_BLEND_MODES], const int semiN[GGS_NUM_BLEND_MODES],
                             bool stampSemiCoverage = false, bool clearColorBlack = false, int phase = 0) {
  int semiTotal = 0; for (int m = 0; m < NUM_BLEND_MODES; m++) semiTotal += semiN[m];
  SDL_GPUTextureSamplerBinding snap = { vramSnap, s_samp_nearest };
  SDL_GPUBufferBinding bb = {}; bb.offset = 0;
  // ---- Pass A: opaque (flat + textured) -----------------------------------------------------------
  if (phase != 2) {
    // clearColorBlack (first band): composite native submits over BLACK, not over the uploaded PSX VRAM
    // — the PC renderer shows ONLY what a native producer submitted; anything else is black.
    SDL_GPUColorTargetInfo ct = {}; ct.texture = colorTgt; ct.store_op = SDL_GPU_STOREOP_STORE;
    ct.load_op = clearColorBlack ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
    ct.clear_color = (SDL_FColor){ 0, 0, 0, 1 };
    SDL_GPUDepthStencilTargetInfo dt = {}; dt.texture = depthTgt; dt.clear_depth = 0.0f;
    dt.load_op = SDL_GPU_LOADOP_CLEAR; dt.store_op = SDL_GPU_STOREOP_STORE;   // STORE: the semi pass reuses this depth
    dt.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE; dt.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(cmd, &ct, 1, &dt);
    SDL_SetGPUViewport(rp, &vp); SDL_SetGPUScissor(rp, &sc);
    if (triN) { SDL_BindGPUGraphicsPipeline(rp, s_tri_pipe); bb.buffer = triVbuf; SDL_BindGPUVertexBuffers(rp, 0, &bb, 1);
                   int32_t ires_scale_pc = scale; SDL_PushGPUFragmentUniformData(cmd, 0, &ires_scale_pc, sizeof ires_scale_pc);
                   SDL_DrawGPUPrimitives(rp, triN, 1, 0, 0); }
    if (texN) { SDL_BindGPUGraphicsPipeline(rp, s_tritex_pipe); bb.buffer = texVbuf; SDL_BindGPUVertexBuffers(rp, 0, &bb, 1);
                   SDL_BindGPUFragmentSamplers(rp, 0, &snap, 1);
                   int32_t ires_scale_pc = scale; SDL_PushGPUFragmentUniformData(cmd, 0, &ires_scale_pc, sizeof ires_scale_pc);
                   SDL_DrawGPUPrimitives(rp, texN, 1, 0, 0); }
    SDL_EndGPURenderPass(rp);
  }
  if (phase == 1) return;
  if (!semiTotal) return;
  // ---- decode: colorTgt (this frame's opaque content) -> rgbaTgt (float, real-blend target) ----
  {
    SDL_GPUColorTargetInfo ct = {}; ct.texture = rgbaTgt; ct.load_op = SDL_GPU_LOADOP_DONT_CARE; ct.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(cmd, &ct, 1, nullptr);
    SDL_SetGPUViewport(rp, &vp); SDL_SetGPUScissor(rp, &sc);
    SDL_GPUTextureSamplerBinding vramtex = { colorTgt, s_samp_nearest };
    SDL_BindGPUGraphicsPipeline(rp, s_decode_pipe); SDL_BindGPUFragmentSamplers(rp, 0, &vramtex, 1);
    SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
    SDL_EndGPURenderPass(rp);
  }
  // ---- Pass B: textured-semi, one draw call per non-empty blend-mode bucket, REAL HW blend, testing
  //      (not clearing/writing) Pass A's depth ----------------------------------------------------------
  {
    SDL_GPUColorTargetInfo ct = {}; ct.texture = rgbaTgt; ct.load_op = SDL_GPU_LOADOP_LOAD; ct.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPUDepthStencilTargetInfo dt = {}; dt.texture = depthTgt;
    // STORE (not DONT_CARE): Pass B itself never writes depth (depth_write=false, test-only), but the
    // bug #55 part-3 coverage stamp right after this pass DOES need to read Pass A's already-written
    // opaque depth via the SAME GREATER_OR_EQUAL test — DONT_CARE would let the driver discard it.
    dt.load_op = SDL_GPU_LOADOP_LOAD; dt.store_op = SDL_GPU_STOREOP_STORE;
    dt.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE; dt.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(cmd, &ct, 1, &dt);
    SDL_SetGPUViewport(rp, &vp); SDL_SetGPUScissor(rp, &sc);
    { int32_t ires_scale_pc = scale; SDL_PushGPUFragmentUniformData(cmd, 0, &ires_scale_pc, sizeof ires_scale_pc); }
    for (int m = 0; m < NUM_BLEND_MODES; m++) if (semiN[m]) {
      SDL_BindGPUGraphicsPipeline(rp, s_semi_pipe[m]); bb.buffer = semiVbuf[m]; SDL_BindGPUVertexBuffers(rp, 0, &bb, 1);
      SDL_BindGPUFragmentSamplers(rp, 0, &snap, 1); SDL_DrawGPUPrimitives(rp, semiN[m], 1, 0, 0);
    }
    SDL_EndGPURenderPass(rp);
  }
  // ---- bug #55 (part 3): depth-only re-rasterization of the semi buckets, marking depth wherever a real
  // (non-discarded) TRANSLUCENT fragment landed — see semi_cover.frag's header comment. Pass B above never
  // writes depth by design (so overlapping semi quads all blend), so without this a scene whose visible 3D
  // content is mostly/entirely semi (e.g. the "ghosted" paused-game world behind the pause menu) would
  // register as fully uncovered to the ires composite-back's coverage gate, discarding the correct blended
  // picture in favor of the native pre-3D snapshot. Only meaningful when the composite-back will actually
  // run (stampSemiCoverage is passed true only for the 3D band, only when ires>1 — see render_geom).
  if (stampSemiCoverage) {
    SDL_GPUDepthStencilTargetInfo dt = {}; dt.texture = depthTgt;
    dt.load_op = SDL_GPU_LOADOP_LOAD; dt.store_op = SDL_GPU_STOREOP_STORE;
    dt.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE; dt.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(cmd, nullptr, 0, &dt);
    SDL_SetGPUViewport(rp, &vp); SDL_SetGPUScissor(rp, &sc);
    SDL_BindGPUGraphicsPipeline(rp, s_semi_cover_pipe);
    { int32_t ires_scale_pc = scale; SDL_PushGPUFragmentUniformData(cmd, 0, &ires_scale_pc, sizeof ires_scale_pc); }
    for (int m = 0; m < NUM_BLEND_MODES; m++) if (semiN[m]) {
      bb.buffer = semiVbuf[m]; SDL_BindGPUVertexBuffers(rp, 0, &bb, 1);
      SDL_BindGPUFragmentSamplers(rp, 0, &snap, 1); SDL_DrawGPUPrimitives(rp, semiN[m], 1, 0, 0);
    }
    SDL_EndGPURenderPass(rp);
  }
  // ---- encode: rgbaTgt -> colorTgt (1555), for present/shot/vkvram/provat/SBS -------------------
  {
    SDL_GPUColorTargetInfo ct = {}; ct.texture = colorTgt; ct.load_op = SDL_GPU_LOADOP_DONT_CARE; ct.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(cmd, &ct, 1, nullptr);
    SDL_SetGPUViewport(rp, &vp); SDL_SetGPUScissor(rp, &sc);
    SDL_GPUTextureSamplerBinding colorrgba = { rgbaTgt, s_samp_nearest };
    SDL_BindGPUGraphicsPipeline(rp, s_encode_pipe); SDL_BindGPUFragmentSamplers(rp, 0, &colorrgba, 1);
    SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
    SDL_EndGPURenderPass(rp);
  }
}
// True when this frame's geometry batch received nothing — the same accounting render_geom does for
// its `total`, hoisted so the present path can decide whether there is anything new to composite.
static bool geom_batch_empty(GpuVkState& g) {
  int total = g.s_tri_n + g.s_tex_n + g.s_painter_tex_n + g.s_painter_tri_n;
  for (int m = 0; m < NUM_BLEND_MODES; m++) total += g.s_semi_n[m];
  for (int band = 0; band < GGS_NUM_2D_BANDS; band++) {
    total += g.s_tri2d_n[band] + g.s_tex2d_n[band];
    for (int m = 0; m < NUM_BLEND_MODES; m++) total += g.s_semi2d_n[band][m];
  }
  return total == 0;
}

static void render_geom(GpuVkState& g, SDL_GPUCommandBuffer* cmd, const uint16_t* src,
                        int sx, int sy, int disp_w, int h, int* dtri, int* dtex, int* dsemi,
                        bool preserveBackdrop = false) {
  int semi_total = 0; for (int m = 0; m < NUM_BLEND_MODES; m++) semi_total += g.s_semi_n[m];
  int semi2d_total[GGS_NUM_2D_BANDS] = {};
  for (int band = 0; band < GGS_NUM_2D_BANDS; band++)
    for (int m = 0; m < NUM_BLEND_MODES; m++) semi2d_total[band] += g.s_semi2d_n[band][m];
  *dtri = g.s_tri_n; *dtex = g.s_tex_n; *dsemi = semi_total;   // 3D-world-only counts, as before the split
  const bool has3d = (g.s_tri_n + g.s_tex_n + semi_total) > 0;
  int total = g.s_tri_n + g.s_tex_n + g.s_painter_tex_n + g.s_painter_tri_n + semi_total;
  for (int band = 0; band < GGS_NUM_2D_BANDS; band++)
    total += g.s_tri2d_n[band] + g.s_tex2d_n[band] + semi2d_total[band];
  g.s_present_ires = 0;   // default: present from native s_vram_tex; the unified path below raises it to `scale`
  if (total == 0) {
    // No native submit this frame. Which of two things that means depends on who owns the frame, and
    // GameConfig::preserveVramBackdrop is what says so — the SAME switch band 1 below consults for its
    // clear. Honour it here too; not doing so was issue 0029.
    //   * A port whose NATIVE renderer owns the frame: zero prims means nothing to show, so clear to
    //     BLACK rather than reveal raw PSX VRAM. (A native FMV or splash draws via gpu_vk_present_image,
    //     a separate native RGBA path — not this VRAM present.)
    //   * A port still running the GUEST's drawing: upload-only screens are NORMAL — loading screens,
    //     fades and static art are blitted straight into VRAM and submit zero primitives. Clearing here
    //     destroys exactly those frames, and it does so ABOVE every other backdrop control in this
    //     function, so preserving the backdrop at band 1 could never take effect on them.
    if (!preserveBackdrop) {
      SDL_GPUColorTargetInfo ct = {}; ct.texture = g.s_vram_tex; ct.store_op = SDL_GPU_STOREOP_STORE;
      ct.load_op = SDL_GPU_LOADOP_CLEAR; ct.clear_color = (SDL_FColor){ 0, 0, 0, 1 };
      SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(cmd, &ct, 1, NULL);
      SDL_EndGPURenderPass(rp);
    }
    return;   // s_present_ires is already 0 above → present() samples the native s_vram_tex either way
  }
  g.ensure_targets();

  int native_w = 0, ires_i = 1, fbw = 0, fbh = 0, ww = 0, wh = 0, ires_cap = 0;
  gpu_vk_video_status(&g.game->core, &native_w, &ires_i, &fbw, &fbh, &ww, &wh, &ires_cap);
  g.ensure_ires_targets(ires_i);
  const bool ires = ires_i > 1;
  const int scale = ires ? ires_i : 1;
  const int cw = VRAM_W * scale, ch = VRAM_H * scale;
  if (g.s_painter_ranges) g.ensure_painter_targets(cw,ch);
  lucent::debug("ires", "sx={} sy={} disp_w={} h={} ires_i={} scale={} cw={} ch={} | tri={} tex={} semi={} | bg tri={} tex={} semi={} | fg tri={} tex={} semi={}",
    sx, sy, disp_w, h, ires_i, scale, cw, ch, g.s_tri_n, g.s_tex_n, semi_total,
    g.s_tri2d_n[GGS_2D_BG], g.s_tex2d_n[GGS_2D_BG], semi2d_total[GGS_2D_BG],
    g.s_tri2d_n[GGS_2D_FG], g.s_tex2d_n[GGS_2D_FG], semi2d_total[GGS_2D_FG]);

  // ---- upload: snapshot + ALL vertex batches (3D world + both 2D bands) in ONE copy pass -----------
  { void* p = SDL_MapGPUTransferBuffer(s_dev, g.s_snap_xfer, true); memcpy(p, src, (size_t)VRAM_W*VRAM_H*2); SDL_UnmapGPUTransferBuffer(s_dev, g.s_snap_xfer); }
  if (g.s_tri_n)  { void* p = SDL_MapGPUTransferBuffer(s_dev, g.s_tri_xfer, true);  memcpy(p, g.s_tri_buf,  (size_t)g.s_tri_n*sizeof(TriVtx));  SDL_UnmapGPUTransferBuffer(s_dev, g.s_tri_xfer); }
  if (g.s_tex_n)  { void* p = SDL_MapGPUTransferBuffer(s_dev, g.s_tex_xfer, true);  memcpy(p, g.s_tex_buf,  (size_t)g.s_tex_n*sizeof(TexVtx));  SDL_UnmapGPUTransferBuffer(s_dev, g.s_tex_xfer); }
  if (g.s_painter_tex_n) { void* p=SDL_MapGPUTransferBuffer(s_dev,g.s_painter_tex_xfer,true); memcpy(p,g.s_painter_tex_buf,(size_t)g.s_painter_tex_n*sizeof(TexVtx)); SDL_UnmapGPUTransferBuffer(s_dev,g.s_painter_tex_xfer); }
  if (g.s_painter_tri_n) { void* p=SDL_MapGPUTransferBuffer(s_dev,g.s_painter_tri_xfer,true); memcpy(p,g.s_painter_tri_buf,(size_t)g.s_painter_tri_n*sizeof(TriVtx)); SDL_UnmapGPUTransferBuffer(s_dev,g.s_painter_tri_xfer); }
  for (int m = 0; m < NUM_BLEND_MODES; m++) if (g.s_semi_n[m]) {
    void* p = SDL_MapGPUTransferBuffer(s_dev, g.s_semi_xfer[m], true);
    memcpy(p, g.s_semi_buf[m], (size_t)g.s_semi_n[m]*sizeof(TexVtx));
    SDL_UnmapGPUTransferBuffer(s_dev, g.s_semi_xfer[m]);
  }
  for (int band = 0; band < GGS_NUM_2D_BANDS; band++) {
    if (g.s_tri2d_n[band]) { void* p = SDL_MapGPUTransferBuffer(s_dev, g.s_tri2d_xfer[band], true); memcpy(p, g.s_tri2d_buf[band], (size_t)g.s_tri2d_n[band]*sizeof(TriVtx)); SDL_UnmapGPUTransferBuffer(s_dev, g.s_tri2d_xfer[band]); }
    if (g.s_tex2d_n[band]) { void* p = SDL_MapGPUTransferBuffer(s_dev, g.s_tex2d_xfer[band], true); memcpy(p, g.s_tex2d_buf[band], (size_t)g.s_tex2d_n[band]*sizeof(TexVtx)); SDL_UnmapGPUTransferBuffer(s_dev, g.s_tex2d_xfer[band]); }
    for (int m = 0; m < NUM_BLEND_MODES; m++) if (g.s_semi2d_n[band][m]) {
      void* p = SDL_MapGPUTransferBuffer(s_dev, g.s_semi2d_xfer[band][m], true);
      memcpy(p, g.s_semi2d_buf[band][m], (size_t)g.s_semi2d_n[band][m]*sizeof(TexVtx));
      SDL_UnmapGPUTransferBuffer(s_dev, g.s_semi2d_xfer[band][m]);
    }
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
  upv(g.s_painter_tex_xfer,g.s_painter_tex_vbuf,g.s_painter_tex_n,sizeof(TexVtx));
  upv(g.s_painter_tri_xfer,g.s_painter_tri_vbuf,g.s_painter_tri_n,sizeof(TriVtx));
  for (int m = 0; m < NUM_BLEND_MODES; m++) upv(g.s_semi_xfer[m], g.s_semi_vbuf[m], g.s_semi_n[m], sizeof(TexVtx));
  for (int band = 0; band < GGS_NUM_2D_BANDS; band++) {
    upv(g.s_tri2d_xfer[band], g.s_tri2d_vbuf[band], g.s_tri2d_n[band], sizeof(TriVtx));
    upv(g.s_tex2d_xfer[band], g.s_tex2d_vbuf[band], g.s_tex2d_n[band], sizeof(TexVtx));
    for (int m = 0; m < NUM_BLEND_MODES; m++) upv(g.s_semi2d_xfer[band][m], g.s_semi2d_vbuf[band][m], g.s_semi2d_n[band][m], sizeof(TexVtx));
  }
  SDL_EndGPUCopyPass(cp);

  // ---- ONE UNIFIED RENDER PATH (USER 2026-07-16): render EVERY band into the composite C at THIS scale,
  // then present from C. The ires level changes only the target SIZE, never the behaviour — no content
  // gates (has3d/have_2dfg), no per-level branches. C = s_vram_tex at 1x (already holds the uploaded VRAM),
  // s_ires_color at >1x (its legacy-2D base seeded once from the native upload). The old SSAA apparatus —
  // seed blit, bg-snapshot, coverage-mixing downsample (bug #55) — is DELETED: it only existed to
  // downsample-to-native BEFORE present; now the WINDOW presents from C directly at full res, so the only
  // downsample left is a plain box C -> s_vram_tex, purely so the headless `shot` / VRAM readback still work.
  (void)has3d;
  SDL_GPUTexture* C  = ires ? g.s_ires_color : g.s_vram_tex;
  SDL_GPUTexture* Cd = ires ? g.s_ires_depth : g.s_depth;
  SDL_GPUTexture* Cr = ires ? g.s_ires_rgba  : g.s_color_rgba;
  if (ires) {   // seed C's legacy-2D base = this frame's native VRAM upload, scaled up (usually empty)
    SDL_GPUBlitInfo bi = {};
    bi.source.texture = g.s_vram_tex; bi.source.w = (Uint32)VRAM_W; bi.source.h = (Uint32)VRAM_H;
    bi.destination.texture = C; bi.destination.w = (Uint32)cw; bi.destination.h = (Uint32)ch;
    bi.load_op = SDL_GPU_LOADOP_DONT_CARE; bi.filter = SDL_GPU_FILTER_LINEAR;
    SDL_BlitGPUTexture(cmd, &bi);
  }
  // Viewport spans the full (scaled) canvas — tri.vert's NDC divisors are fixed to the 1024x512 canvas, so
  // the viewport is what scales. 2D bands cover the whole canvas; the 3D band restricts to the display rect.
  SDL_GPUViewport vp = { 0, 0, (float)cw, (float)ch, 0.0f, 1.0f };
  SDL_Rect sc2d = { 0, 0, cw, ch };
  SDL_Rect sc3d = { sx * scale, sy * scale, disp_w * scale, h * scale };
  render_pass_set(cmd, C, Cd, Cr, g.s_vram_snap, vp, sc2d, scale,          // band 1: 2D_BG (backdrop)
                   g.s_tri2d_vbuf[GGS_2D_BG], g.s_tri2d_n[GGS_2D_BG], g.s_tex2d_vbuf[GGS_2D_BG], g.s_tex2d_n[GGS_2D_BG],
                   g.s_semi2d_vbuf[GGS_2D_BG], g.s_semi2d_n[GGS_2D_BG],
                   /*stampSemiCoverage=*/false, /*clearColorBlack=*/!preserveBackdrop);
                   // Clearing to black shows ONLY what was submitted, which is right when a native
                   // renderer owns the frame. A port still running the guest's drawing needs the
                   // uploaded VRAM to survive, or its upload-only screens render black — see
                   // GameConfig::preserveVramBackdrop.
  render_pass_set(cmd, C, Cd, Cr, g.s_vram_snap, vp, sc3d, scale,
                   g.s_tri_vbuf,g.s_tri_n,g.s_tex_vbuf,g.s_tex_n,g.s_semi_vbuf,g.s_semi_n,false,false,1);
  // Painter objects: clear the reusable local target per object, replay that object's unified authored
  // range with ALWAYS/write, then export the winning fragment's packed color + actual interpolated D32
  // through the ordinary world GE/write test. TexVtx ord carries the same global submit-order epsilon as
  // ordinary geometry, explicitly preserving equal-real-depth ties despite the regrouped passes.
  for(int r=0;r<g.s_painter_ranges;r++) {
    SDL_GPUColorTargetInfo pct={}; pct.texture=g.s_painter_color; pct.load_op=SDL_GPU_LOADOP_CLEAR; pct.store_op=SDL_GPU_STOREOP_STORE; pct.clear_color=(SDL_FColor){0,0,0,1};
    SDL_GPUDepthStencilTargetInfo pdt={}; pdt.texture=g.s_painter_depth; pdt.clear_depth=0.f; pdt.load_op=SDL_GPU_LOADOP_CLEAR; pdt.store_op=SDL_GPU_STOREOP_STORE;
    pdt.stencil_load_op=SDL_GPU_LOADOP_DONT_CARE; pdt.stencil_store_op=SDL_GPU_STOREOP_DONT_CARE;
    SDL_GPURenderPass* pr=SDL_BeginGPURenderPass(cmd,&pct,1,&pdt); SDL_SetGPUViewport(pr,&vp); SDL_SetGPUScissor(pr,&sc3d);
    SDL_GPUTextureSamplerBinding snap={g.s_vram_snap,s_samp_nearest};
    for (int ci=g.s_painter_first[r]; ci<g.s_painter_first[r]+g.s_painter_count[r]; ++ci) {
      const bool textured=g.s_painter_cmd_material[ci]!=0;
      SDL_GPUBufferBinding pb={textured?g.s_painter_tex_vbuf:g.s_painter_tri_vbuf,
        (Uint32)(g.s_painter_cmd_first[ci]*(textured?sizeof(TexVtx):sizeof(TriVtx)))};
      SDL_BindGPUGraphicsPipeline(pr,textured?s_painter_tex_pipe:s_painter_tri_pipe);
      SDL_BindGPUVertexBuffers(pr,0,&pb,1);
      if(textured) SDL_BindGPUFragmentSamplers(pr,0,&snap,1);
      int32_t scale_pc=scale; SDL_PushGPUFragmentUniformData(cmd,0,&scale_pc,sizeof scale_pc);
      SDL_DrawGPUPrimitives(pr,g.s_painter_cmd_count[ci],1,0,0);
    }
    SDL_EndGPURenderPass(pr);

    if (s_painter_test_local_depth) {
      SDL_GPUCopyPass* dcp=SDL_BeginGPUCopyPass(cmd);
      SDL_GPUTextureRegion ds={}; ds.texture=g.s_painter_depth; ds.w=(Uint32)cw; ds.h=(Uint32)ch; ds.d=1;
      SDL_GPUTextureTransferInfo dd={}; dd.transfer_buffer=s_painter_test_local_depth; dd.pixels_per_row=(Uint32)cw; dd.rows_per_layer=(Uint32)ch;
      SDL_DownloadFromGPUTexture(dcp,&ds,&dd); SDL_EndGPUCopyPass(dcp);
    }
    if (s_painter_test_local_color) {
      SDL_GPUCopyPass* ccp=SDL_BeginGPUCopyPass(cmd);
      SDL_GPUTextureRegion cs={}; cs.texture=g.s_painter_color; cs.w=(Uint32)cw; cs.h=(Uint32)ch; cs.d=1;
      SDL_GPUTextureTransferInfo cd={}; cd.transfer_buffer=s_painter_test_local_color; cd.pixels_per_row=(Uint32)cw; cd.rows_per_layer=(Uint32)ch;
      SDL_DownloadFromGPUTexture(ccp,&cs,&cd); SDL_EndGPUCopyPass(ccp);
    }

    SDL_GPUColorTargetInfo cct={}; cct.texture=C; cct.load_op=SDL_GPU_LOADOP_LOAD; cct.store_op=SDL_GPU_STOREOP_STORE;
    SDL_GPUDepthStencilTargetInfo cdt={}; cdt.texture=Cd; cdt.load_op=SDL_GPU_LOADOP_LOAD; cdt.store_op=SDL_GPU_STOREOP_STORE;
    cdt.stencil_load_op=SDL_GPU_LOADOP_DONT_CARE; cdt.stencil_store_op=SDL_GPU_STOREOP_DONT_CARE;
    SDL_GPURenderPass* crp=SDL_BeginGPURenderPass(cmd,&cct,1,&cdt); SDL_SetGPUViewport(crp,&vp); SDL_SetGPUScissor(crp,&sc3d);
    SDL_GPUTextureSamplerBinding ps[2]={{g.s_painter_color,s_samp_nearest},{g.s_painter_depth,s_samp_nearest}};
    SDL_BindGPUGraphicsPipeline(crp,s_painter_composite_pipe); SDL_BindGPUFragmentSamplers(crp,0,ps,2);
    SDL_DrawGPUPrimitives(crp,3,1,0,0); SDL_EndGPURenderPass(crp);
    if (s_painter_test_main_depth) {
      SDL_GPUCopyPass* dcp=SDL_BeginGPUCopyPass(cmd);
      SDL_GPUTextureRegion ds={}; ds.texture=Cd; ds.w=(Uint32)cw; ds.h=(Uint32)ch; ds.d=1;
      SDL_GPUTextureTransferInfo dd={}; dd.transfer_buffer=s_painter_test_main_depth; dd.pixels_per_row=(Uint32)cw; dd.rows_per_layer=(Uint32)ch;
      SDL_DownloadFromGPUTexture(dcp,&ds,&dd); SDL_EndGPUCopyPass(dcp);
    }
  }
  render_pass_set(cmd,C,Cd,Cr,g.s_vram_snap,vp,sc3d,scale,
                   g.s_tri_vbuf,g.s_tri_n,g.s_tex_vbuf,g.s_tex_n,g.s_semi_vbuf,g.s_semi_n,false,false,2);
  render_pass_set(cmd, C, Cd, Cr, g.s_vram_snap, vp, sc2d, scale,          // band 3: 2D_FG (HUD / menus)
                   g.s_tri2d_vbuf[GGS_2D_FG], g.s_tri2d_n[GGS_2D_FG], g.s_tex2d_vbuf[GGS_2D_FG], g.s_tex2d_n[GGS_2D_FG],
                   g.s_semi2d_vbuf[GGS_2D_FG], g.s_semi2d_n[GGS_2D_FG]);
  g.s_present_ires = scale;   // present() samples C (native s_vram_tex at 1x, s_ires_color at >1x)

  // Headless `shot` / VRAM-space readback: plain box-downsample C's display sub-rect -> s_vram_tex. No-op
  // at 1x (C IS s_vram_tex). The WINDOW never uses this — it presents from C directly (present()).
  if (ires) {
    SDL_GPUColorTargetInfo ct2 = {}; ct2.texture = g.s_vram_tex; ct2.load_op = SDL_GPU_LOADOP_LOAD; ct2.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* rp2 = SDL_BeginGPURenderPass(cmd, &ct2, 1, nullptr);
    SDL_GPUViewport vp2 = { (float)sx, (float)sy, (float)disp_w, (float)h, 0.0f, 1.0f };
    SDL_Rect sc2r = { sx, sy, disp_w, h };
    SDL_SetGPUViewport(rp2, &vp2); SDL_SetGPUScissor(rp2, &sc2r);
    SDL_GPUTextureSamplerBinding srcbind = { C, s_samp_nearest };
    SDL_BindGPUGraphicsPipeline(rp2, s_ires_downsample_pipe); SDL_BindGPUFragmentSamplers(rp2, 0, &srcbind, 1);
    int32_t n_pc = scale; SDL_PushGPUFragmentUniformData(cmd, 0, &n_pc, sizeof n_pc);
    SDL_DrawGPUPrimitives(rp2, 3, 1, 0, 0);
    SDL_EndGPURenderPass(rp2);
    lucent::debug("ires", "shot downsample dst=({},{},{},{}) n={}", sx, sy, disp_w, h, scale);
  }
}

// ---- present: upload CPU VRAM, render the 3D/textured batch on top, sample [sx,sy,w,h] to the swapchain
// renderFadeState is an OPTIONAL GameHooks entry — a port whose fade is not RE'd yet leaves it null,
// and two call sites below already guarded it, which is what makes it optional by design. Five others
// called it unconditionally and segfaulted the moment a shot/dump ran in such a port. That crash was
// recorded for months as "the VK readback BLOCKS" (issue 0018): the run produced few frames and no
// files, which looks exactly like a hang unless you check the exit status — it was 139 all along.
// One accessor so the guard cannot be forgotten again; absent means "no fade", which is what the
// guarded sites already did by leaving FadeState default-initialised.
static inline FadeState fade_state_of(Core* c) {
  FadeState f{};
  if (c && c->hooks && c->hooks->renderFadeState) c->hooks->renderFadeState(c, &f);
  return f;
}

void GpuVkState::ensure_present_img(int w, int h) {
  if (w <= 0 || h <= 0) return;
  if (s_present_img && s_present_img_w == w && s_present_img_h == h) return;
  if (s_present_img) { SDL_ReleaseGPUTexture(s_dev, s_present_img); SDL_ReleaseGPUTransferBuffer(s_dev, s_present_rb); }
  SDL_GPUTextureCreateInfo ti = {};
  ti.type = SDL_GPU_TEXTURETYPE_2D; ti.format = PRESENT_IMG_FMT;
  ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
  ti.width = (Uint32)w; ti.height = (Uint32)h; ti.layer_count_or_depth = 1; ti.num_levels = 1;
  s_present_img = SDL_CreateGPUTexture(s_dev, &ti); GPUCHK(s_present_img, "CreateGPUTexture(present img)");
  SDL_GPUTransferBufferCreateInfo dn = {}; dn.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
  dn.size = (Uint32)w * (Uint32)h * 4;
  s_present_rb = SDL_CreateGPUTransferBuffer(s_dev, &dn); GPUCHK(s_present_rb, "present img readback xfer");
  s_present_img_w = w; s_present_img_h = h;
  lucent::info("gpu_vk", "present image {}x{} ({} sink)", w, h, s_headless ? "headless" : "windowed");
}

// Gather the plan's inputs out of renderer state. Separate from plan_present() so the DECISION stays
// pure and testable while the state-reading stays in one place — and so it is visible at a glance that
// no leg term is read here either.
static PresentInputs present_inputs(const GpuVkState& g, int sx, int sy, int disp_w, int h,
                                    int native_w, const FadeState& fade) {
  PresentInputs in{};
  sink_size(&in.sink_w, &in.sink_h);
  in.sx = sx; in.sy = sy; in.disp_w = disp_w; in.disp_h = h;
  // The game's OWN 4:3 width, BEFORE any widescreen widening — this is what makes the presented
  // aspect 4:3 for a native frame of ANY width, and wider only when the port deliberately widened
  // it. See present_plan.h and issue 0008; passing disp_w here would restore the stretch.
  in.native_w = native_w;
  in.present_ires = g.s_present_ires;
  in.fade_mode = fade.mode; in.fade_r = fade.r; in.fade_g = fade.g; in.fade_b = fade.b;
  in.disp_rgb24 = g.s_disp_rgb24;
  return in;
}

static bool dump_to(GpuVkState& g, const char*, int, int, int, int, int, uint8_t, uint8_t, uint8_t);   // fwd (defined below) — preseq dump; false = NOTHING written
void GpuVkState::present(const uint16_t* src, int sx, int sy, int w, int h) {
  if (!gpu_vk_enabled()) return;
  if (!s_inited) init_gpu(game);
  // Widescreen: the engine renders a wider FOV into VRAM columns [sx, sx+nw). Everything downstream (the
  // windowed present sample region AND the `shot`/vkshot readback, which use s_last_w) must span that wide
  // width, else the wide FB is cropped back to the 4:3 s_disp_w. At 4:3 nw==320 so w is unchanged.
  // The PC renderer composites native submits over BLACK (render_geom), so a frame with no native
  // content is already black here — sampling the wide width just shows black, never the atlas. So the
  // present can unconditionally span the wide FB when widescreen; no frame-type heuristic needed.
  int disp_w = w;
  if (gpu_vk_wide_engine(&game->core)) disp_w = gpu_vk_wide_engine_w(&game->core);
  s_present_sx = sx; s_present_sy = sy;
  s_last_sx = sx; s_last_sy = sy; s_last_w = disp_w; s_last_h = h;

  // PSXPORT_GPU_TRACE: per-present source-VRAM occupancy + sampled display region (diagnostic).
  if (cfg_on("PSXPORT_GPU_TRACE")) { int& n = gdev().s_trace_n; if (n++ < 4 || (n % 200) == 0) {
    long nz = 0; for (long i = 0; i < (long)VRAM_W * VRAM_H; i++) if (src[i]) nz++;
    int semi_total = 0; for (int m = 0; m < NUM_BLEND_MODES; m++) semi_total += s_semi_n[m];
    // `batch` is the LIVE accumulator, sampled here at the TOP of present() — which is BEFORE this
    // frame's drawing and AFTER the previous frame_end reset it (gpu_vk.cpp:1569). On a game that
    // presents at the top of its frame loop, batch is therefore LEGITIMATELY 0 every time, and
    // reading it as "no primitives reached the native raster" is a false negative this line has
    // already produced once. `drawn` is what render_geom ACTUALLY rasterised on the last present
    // (s_dbg_*_c, the same counters gpu_vk_stats/dbg_server report) — that is the number to read
    // when asking whether the native renderer is doing anything.
    lucent::info("gpu_vk", "present #{} src nonzero={}/{} disp={},{} {}x{} | batch tri={} tex={} semi={} | drawn tri={} tex={} semi={}",
                 n, nz, VRAM_W*VRAM_H, sx, sy, w, h, s_tri_n, s_tex_n, semi_total,
                 s_dbg_tri_c, s_dbg_tex_c, s_dbg_semi_c); } }
  // `debug fadewatch`: per-present log of the ScreenFade state (the PC-native subsystem that owns fade).
  // A game that owns no fade subsystem leaves this hook null — the seam documents null hooks as
  // tolerated, and other call sites guard. These did not, so presenting from such a game jumped to
  // address 0. It stayed hidden because the reference consumer always supplies the hook; a second
  // consumer (Spyro, whose Phase-0 hook table is almost entirely null) segfaulted on its first
  // present. Default to "no fade" — zeroed, i.e. mode 0 / rgb 0 — which is exactly the state a game
  // without a fade subsystem is in.
  FadeState fade{};
  if (game->core.hooks->renderFadeState) game->core.hooks->renderFadeState(&game->core, &fade);
  static const lucent::Channel fadewatch_ch{"fadewatch"};
  if (fadewatch_ch) { GpuDevice& gd = gdev();
    int& lastmode = gd.s_fw_lastmode; uint8_t& lr = gd.s_fw_lr; uint8_t& lg = gd.s_fw_lg; uint8_t& lb = gd.s_fw_lb;
    int& lsx = gd.s_fw_lsx; int& lsy = gd.s_fw_lsy; int& lw = gd.s_fw_lw; int& lh = gd.s_fw_lh;
    if (fade.mode != lastmode || fade.r != lr || fade.g != lg || fade.b != lb ||
        sx != lsx || sy != lsy || w != lw || h != lh) {
      lucent::debug("fadewatch", "present disp={},{} {}x{} fade mode={} rgb=({},{},{})",
              sx, sy, w, h, fade.mode, fade.r, fade.g, fade.b);
      lastmode = fade.mode; lr = fade.r; lg = fade.g; lb = fade.b;
      lsx = sx; lsy = sy; lw = w; lh = h;
    }
  }
  SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(s_dev);
  GPUCHK(cmd, "AcquireGPUCommandBuffer");

  // A PRESENT WITH NO NEW GEOMETRY RE-SHOWS THE LAST COMPOSITE — it does not rebuild one.
  //
  // Presents are paced by the display field clock, but a guest need not draw every field: a 30 fps
  // game builds one ordering table per TWO fields. Rebuilding the composite on the empty present
  // produced a frame that alternated full-scene / black at 30 Hz. Measured on the Spider-Man port
  // over six consecutive presents: 0.0%, 99.4%, 0.0%, 99.4%, 0.0%, 99.4% non-black, where the 99.4%
  // frames are the fully-rendered main menu.
  //
  // Hardware does not do this. The display re-scans the SAME persistent framebuffer every field
  // whether or not the game drew into it, so a field with no drawing shows the previous image again.
  // Skipping the rebuild reproduces that exactly, and costs nothing — there is nothing new to show.
  //
  // preserveVramBackdrop does NOT cover this case and was tried first: it only skips render_geom's
  // CLEAR, while upload_vram above still overwrites the composite with guest VRAM — which for a port
  // that composites natively is empty, so the frame came out black anyway.
  //
  // BUT "the batch got nothing" is not the same question as "the guest produced nothing". A port
  // still running the guest's own drawing has a SECOND producer: a direct framebuffer write. An
  // upload-only screen — a logo still, a loading screen, a fade — is DMA'd straight into VRAM with
  // zero primitives, and skipping the rebuild for it means no composite is ever built, so it shows
  // black for its whole duration. That is Spyro's black boot logos, and it is issue 0029 one level
  // up: the identical "zero prims means nothing to show" assumption, re-introduced ABOVE the
  // preserveVramBackdrop control that was added to fix it, where that control cannot be reached.
  //
  // So the guard asks about CHANGE from BOTH producers, which is also what the hardware analogy
  // actually says — the display re-scans the same framebuffer only while nothing has written it.
  // A genuinely blank frame the guest MEANT to be blank clears its own VRAM, and that clear is a
  // VRAM write, so it still rebuilds.
  const bool guestVramIsPicture = game->core.cfg && game->core.cfg->preserveVramBackdrop;
  // …and a THIRD source, on which both of the inputs above are structurally blind: the PSX software
  // rasterizer (RenderPath::Psx) draws the frame into s_vram with no VK geometry and no dirty mark, so
  // s_vram is the picture at every present. See the policy header for the measurement.
  const PresentRebuild decision =
      present_rebuild_decision(geom_batch_empty(*this), guestVramIsPicture,
                               s_vram_writes, s_vram_writes_built, game->gpu.sw_path());
  // `debug presentskip`: the decision's running DISTRIBUTION with its denominator. This is the
  // measurement that sizes the change for a given port: REUSE_LAST is what afca817d bought (an idle
  // field costing nothing), REBUILD_VRAM is what this predicate restored (an upload-only screen that
  // would otherwise be black). A port where REUSE_LAST collapses to ~0 has a guest that writes VRAM
  // every field, and for that port the early-out is doing nothing regardless of this change.
  // The tally is kept UNCONDITIONALLY — it is one add, and totals that depended on whether logging
  // was enabled would be worthless. The emit is one unguarded lucent::debug: the channel gate lives
  // inside the logger, which is the whole point of having a configurable one.
  GpuDevice& gd = gdev();
  gd.s_ps_n[decision]++;
  lucent::debug("presentskip", "presents={} reuse_last={} rebuild_geom={} rebuild_vram={} | vram_writes={}",
                gd.s_ps_n[0] + gd.s_ps_n[1] + gd.s_ps_n[2], gd.s_ps_n[PRESENT_REUSE_LAST],
                gd.s_ps_n[PRESENT_REBUILD_GEOM], gd.s_ps_n[PRESENT_REBUILD_VRAM], s_vram_writes);
  // REUSE_LAST skips REBUILDING THE FRAME (no VRAM upload, no geometry) — it does not skip PRESENTING
  // one. The composite below still runs, because the picture depends on live state the frame does not:
  // the fade ramps every field, and the window can be resized under a completely idle guest.
  if (decision != PRESENT_REUSE_LAST) {
    // SOFTWARE RASTERIZER: the whole of s_vram is new, every present. The dirty list is the OTHER half
    // of the same blindness the decision above just fixed — both are fed exclusively by
    // `gpu_vk_dirty()`, and every one of its call sites in gpu_native.cpp is gated `if (vk_path())`. So
    // on RenderPath::Psx the region list is empty, `nup` is 0, and a rebuild uploads NOTHING: measured
    // 2026-08-11, fixing only the decision left the present still 0.0% non-black at 700/1200/2010. It
    // is not a heuristic here — on this path we rasterized every pixel of that buffer ourselves.
    if (game->gpu.sw_path()) s_dirty.markAll();

    // WIDESCREEN STORAGE IS NOT A FRAMEBUFFER. The guest owns only [sx,sx+w); the extra host-visible
    // columns [sx+w,sx+disp_w) are ordinary PSX VRAM and commonly hold textures/CLUTs. Loading that
    // region into the persistent composite leaks atlas pixels wherever no later primitive covers it.
    // Put an opaque black base behind the extension in the 2D-background band. This changes only the
    // host render batch: guest VRAM remains byte-for-byte intact, and authored backdrop/world/HUD
    // geometry draws over it in the normal three-band order. Index 0 is the back of the background
    // band, so this cannot cover an authored background primitive.
    const WideMarginPlan margin = plan_wide_margin(sx, sy, w, disp_w, h);
    if (margin.draw) {
      set_order_2d_bg(0);
      // FULL CANVAS clip, deliberately: the wide margin exists to paint the strip OUTSIDE the
      // guest's own draw area, so clipping it to that area would erase exactly what it is for.
      draw_tri(margin.x0, margin.y0, 0, 0, 0,
               margin.x1, margin.y0, 0, 0, 0,
               margin.x0, margin.y1, 0, 0, 0, 0, 0, 1023, 511);
      draw_tri(margin.x1, margin.y0, 0, 0, 0,
               margin.x1, margin.y1, 0, 0, 0,
               margin.x0, margin.y1, 0, 0, 0, 0, 0, 1023, 511);
    }
    // Only the regions the guest actually wrote (vram_dirty.h). Uploading all of VRAM here is what
    // erased the rasterized picture out of the buffer that was about to be displayed.
    VramDirtyRect up[VramDirty::CAP + 1];
    const int nup = vram_upload_regions(s_dirty, up, VramDirty::CAP + 1);
    lucent::debug("vramup", "regions={} all={} writes={} merges={} dropped={}",
                  nup, s_dirty.all() ? 1 : 0, s_dirty.adds(), s_dirty.merges(), s_dirty.dropped());
    upload_vram(*this, cmd, src, up, nup);                   // CPU VRAM -> THIS Game's VRAM image (2D backdrop)
    s_dirty.clear();                                         // the composite now holds every guest write
    render_geom(*this, cmd, src, sx, sy, disp_w, h, &s_dbg_tri_c, &s_dbg_tex_c, &s_dbg_semi_c,
                game->core.cfg && game->core.cfg->preserveVramBackdrop);   // draw the batch on top (+depth)
    s_vram_writes_built = s_vram_writes;   // this composite now reflects every guest write so far
  }

  // ---- THE PRESENT STAGE, ONE CODE PATH -------------------------------------------------------------
  // What used to be here was `if (s_headless) { submit; return; }` — headless stopped one stage short of
  // the picture, so every headless capture measured guest VRAM and no capture anywhere measured what the
  // player sees (issue 0005; instruments.md INST-18). The plan is computed from inputs with no leg term,
  // the composite is built in both legs, and the leg appears exactly once: whether it reaches a window.
  const PresentPlan plan = plan_present(present_inputs(*this, sx, sy, disp_w, h, /*native_w=*/w, fade), s_headless != 0);
  if (plan.build) build_present_image(cmd, plan);
  if (plan.to_swapchain) { show_present_image(cmd); return; }   // consumes cmd (submits + polls)
  gpu_submit(cmd, "present");
}

// ---- build_present_image: THE PICTURE — sample the composite target built by the last render_geom
// (s_vram_tex at 1x, s_ires_color at >1x) into s_present_img, letterboxed, with the live fade.
//
// This is the half that used to be swapchain-only, and moving it off the swapchain is the entire point:
// a window is a SINK, not a rendering stage, and nothing that decides what the picture LOOKS like may
// depend on whether one is open. Runs identically in both legs — the plan it consumes has no leg term
// (present_plan.h), and tests/test_present_plan.cpp fails if one is ever re-introduced.
//
// Does NOT consume `cmd`: the caller either follows with show_present_image (windowed) or submits.
void GpuVkState::build_present_image(SDL_GPUCommandBuffer* cmd, const PresentPlan& plan) {
  // The empty-batch early-out in present() (and repaint()) reaches here WITHOUT upload_vram/render_geom,
  // which are what lazily create the per-Game targets. Before any real frame has ever been built — e.g.
  // the Tomba2 boot stub's gpu_clear_display + present before the first scene submit — s_vram_tex is still
  // null, and binding it as the present sampler segfaults inside the SDL_GPU driver. Materialise the
  // targets here; ensure_targets() is a one-shot no-op once they exist.
  ensure_targets();
  // From the PLAN, never a fresh sink_size() call — see PresentPlan::sink_w. The target and the
  // viewport must be two views of ONE measurement of the window, or a resize mid-present letterboxes
  // for a rectangle that is not the one being drawn into.
  ensure_present_img(plan.sink_w, plan.sink_h);
  if (!s_present_img) return;

  SDL_GPUColorTargetInfo cti = {};
  cti.texture = s_present_img; cti.clear_color = (SDL_FColor){ 0, 0, 0, 1 };
  cti.load_op = SDL_GPU_LOADOP_CLEAR; cti.store_op = SDL_GPU_STOREOP_STORE;   // CLEAR paints the letterbox bars
  SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(cmd, &cti, 1, NULL);

  // Widescreen present: SAMPLE the wide FB region [sx, sx+nw) and letterbox to the aspect's display shape
  // (nw:240), else the wide FB is squeezed into a 4:3 box. At 4:3 nw==320 = old path. HIGH-RES PRESENT:
  // when render_geom built a valid ires composite this frame (plan.src_ires>1), sample the SCALED
  // s_ires_color over the scaled display sub-rect — a genuinely high-res picture — instead of the native
  // s_vram_tex downsample. Pure-2D frames / ires=1 keep the native path (src_ires==0). Every one of those
  // decisions is now made in plan_present() and merely EXECUTED here.
  SDL_GPUTexture* present_src = plan.src_ires > 1 ? s_ires_color : s_vram_tex;
  PresentPC pc;
  for (int i = 0; i < 4; i++) { pc.disp[i] = plan.disp[i]; pc.fade[i] = plan.fade[i]; pc.fmt[i] = plan.fmt[i]; }
  SDL_PushGPUFragmentUniformData(cmd, 0, &pc, sizeof pc);

  SDL_GPUViewport vp = viewport_of(plan.viewport);
  SDL_Rect sc = { 0, 0, s_present_img_w, s_present_img_h };
  SDL_BindGPUGraphicsPipeline(rp, s_present_pipe);
  SDL_SetGPUViewport(rp, &vp); SDL_SetGPUScissor(rp, &sc);
  SDL_GPUTextureSamplerBinding tsb = { present_src, s_samp_nearest };
  SDL_BindGPUFragmentSamplers(rp, 0, &tsb, 1);
  SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
  SDL_EndGPURenderPass(rp);
}

// ---- show_present_image: THE SINK — blit the finished picture into the window swapchain. -------------
// The one place in the renderer a leg difference is legitimate, and it does no picture work at all: the
// letterbox, fade, source selection and 24bpp decode are already baked into s_present_img, so this is a
// 1:1 fullscreen copy. If this function ever starts DECIDING something, the split has been lost.
// Consumes `cmd` (submits it).
void GpuVkState::show_present_image(SDL_GPUCommandBuffer* cmd) {
  SDL_GPUTexture* swaptex = NULL; Uint32 sw = 0, sh = 0;
  if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, s_win, &swaptex, &sw, &sh) || !swaptex) {
    gpu_submit(cmd, "show_present_image"); poll_quit(game); return;   // minimized / no swapchain image this frame
  }
  SDL_GPUColorTargetInfo cti = {};
  cti.texture = swaptex; cti.clear_color = (SDL_FColor){ 0, 0, 0, 1 };
  cti.load_op = SDL_GPU_LOADOP_CLEAR; cti.store_op = SDL_GPU_STOREOP_STORE;
  SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(cmd, &cti, 1, NULL);
  if (s_present_img) {
    // The image pipeline is an RGBA sampler with a scalar brightness; 1.0 = copy. The picture's own fade
    // was applied in build_present_image, where it belongs — applying any here would be a second,
    // window-only fade, i.e. exactly the leg-dependent picture this split exists to make impossible.
    float fpc[4] = { 1.0f, 0, 0, 0 };
    SDL_PushGPUFragmentUniformData(cmd, 0, fpc, sizeof fpc);
    // LETTERBOX, never stretch. s_present_img is normally already the swapchain's shape, in which case
    // this is the identity and the blit is the 1:1 copy it claims to be. But the two CAN disagree, and
    // when they do a full-window viewport silently distorts the picture:
    //   * the window is resized between the size this image was built for and this acquire;
    //   * repaint() — the debug-server pause loop, ~66 Hz from native_boot.cpp — re-blits the LAST
    //     BUILT image, which is sized to whatever the window was when the pause began. Resize while
    //     paused and nothing rebuilds it, because repaint deliberately builds nothing.
    // Fitting by aspect degrades to bars, which is the honest answer for "re-show the last built
    // frame"; stretching invents pixel geometry the game never produced.
    SDL_GPUViewport vp = viewport_of(pane_letterbox(s_present_img_w, s_present_img_h, (int)sw, (int)sh));
    SDL_Rect sc = { 0, 0, (int)sw, (int)sh };
    SDL_BindGPUGraphicsPipeline(rp, s_image_pipe);
    SDL_SetGPUViewport(rp, &vp); SDL_SetGPUScissor(rp, &sc);
    SDL_GPUTextureSamplerBinding tsb = { s_present_img, s_samp_nearest };
    SDL_BindGPUFragmentSamplers(rp, 0, &tsb, 1);
    SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
  }
  // RmlUi mod/debug overlay (ESC) composites ON TOP of the game frame, into the same present pass over
  // the FULL window. It is host UI, not the game's picture, which is why it belongs to the sink and not
  // to s_present_img — a present shot must show the frame, not the debug menu over it.
  overlay_glue_record(game, cmd, rp, (int)sw, (int)sh);
  SDL_EndGPURenderPass(rp);
  gpu_submit(cmd, "show_present_image");
  poll_quit(game);
}

// ---- repaint: re-show the LAST BUILT frame, without building anything (kanban #20) -------------------
// Used by the debug-server pause loop, which must keep the window live at ~66 Hz while the game does not
// advance. It must NOT go through present(): present() RE-RENDERS — it re-uploads CPU VRAM and re-runs
// render_geom over the LIVE vertex batch. At a pause point that batch is whatever the frame ordering
// happens to have left behind, which is not a property the pause loop can rely on:
//   * fps60 OFF — frameUpdate presents at the TOP of the frame and drawOTag/rq.flush fills the batch at the
//     BOTTOM, so at the frame-loop top a full batch is still resident; the re-render accidentally
//     reproduced the picture, at the cost of a full world re-render every 15 ms while paused.
//   * fps60 ON  — Fps60::present_vk emits, presents and frame_end-resets BOTH passes inside frameUpdate,
//     and rq.flush only rq_capture()s afterwards, so the batch is EMPTY at the frame-loop top. render_geom's
//     "no native submit -> the PC renderer shows BLACK" clear then wiped s_vram_tex: a black window, and a
//     `vkshot` that read back that same wiped target.
// So the paused window is not a rendering question at all — the frame is already finished and sitting in
// the composite target. Re-showing it is one swapchain pass with no upload, no geometry, no batch reset,
// and it costs the same in both fps modes. Headless: nothing to show and, crucially, nothing to clobber —
// s_vram_tex keeps holding the last real frame, so `shot`/`vkshot` while paused report it truthfully.
// Since the present split this is literally true rather than approximately so: the finished picture is
// sitting in s_present_img, and a repaint is one swapchain blit of it. It no longer even re-runs the
// composite, which is what "without building anything" always meant.
void GpuVkState::repaint() {
  if (!gpu_vk_enabled() || !s_inited || s_headless) return;
  SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(s_dev);
  GPUCHK(cmd, "AcquireGPUCommandBuffer");
  show_present_image(cmd);
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
void gpu_vk_present_image(Core* core, const uint8_t* rgba, int iw, int ih, float fade) {
  Game* game = core ? core->game : nullptr;
  if (!gpu_vk_enabled() || iw <= 0 || ih <= 0) return;
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

  if (s_headless) { gpu_submit(cmd, "gpu_vk_present_image"); return; }   // caller PPM-dumps its own rgba headless

  SDL_GPUTexture* swaptex = NULL; Uint32 sw = 0, sh = 0;
  if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, s_win, &swaptex, &sw, &sh) || !swaptex) {
    gpu_submit(cmd, "gpu_vk_present_image"); poll_quit(game); return;
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
  gpu_submit(cmd, "gpu_vk_present_image");
  poll_quit(game);
}

// ---- readback (shot / vram dump): download THIS Game's VRAM image → host, decode 1555 → PPM ---------
static const uint16_t* readback_vram(GpuVkState& g) {
  // STEP TRACE (PSXPORT_DEBUG=rbtrace). Kept, not temporary: this is what falsified issue 0018's
  // recorded diagnosis. That issue said this function BLOCKED from three call sites; the trace shows
  // enter -> targets ok -> cmd acquired -> submitted -> fence signalled every time, and the real fault
  // was a null optional GameHooks call at the DUMP site. A claim that a specific call blocks should be
  // cheap to check rather than re-argued.
  lucent::debug("rbtrace", "enter");
  g.ensure_targets();
  lucent::debug("rbtrace", "targets ok");
  SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(s_dev); GPUCHK(cmd, "AcquireGPUCommandBuffer");
  lucent::debug("rbtrace", "cmd acquired");
  SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
  SDL_GPUTextureRegion srcr = {}; srcr.texture = g.s_vram_tex; srcr.w = VRAM_W; srcr.h = VRAM_H; srcr.d = 1;
  SDL_GPUTextureTransferInfo dsti = {}; dsti.transfer_buffer = g.s_rb_xfer; dsti.pixels_per_row = VRAM_W; dsti.rows_per_layer = VRAM_H;
  SDL_DownloadFromGPUTexture(cp, &srcr, &dsti);
  SDL_EndGPUCopyPass(cp);
  lucent::debug("rbtrace", "copy pass ended, submitting");
  // Bounded submit+wait. On a fault the transfer buffer holds whatever was last in it, so returning it
  // would hand the caller a STALE VRAM image that looks like a real readback — refuse instead.
  if (!gpu_submit_and_wait(cmd, "readback_vram")) {
    lucent::error("gpu_vk", "readback_vram: no VRAM image this call — the GPU is latched off. Callers get "
                            "nullptr rather than the previous frame's bytes.");
    return nullptr;
  }
  lucent::debug("rbtrace", "fence signalled");
  const uint16_t* p = (const uint16_t*)SDL_MapGPUTransferBuffer(s_dev, g.s_rb_xfer, false);
  if (cfg_on("PSXPORT_GPU_TRACE")) { long nz = 0; for (long i = 0; i < (long)VRAM_W * VRAM_H; i++) if (p[i]) nz++;
    lucent::info("gpu_vk", "readback nonzero={}/{}", nz, VRAM_W * VRAM_H); }
  return p;
}
// ---- present_shot: THE INSTRUMENT THAT WAS MISSING — read back what the player sees ----------------
//
// Every other capture in this framework (shot / dump_to / gpu_vk_render_readback / PSXPORT_GPU_DUMP)
// reads GUEST VRAM, i.e. the stage BEFORE the composite. instruments.md INST-18 records what that cost:
// "nothing in this port samples the swapchain, so every 'the picture is correct' result in this repo is
// a claim about VRAM", after PSXPORT_SHOT_AT certified an intro at 99.95% non-black that the user was
// watching go black. This reads back s_present_img instead — after the letterbox, the fade, the source
// selection and the 24bpp decode — so its answer is about the picture, in EITHER leg.
//
// It cannot silently return an empty file: with no image it says so and writes nothing, because a
// plausible black PPM is exactly how the earlier instruments lied.
void GpuVkState::present_shot(const char* path) {
  bool image_write_rgb24(const char*, const unsigned char*, int, int);   // defined below — PNG by default; false = nothing written
  if (!gpu_vk_enabled() || !s_inited) { lucent::warn("present_shot", "GPU not active — NOTHING captured"); return; }
  if (!s_present_img) {
    lucent::warn("present_shot", "no present image yet (no frame has been composited) — NOTHING captured");
    return;
  }
  const int w = s_present_img_w, h = s_present_img_h;
  SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(s_dev); GPUCHK(cmd, "present_shot cmd");
  SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
  SDL_GPUTextureRegion srcr = {}; srcr.texture = s_present_img; srcr.w = (Uint32)w; srcr.h = (Uint32)h; srcr.d = 1;
  SDL_GPUTextureTransferInfo dsti = {}; dsti.transfer_buffer = s_present_rb;
  dsti.pixels_per_row = (Uint32)w; dsti.rows_per_layer = (Uint32)h;
  SDL_DownloadFromGPUTexture(cp, &srcr, &dsti);
  SDL_EndGPUCopyPass(cp);
  if (!gpu_submit_and_wait(cmd, "present_shot")) {
    lucent::error("present_shot", "NOTHING captured for {} — the GPU faulted and is latched off. The "
                                  "transfer buffer still holds an older frame; writing it would be a "
                                  "capture of the wrong moment presented as this one.", path ? path : "(null)");
    return;
  }
  const uint8_t* rgba = (const uint8_t*)SDL_MapGPUTransferBuffer(s_dev, s_present_rb, false);
  unsigned char* rgb = (unsigned char*)malloc((size_t)w * h * 3);
  if (!rgb) { SDL_UnmapGPUTransferBuffer(s_dev, s_present_rb); lucent::error("present_shot", "out of memory — NOTHING captured"); return; }
  // The picture is already final: no fade to apply, no format to decode. Anything this function did to
  // the pixels beyond dropping alpha would be the instrument editing its own measurement.
  long nonblack = 0;
  for (long i = 0; i < (long)w * h; i++) {
    rgb[i*3+0] = rgba[i*4+0]; rgb[i*3+1] = rgba[i*4+1]; rgb[i*3+2] = rgba[i*4+2];
    if (rgba[i*4+0] || rgba[i*4+1] || rgba[i*4+2]) nonblack++;
  }
  SDL_UnmapGPUTransferBuffer(s_dev, s_present_rb);
  const bool wrote = image_write_rgb24(path, rgb, w, h);
  free(rgb);
  if (!wrote) {
    // The measurement is real but the FILE is not, and saying "wrote" here is how this instrument
    // would certify a capture that does not exist. errno carries the reason (ENOENT, EACCES, ENOSPC).
    lucent::error("present_shot", "NOTHING captured for {} (image_write said why) — the picture itself was {:.2f}% non-black",
                  path ? path : "(null)", 100.0 * (double)nonblack / ((double)w * h));
    return;
  }
  // The coverage rides WITH the file, unconditionally: an all-black present shot is a real and
  // important answer, and it must be distinguishable from a capture that never happened.
  lucent::info("present_shot", "wrote {} ({}x{} {} sink) non-black {}/{} ({:.2f}%)",
               path ? path : "(null)", w, h, s_headless ? "headless" : "windowed",
               nonblack, (long)w * h, 100.0 * (double)nonblack / ((double)w * h));
}

#include <SDL3_image/SDL_image.h>
// THE one place any shot/dump turns an RGB24 buffer into a file. Format is chosen by extension, and
// PNG is the DEFAULT (a bare or unknown extension writes PNG) so callers get a directly-viewable file
// with no PPM->PNG convert step — only an explicit `.ppm` path keeps the raw P6 dump. Shared by the VK
// readback (dump_to) and the software-GPU shot (gpu_native.cpp) so the rule can't drift between them.
//
// RETURNS FALSE WHEN NOTHING WAS WRITTEN, and that return type is the whole point of this function's
// last revision. It used to be `void` with a bare `if (f)`, so a missing scratch/screenshots/ made
// every capture a silent no-op while its caller logged "wrote <path>" with a coverage percentage.
// MEASURED 2026-08-05 on the real spider1 build from a cwd without that directory: 2 armed captures,
// 2 cheerful success lines, 0 files on disk. That is this project's cardinal failure — an instrument
// certifying a measurement it never took — committed by the very code written to stop it. The parent
// directory is now created (nothing else in the framework creates scratch/screenshots), and every
// failure path returns false so the caller must say so.
// WHY THE REASON IS LOGGED HERE AND NOT BY THE CALLER: only this function knows which step failed,
// and the obvious caller-side `strerror(errno)` is a LIE — Fs::ensureParentDirs goes through
// std::filesystem, which reports via error_code and does not set errno, so a failed mkdir left the
// caller printing whatever unrelated errno happened to be lying around. It printed "Timer expired"
// for a path that could not be created. A diagnostic that states a confidently wrong cause is worse
// than one that states none, so the cause is named at the site that actually has it.
bool image_write_rgb24(const char* path, const unsigned char* rgb, int w, int h) {
  if (!path || !rgb || w <= 0 || h <= 0) {
    lucent::error("image_write", "refusing to write: path={} rgb={} {}x{}",
                  path ? path : "(null)", (const void*)rgb, w, h);
    return false;
  }
  if (!Fs::ensureParentDirs(path)) {
    lucent::error("image_write", "cannot create the parent directory of {} — NOTHING written", path);
    return false;
  }
  size_t n = strlen(path);
  bool ppm = (n > 4 && strcmp(path + n - 4, ".ppm") == 0);
  if (ppm) {
    FILE* f = fopen(path, "wb");
    if (!f) { lucent::error("image_write", "fopen({}) failed: {}", path, strerror(errno)); return false; }
    bool ok = fprintf(f, "P6\n%d %d\n255\n", w, h) > 0
           && fwrite(rgb, 3, (size_t)w * h, f) == (size_t)w * h;
    if (!ok) lucent::error("image_write", "short write to {}: {}", path, strerror(errno));
    if (fclose(f) != 0) { lucent::error("image_write", "fclose({}) failed: {}", path, strerror(errno)); ok = false; }
    return ok;
  }
  SDL_Surface* s = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGB24, (void*)rgb, w * 3);
  if (!s) { lucent::error("image_write", "SDL_CreateSurfaceFrom failed for {}: {}", path, SDL_GetError()); return false; }
  bool ok = IMG_SavePNG(s, path);
  if (!ok) lucent::error("image_write", "IMG_SavePNG({}) failed: {}", path, SDL_GetError());
  SDL_DestroySurface(s);
  return ok;
}
static bool dump_to(GpuVkState& g, const char* path, int sx, int sy, int w, int h,
                    int fade_mode, uint8_t fade_r, uint8_t fade_g, uint8_t fade_b) {
  // readback_vram returns nullptr when the GPU is latched off. Refuse by NAME here: dereferencing it
  // would segfault, and pretending success would emit garbage as a measurement.
  const uint16_t* vram = readback_vram(g);
  if (!vram) { lucent::error("gpu_vk", "shot24: NOTHING captured — GPU latched off"); return false; }
  unsigned char* rgb = (unsigned char*)malloc((size_t)w * h * 3);
  if (!rgb) { SDL_UnmapGPUTransferBuffer(s_dev, g.s_rb_xfer); return false; }
  // 24bpp packs RGB888 across 1.5 VRAM halfwords per pixel, so column x of the display sits at BYTE
  // offset sx*2 + x*3 in the row — not at halfword sx+x. Decoding it as 1555 is what scrambled the
  // colours and showed two thirds of the width.
  const int rgb24 = g.s_disp_rgb24;
  for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
    int r, g_, b;
    if (rgb24) {
      const uint8_t* row = (const uint8_t*)&vram[((sy + y) % VRAM_H) * VRAM_W];
      int bx = sx * 2 + x * 3;                       // VRAM rows are VRAM_W halfwords = VRAM_W*2 bytes
      r  = row[ bx      % (VRAM_W * 2)];
      g_ = row[(bx + 1) % (VRAM_W * 2)];
      b  = row[(bx + 2) % (VRAM_W * 2)];
    } else {
      uint16_t p = vram[((sy + y) % VRAM_H) * VRAM_W + ((sx + x) & 1023)];
      r = (p & 31) << 3; g_ = ((p >> 5) & 31) << 3; b = ((p >> 10) & 31) << 3;
    }
    int g = g_;
    if (fade_mode == 1)      { r += fade_r; g += fade_g; b += fade_b; if (r>255)r=255; if (g>255)g=255; if (b>255)b=255; }
    else if (fade_mode == 2) { r -= fade_r; g -= fade_g; b -= fade_b; if (r<0)r=0; if (g<0)g=0; if (b<0)b=0; }
    unsigned char* c = &rgb[((size_t)y * w + x) * 3];
    c[0] = (unsigned char)r; c[1] = (unsigned char)g; c[2] = (unsigned char)b;
  }
  // Propagated, not swallowed: every VRAM shot below reports the file it ACTUALLY wrote. These
  // callers logged "wrote <path>" unconditionally too — the same lie as present_shot's, and measured
  // in the same run — so they are fixed in the same pass rather than left as the older half of a
  // rule that now only holds in one place.
  const bool wrote = image_write_rgb24(path, rgb, w, h);
  free(rgb);
  SDL_UnmapGPUTransferBuffer(s_dev, g.s_rb_xfer);
  return wrote;
}
void GpuVkState::shot(const char* path) {
  if (!gpu_vk_enabled() || !s_inited) { lucent::warn("gpu_shot", "GPU not active — NOTHING captured"); return; }
  FadeState f = fade_state_of(&game->core);
  const bool wrote = dump_to(*this, path, s_last_sx, s_last_sy, s_last_w, s_last_h, f.mode, f.r, f.g, f.b);
  if (!wrote) { lucent::error("gpu_shot", "NOTHING captured for {} (image_write said why)", path ? path : "(null)"); return; }
  lucent::info("gpu_shot", "wrote {} ({}x{} @ {},{})", path ? path : "(null)", s_last_w, s_last_h, s_last_sx, s_last_sy);
}
void GpuVkState::shot_b(const char* path) { shot(path); }   // Pass 1: single target
// GP1(0x08) bit 4 changed. The decode lives in gpu_native; the two things that DECODE the display region
// live here, so the bit has to cross over. Silently ignoring it is what made a 24bpp still render with
// every colour scrambled and only two thirds of the width shown.
void gpu_vk_set_display_depth(Core* core, int rgb24) {
  if (!core || !core->game) return;
  core->game->gpu_vk.s_disp_rgb24 = rgb24 ? 1 : 0;
}
void gpu_vk_shot_region(Core* core, const char* path, int sx, int sy, int w, int h) {
  if (!gpu_vk_enabled() || !s_inited) return;
  FadeState f = fade_state_of(core);
  if (!dump_to(core->game->gpu_vk, path, sx, sy, w, h, f.mode, f.r, f.g, f.b)) {
    lucent::error("gpu_shot", "NOTHING captured for {} (image_write said why)", path ? path : "(null)"); return; }
  lucent::info("gpu_shot", "wrote {} ({}x{} @ {},{})", path ? path : "(null)", w, h, sx, sy);
}
void gpu_vk_vram_region(Core* core, const char* path, int x, int y, int w, int h) {
  if (!gpu_vk_enabled() || !s_inited) return;
  if (!dump_to(core->game->gpu_vk, path, x, y, w, h, 0, 0, 0, 0))   // raw VRAM region dump — no engine fade applied
    lucent::error("gpu_shot", "NOTHING captured for {} (image_write said why)", path ? path : "(null)");
  lucent::info("gpu_vram", "wrote {} ({}x{} @ {},{})", path ? path : "(null)", w, h, x, y);
}
// DEBUG ONLY (temporary, ires bring-up): dump the FULL ires-scaled target verbatim (no downsample) so the
// scaled geometry pass's actual output can be inspected directly. No-op if ires isn't currently active
// (s_ires_scale <= 1 — nothing built). Uses its own command buffer/submit/fence, safe to call any time
// AFTER a present() with ires>1 has been submitted (the target is a persistent GpuVkState field).
void gpu_vk_ires_rawdump(Core* core, const char* path) {
  if (!gpu_vk_enabled() || !s_inited) return;
  GpuVkState& g = core->game->gpu_vk;
  if (g.s_ires_scale <= 1 || !g.s_ires_color) { lucent::info("ires_dbg", "no ires target built (scale={})", g.s_ires_scale); return; }
  int cw = VRAM_W * g.s_ires_scale, ch = VRAM_H * g.s_ires_scale;
  SDL_GPUTransferBufferCreateInfo dn = {}; dn.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD; dn.size = (Uint32)cw * ch * 2;
  SDL_GPUTransferBuffer* dbg_xfer = SDL_CreateGPUTransferBuffer(s_dev, &dn); GPUCHK(dbg_xfer, "ires dbg xfer");
  SDL_GPUCommandBuffer* dbg_cmd = SDL_AcquireGPUCommandBuffer(s_dev); GPUCHK(dbg_cmd, "ires dbg cmd");
  SDL_GPUCopyPass* dcp = SDL_BeginGPUCopyPass(dbg_cmd);
  SDL_GPUTextureRegion sr = {}; sr.texture = g.s_ires_color; sr.w = (Uint32)cw; sr.h = (Uint32)ch; sr.d = 1;
  SDL_GPUTextureTransferInfo di = {}; di.transfer_buffer = dbg_xfer; di.pixels_per_row = (Uint32)cw; di.rows_per_layer = (Uint32)ch;
  SDL_DownloadFromGPUTexture(dcp, &sr, &di);
  SDL_EndGPUCopyPass(dcp);
  if (!gpu_submit_and_wait(dbg_cmd, "GpuVkState::shot")) {
    lucent::error("gpu_vk", "shot: NOTHING written to {} — the GPU faulted and is latched off.",
                  path ? path : "(null)");
    return;
  }
  const uint16_t* px = (const uint16_t*)SDL_MapGPUTransferBuffer(s_dev, dbg_xfer, false);
  FILE* f = fopen(path, "wb");
  if (f) {
    fprintf(f, "P6\n%d %d\n255\n", cw, ch);
    for (int y = 0; y < ch; y++) for (int x = 0; x < cw; x++) {
      uint16_t p = px[y * cw + x]; int r=(p&31)<<3, gg=((p>>5)&31)<<3, b=((p>>10)&31)<<3;
      unsigned char c[3] = {(unsigned char)r,(unsigned char)gg,(unsigned char)b}; fwrite(c,1,3,f);
    }
    fclose(f);
  }
  SDL_UnmapGPUTransferBuffer(s_dev, dbg_xfer);
  SDL_ReleaseGPUTransferBuffer(s_dev, dbg_xfer);
  lucent::info("ires_dbg", "rawdump {}x{} -> {}", cw, ch, path ? path : "(null)");
}
// Raw 16-bit VRAM words at (x,y..y+n-1 wrapped along X) — dark-outline STP-bit diag (2026-07-01,
// scratch/handoff.md): tells apart a genuine opaque texel (STP=0, faithful) from a lost/miscomputed
// STP bit (would-be-blended texel drawing solid instead of translucent).
void gpu_vk_vram_words(Core* core, int x, int y, int n, uint16_t* out) {
  if (!gpu_vk_enabled() || !s_inited) { for (int i = 0; i < n; i++) out[i] = 0; return; }
  GpuVkState& g = core->game->gpu_vk;
  // readback_vram returns nullptr when the GPU is latched off. Refuse by NAME here: dereferencing it
  // would segfault, and pretending success would emit garbage as a measurement.
  const uint16_t* vram = readback_vram(g);
  if (!vram) { lucent::error("gpu_vk", "vram row read of {} word(s) at ({},{}) UNAVAILABLE — GPU latched "
                                       "off; `out` is left untouched, not zero-filled, so a caller cannot "
                                       "mistake a fault for a black row", n, x, y); return; }
  for (int i = 0; i < n; i++) out[i] = vram[(y % VRAM_H) * VRAM_W + ((x + i) & 1023)];
  SDL_UnmapGPUTransferBuffer(s_dev, g.s_rb_xfer);
}
void gpu_vk_vram_raw(Core* core, const char* path) {
  if (!gpu_vk_enabled() || !s_inited) return;
  GpuVkState& g = core->game->gpu_vk;
  // readback_vram returns nullptr when the GPU is latched off. Refuse by NAME here: dereferencing it
  // would segfault, and pretending success would emit garbage as a measurement.
  const uint16_t* vram = readback_vram(g);
  if (!vram) { lucent::error("gpu_vk", "vramdump: NOTHING written to {} — GPU latched off",
                             path ? path : "(null)"); return; }
  FILE* f = fopen(path, "wb"); if (!f) { SDL_UnmapGPUTransferBuffer(s_dev, g.s_rb_xfer); return; }
  for (int y = 0; y < VRAM_H; y++) fwrite(&vram[y * VRAM_W], 2, VRAM_W, f);
  fclose(f);
  SDL_UnmapGPUTransferBuffer(s_dev, g.s_rb_xfer);
  lucent::info("gpu_vram", "wrote RAW {} ({}x{} u16)", path ? path : "(null)", VRAM_W, VRAM_H);
}

// ---- per-prim state setters (depth/order; consumed by the 3D raster) --------------------------------
void GpuVkState::set_vd(const float* d3) { s_vd = d3; }
void GpuVkState::set_vd_n(const float* d3) { s_vdn = d3; }
void GpuVkState::set_xyf(const float* xf, const float* yf) { s_xf = xf; s_yf = yf; }
// Paint-order z-fight TIEBREAK. ZBIAS_UNIT = the depth nudge per emit-order step; ZBIAS_MAX = the reserved
// headroom cap (the accumulated bias never exceeds this, so it can never push a 3D prim past NATIVE_3D_MAX
// into the 2D/HUD band nor overrun a genuine world depth separation — measured world separations near the
// camera are ~1e-3+, an order of magnitude above ZBIAS_MAX). Tunable via PSXPORT_ZBIAS for sweeps.
// Exposed (non-static) for the zfight scanner (render_queue.cpp) so it can model the fix without a re-run.
float gpu_zbias_unit() { static float u = -1.f; if (u < 0.f) { const char* e = cfg_str("PSXPORT_ZBIAS");
                                                                u = e ? (float)atof(e) : 4e-7f; if (u < 0.f) u = 0.f; } return u; }
// The cap is a SWEEP knob too (PSXPORT_ZBIAS_MAX): raising the unit alone changes nothing once the
// accumulated bias saturates, so a "can paint order resolve this at all?" experiment needs both. The
// shipped default is unchanged; only a deliberate sweep moves it.
static float zbias_max() { static float m = -1.f; if (m < 0.f) { const char* e = cfg_str("PSXPORT_ZBIAS_MAX");
                                                                 m = e ? (float)atof(e) : 1.5e-3f; if (m < 0.f) m = 0.f; } return m; }
bool gpu_vk_order_bias_distinguishes(uint32_t seq){ float u=gpu_zbias_unit(),m=zbias_max(); return u>0.f && (double)seq*u < m; }
void GpuVkState::set_order(unsigned idx) { if(s_order_override>=0){ idx=(unsigned)s_order_override; s_order_override=-1; }
                                           s_cur_ord = (float)(idx + 1) / 65536.0f; if (s_cur_ord > 1.0f) s_cur_ord = 1.0f;
                                           s_cur_ordn = s_cur_ord; s_vd = 0; s_vdn = 0; s_xf = 0; s_yf = 0;
                                           const float cap = zbias_max();
                                           float b = (float)idx * gpu_zbias_unit(); s_depth_bias = b > cap ? cap : b; }
void GpuVkState::set_order_2d(unsigned idx) { float t = (float)(idx + 1) / 65536.0f; if (t > 1.0f) t = 1.0f;
                                              s_cur_ord = NATIVE_3D_MAX + (1.0f - NATIVE_3D_MAX) * t; s_vd = 0; }
void GpuVkState::set_order_2d_n(unsigned idx) { float t = (float)(idx + 1) / 65536.0f; if (t > 1.0f) t = 1.0f;
                                                s_cur_ordn = NATIVE_3D_MAX + (1.0f - NATIVE_3D_MAX) * t; s_vdn = 0; }
void GpuVkState::set_order_2d_bg(unsigned idx) { float t = (float)(idx + 1) / 65536.0f; if (t > 1.0f) t = 1.0f;
                                                 s_cur_ord = NATIVE_3D_MIN * t; s_vd = 0; }
void GpuVkState::set_order_2d_bg_n(unsigned idx) { float t = (float)(idx + 1) / 65536.0f; if (t > 1.0f) t = 1.0f;
                                                   s_cur_ordn = NATIVE_3D_MIN * t; s_vdn = 0; }
void GpuVkState::semi_group(int x0, int y0, int x1, int y1) { (void)x0; (void)y0; (void)x1; (void)y1; }
// Every CPU->VRAM write path funnels here (GP0 0xA0 upload, GP0 0x02 fill, VRAM->VRAM copy, native
// load_image). Two consumers, and they need different things from it:
//   * s_vram_writes — the COUNT. A write with no primitives is a complete new picture (an upload-only
//     logo/loading screen), so present() must rebuild for it. See gpu_vk_present_policy.h.
//   * s_dirty — the RECT. The composite is a persistent framebuffer, so a present re-uploads only what
//     the guest wrote; uploading all of VRAM erases everything the rasterizer drew. See vram_dirty.h.
// The rect used to be discarded here (`(void)x; ...`) precisely because the upload was unconditional.
void GpuVkState::dirty(int x, int y, int w, int h) { s_vram_writes++; s_dirty.add(x, y, w, h); }
void GpuVkState::stats(int* tri, int* tex, int* semi) { if (tri) *tri = s_dbg_tri_c; if (tex) *tex = s_dbg_tex_c; if (semi) *semi = s_dbg_semi_c; }
// Per-logic-frame reset of the host geometry batch (present consumed it; the next frame re-emits the queue).
void GpuVkState::frame_end(const uint16_t* svram, int frame) { (void)svram; (void)frame;
  // PRESENT-SEQUENCE capture (REPL `preseq <N> [dir]`): frame_end runs once per PRESENT PASS — the
  // real pass AND the fps60 interp pass, windowed AND headless — so dumping here (before the batch
  // reset; dump_to's readback renders the pending batch) interleaves real/interp frames for
  // tools/preseq_flicker.py's 30Hz-oscillation detection.
  if (s_preseq_left > 0) {
    char p[192]; snprintf(p, sizeof p, "%s/p%04d.ppm", s_preseq_dir, s_preseq_idx++);
    FadeState f = fade_state_of(&game->core);
    dump_to(*this, p, s_last_sx, s_last_sy, s_last_w, s_last_h, f.mode, f.r, f.g, f.b);
    if (--s_preseq_left == 0) lucent::info("preseq", "done: {} frames -> {}", s_preseq_idx, s_preseq_dir);
  }
  s_tri_n = s_tex_n = 0;
  s_painter_tex_n = s_painter_tri_n = s_painter_cmd_n = s_painter_ranges = s_painter_active = s_painter_overflow = 0;
  for (int m = 0; m < NUM_BLEND_MODES; m++) s_semi_n[m] = 0;
  for (int band = 0; band < GGS_NUM_2D_BANDS; band++) {
    s_tri2d_n[band] = s_tex2d_n[band] = 0;
    for (int m = 0; m < NUM_BLEND_MODES; m++) s_semi2d_n[band][m] = 0;
  }
}

// ---- native 3D / textured raster: accumulate the tee'd geometry into the host batch (Pass 2) ---------
// Append one flat triangle (VRAM coords + per-vertex RGB 0..255); depth = per-vertex native (s_vd) or the
// per-prim OT-order (s_cur_ord) band.
// Lazy-alloc THIS core's CPU vertex batches on first use (the VK backend's create_3d already ran on
// s_have_3d = 1, but the batches live on GpuVkState now so each Core allocates its own).
static inline void ggs_alloc_batches(GpuVkState& g) {
  if (!g.s_tri_buf)  g.s_tri_buf  = (TriVtx*)malloc(sizeof(TriVtx) * TRI_CAP);
  if (!g.s_tex_buf)  g.s_tex_buf  = (TexVtx*)malloc(sizeof(TexVtx) * TEX_CAP);
  if (!g.s_painter_tex_buf) g.s_painter_tex_buf = (TexVtx*)malloc(sizeof(TexVtx) * TEX_CAP);
  if (!g.s_painter_tri_buf) g.s_painter_tri_buf = (TriVtx*)malloc(sizeof(TriVtx) * TRI_CAP);
  for (int m = 0; m < NUM_BLEND_MODES; m++)
    if (!g.s_semi_buf[m]) g.s_semi_buf[m] = (TexVtx*)malloc(sizeof(TexVtx) * TEX_CAP);
  for (int band = 0; band < GGS_NUM_2D_BANDS; band++) {
    if (!g.s_tri2d_buf[band]) g.s_tri2d_buf[band] = (TriVtx*)malloc(sizeof(TriVtx) * TRI2D_CAP);
    if (!g.s_tex2d_buf[band]) g.s_tex2d_buf[band] = (TexVtx*)malloc(sizeof(TexVtx) * TEX2D_CAP);
    for (int m = 0; m < NUM_BLEND_MODES; m++)
      if (!g.s_semi2d_buf[band][m]) g.s_semi2d_buf[band][m] = (TexVtx*)malloc(sizeof(TexVtx) * TEX2D_CAP);
  }
}

bool GpuVkState::painter_begin(uint32_t object) {
  ggs_alloc_batches(*this);
  if (!object || s_painter_active || s_painter_ranges >= 256) return false;
  s_painter_active = 1;
  s_painter_object[s_painter_ranges] = object;
  s_painter_first[s_painter_ranges] = s_painter_cmd_n;
  return true;
}

bool GpuVkState::painter_end() {
  if (!s_painter_active) return false;
  s_painter_count[s_painter_ranges] = s_painter_cmd_n - s_painter_first[s_painter_ranges];
  s_painter_active = 0;
  ++s_painter_ranges;
  return !s_painter_overflow && s_painter_count[s_painter_ranges - 1] > 0;
}
void GpuVkState::painter_staging_stats(int* ordinary, int* painter, int* ranges) const {
  if (ordinary) *ordinary = s_tex_n;
  if (painter) *painter = s_painter_tex_n + s_painter_tri_n;
  if (ranges) *ranges = s_painter_ranges;
}

// bug #55 classifier: is the CURRENT prim (about to be appended by draw_tri/draw_tritri/draw_semi) 3D
// world geometry, or which 2D band does it belong to? s_vd (per-vertex native depth) is set ONLY for
// RQ_OM_DEPTH world prims (render_queue.cpp RQ_SETVD) and freshly cleared by set_order()/set_order_2d*
// before every draw (see those bodies below) — so it is a reliable, always-current per-draw signal.
// Non-3D prims are banded by their already-assigned order value: RQ_OM_2D_BG's set_order_2d_bg() writes
// s_cur_ord in (0, NATIVE_3D_MIN], RQ_OM_2D_FG's set_order_2d() writes it in (NATIVE_3D_MAX, 1] — the
// SAME non-overlapping bands render_geom's 3D-band clamp (ord3d_b) already relies on.
static inline bool ggs_is_3d(const GpuVkState& g) { return g.s_vd != nullptr; }
static inline int  ggs_2d_band(const GpuVkState& g) { return g.s_cur_ord <= NATIVE_3D_MIN ? GGS_2D_BG : GGS_2D_FG; }

static bool painter_command(GpuVkState& g, int material, int first, int count) {
  if (!g.s_painter_active || count <= 0) return false;
  // Never coalesce across an object boundary: each object clears and composites the reusable target.
  if (g.s_painter_cmd_n > g.s_painter_first[g.s_painter_ranges]) {
    const int i=g.s_painter_cmd_n-1;
    if (g.s_painter_cmd_material[i]==material &&
        g.s_painter_cmd_gouraud[i]==g.s_painter_item_gouraud &&
        g.s_painter_cmd_dither[i]==g.s_painter_item_dither &&
        g.s_painter_cmd_first[i]+g.s_painter_cmd_count[i]==first) {
      g.s_painter_cmd_count[i]+=count; return true;
    }
  }
  if (g.s_painter_cmd_n >= 16384) { g.s_painter_overflow=1; return false; }
  const int i=g.s_painter_cmd_n++;
  g.s_painter_cmd_material[i]=(uint8_t)material;
  g.s_painter_cmd_gouraud[i]=(uint8_t)g.s_painter_item_gouraud;
  g.s_painter_cmd_dither[i]=(uint8_t)g.s_painter_item_dither;
  g.s_painter_cmd_first[i]=first; g.s_painter_cmd_count[i]=count;
  return true;
}

void GpuVkState::draw_tri(int x0,int y0,int r0,int g0,int b0, int x1,int y1,int r1,int g1,int b1,
                          int x2,int y2,int r2,int g2,int b2, int dax0,int day0,int dax1,int day1) {
  ggs_alloc_batches(*this);
  const int32_t da[4] = { dax0, day0, dax1, day1 };
  if (s_painter_active) {
    if (s_painter_tri_n + 3 > TRI_CAP || !painter_command(*this,0,s_painter_tri_n,3)) { s_painter_overflow=1; return; }
    TriVtx* v=((TriVtx*)s_painter_tri_buf)+s_painter_tri_n;
    const float gf=(float)s_painter_item_gouraud, df=(float)s_painter_item_dither;
    v[0]={(float)x0,(float)y0,r0/255.f,g0/255.f,b0/255.f,ord3d_b(s_vd[0],s_depth_bias),gf,df,{da[0],da[1],da[2],da[3]}};
    v[1]={(float)x1,(float)y1,r1/255.f,g1/255.f,b1/255.f,ord3d_b(s_vd[1],s_depth_bias),gf,df,{da[0],da[1],da[2],da[3]}};
    v[2]={(float)x2,(float)y2,r2/255.f,g2/255.f,b2/255.f,ord3d_b(s_vd[2],s_depth_bias),gf,df,{da[0],da[1],da[2],da[3]}};
    s_painter_tri_n+=3; return;
  }
  // bug #55: route 2D (non-world) flat tris into the native-resolution 2D bands instead of the 3D-world
  // batch — see ggs_is_3d/ggs_2d_band above and render_geom's band split below.
  if (!ggs_is_3d(*this)) {
    int band = ggs_2d_band(*this);
    if (s_tri2d_n[band] + 3 > TRI2D_CAP) return;
    TriVtx* v = ((TriVtx*)s_tri2d_buf[band]) + s_tri2d_n[band];
    v[0] = { (float)x0, (float)y0, r0/255.f, g0/255.f, b0/255.f, s_cur_ord, 0, 0, {da[0],da[1],da[2],da[3]} };
    v[1] = { (float)x1, (float)y1, r1/255.f, g1/255.f, b1/255.f, s_cur_ord, 0, 0, {da[0],da[1],da[2],da[3]} };
    v[2] = { (float)x2, (float)y2, r2/255.f, g2/255.f, b2/255.f, s_cur_ord, 0, 0, {da[0],da[1],da[2],da[3]} };
    s_tri2d_n[band] += 3;
    return;
  }
  if (s_tri_n + 3 > TRI_CAP) return;
  TriVtx* v = ((TriVtx*)s_tri_buf) + s_tri_n;
  v[0] = { (float)x0, (float)y0, r0/255.f, g0/255.f, b0/255.f, ord3d_b(s_vd[0], s_depth_bias), 0, 0, {da[0],da[1],da[2],da[3]} };
  v[1] = { (float)x1, (float)y1, r1/255.f, g1/255.f, b1/255.f, ord3d_b(s_vd[1], s_depth_bias), 0, 0, {da[0],da[1],da[2],da[3]} };
  v[2] = { (float)x2, (float)y2, r2/255.f, g2/255.f, b2/255.f, ord3d_b(s_vd[2], s_depth_bias), 0, 0, {da[0],da[1],da[2],da[3]} };
  s_tri_n += 3;
}
// Fill 3 textured vertices: per-vertex pos/uv/color + shared page/CLUT/window/clip/semi/blend state. Uses
// the sub-pixel float XY (s_xf/s_yf) for the world path when set, else the integer xs/ys (2D/HUD).
void GpuVkState::tex_emit(TexVtx* t, const int* xs, const int* ys, const int* us, const int* vs,
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
void GpuVkState::draw_tritri(const int* xs, const int* ys, const int* us, const int* vs,
                             const unsigned char* rs, const unsigned char* gs, const unsigned char* bs,
                             int tpx, int tpy, int mode, int raw, int clutx, int cluty,
                             int twmx, int twmy, int twox, int twoy, int dax0, int day0, int dax1, int day1) {
  ggs_alloc_batches(*this);
  if (s_painter_active) {
    if (s_painter_tex_n + 3 > TEX_CAP) { s_painter_overflow = 1; return; }
    if (!painter_command(*this,1,s_painter_tex_n,3)) { s_painter_overflow=1; return; }
    tex_emit(((TexVtx*)s_painter_tex_buf) + s_painter_tex_n, xs, ys, us, vs, rs, gs, bs,
             tpx,tpy,mode,raw,clutx,cluty,twmx,twmy,twox,twoy,dax0,day0,dax1,day1,0,0);
    s_painter_tex_n += 3;
    return;
  }
  // bug #55: 2D (non-world) textured tris render at native resolution, never through the ires target.
  if (!ggs_is_3d(*this)) {
    int band = ggs_2d_band(*this);
    if (s_tex2d_n[band] + 3 > TEX2D_CAP) return;
    tex_emit(((TexVtx*)s_tex2d_buf[band]) + s_tex2d_n[band], xs, ys, us, vs, rs, gs, bs, tpx, tpy, mode, raw, clutx, cluty,
             twmx, twmy, twox, twoy, dax0, day0, dax1, day1, 0, 0);
    s_tex2d_n[band] += 3;
    return;
  }
  if (s_tex_n + 3 > TEX_CAP) return;
  tex_emit(((TexVtx*)s_tex_buf) + s_tex_n, xs, ys, us, vs, rs, gs, bs, tpx, tpy, mode, raw, clutx, cluty,
           twmx, twmy, twox, twoy, dax0, day0, dax1, day1, 0, 0);
  s_tex_n += 3;
}
void GpuVkState::draw_semi(const int* xs, const int* ys, const int* us, const int* vs,
                           const unsigned char* rs, const unsigned char* gs, const unsigned char* bs,
                           int tpx, int tpy, int mode, int raw, int clutx, int cluty,
                           int twmx, int twmy, int twox, int twoy, int dax0, int day0, int dax1, int day1, int blend) {
  int m = blend & 3;   // bucket by PSX blend mode: one HW-blend pipeline/vertex-buffer per mode (see render_geom)
  ggs_alloc_batches(*this);
  // bug #55: 2D (non-world) semi/translucent tris render at native resolution, never through the ires target.
  if (!ggs_is_3d(*this)) {
    int band = ggs_2d_band(*this);
    if (s_semi2d_n[band][m] + 3 > TEX2D_CAP) return;
    tex_emit(((TexVtx*)s_semi2d_buf[band][m]) + s_semi2d_n[band][m], xs, ys, us, vs, rs, gs, bs, tpx, tpy, mode, raw, clutx, cluty,
             twmx, twmy, twox, twoy, dax0, day0, dax1, day1, 1, blend);
    s_semi2d_n[band][m] += 3;
    return;
  }
  if (s_semi_n[m] + 3 > TEX_CAP) return;
  tex_emit(((TexVtx*)s_semi_buf[m]) + s_semi_n[m], xs, ys, us, vs, rs, gs, bs, tpx, tpy, mode, raw, clutx, cluty,
           twmx, twmy, twox, twoy, dax0, day0, dax1, day1, 1, blend);
  s_semi_n[m] += 3;
}
void GpuVkState::tri_render_and_readback(uint16_t* out) { (void)out; }
void GpuVkState::tri_over_bg_readback(const uint16_t* bg, uint16_t* out) { (void)bg; (void)out; }
void GpuVkState::panel_upload(Panel* p) { (void)p; }
void GpuVkState::panel_render(Panel* p) { (void)p; }
void GpuVkState::ssao_pass() {}
void GpuVkState::shadow_pass() {}
// PSXPORT_GPU_SELFTEST=1: headless renderer self-test, then exit. Renders a KNOWN VRAM pattern through the
// REAL present pipeline (present.vert/frag) into an offscreen RGBA8 target, reads it back, and asserts:
//   (1) ORIENTATION — VRAM row 0 (top) lands at the TOP of the output, not the bottom. This is the
//       regression guard for the SDL_GPU swapchain Y-flip (the "rendering upside down" bug).
//   (2) 1555 UNPACK — the present.frag 1555→RGB decode is correct (red marker decodes red, blue→blue).
// No disc/game needed (boot.cpp calls this before load_exe). Prints PASS/FAIL and exits with that status.
void GpuVkState::tritest() {
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
  upload_vram(*this, cmd, pat, kWholeVram, 1);   // a synthetic full-VRAM pattern: all of it
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
  if (!gpu_submit_and_wait(cmd, "GpuVkState::tritest")) {
    lucent::error("gpu_vk", "tritest: INCONCLUSIVE — the GPU faulted and is latched off, so this "
                            "self-test neither passed nor failed. Do not read it as a pass.");
    return;
  }

  const uint8_t* px = (const uint8_t*)SDL_MapGPUTransferBuffer(s_dev, dl, false);
  const uint8_t* top = px + ((size_t)(TH / 4) * TW + TW / 2) * 4;        // y=60: should be RED (VRAM row ~64)
  const uint8_t* bot = px + ((size_t)(3 * TH / 4) * TW + TW / 2) * 4;    // y=180: should be BLUE (VRAM row ~448)
  int top_red  = (top[0] > 200 && top[1] < 60 && top[2] < 60);
  int bot_blue = (bot[2] > 200 && bot[0] < 60 && bot[1] < 60);
  int ok = top_red && bot_blue;
  lucent::info("gpu_selftest", "top({},{},{}) expect RED  bottom({},{},{}) expect BLUE  orientation+1555 => {}", top[0], top[1], top[2], bot[0], bot[1], bot[2], ok ? "PASS" : "FAIL");
  if (!ok && bot[0] > 200 && top[2] > 200)
    lucent::info("gpu_selftest", "(top is BLUE, bottom is RED → image is UPSIDE DOWN — the swapchain Y-flip regressed)");
  SDL_UnmapGPUTransferBuffer(s_dev, dl);

  // Shipping painter discriminator: two authored textured faces cross at the same pixels; the later
  // face is farther and must nevertheless win locally, exporting that FAR depth. An ordinary world
  // face behind it loses on the left, while one in front wins on the right. Read back BOTH RG8 and D32.
  if (cfg_on("PSXPORT_PAINTER_GPU_SELFTEST")) {
    frame_end(nullptr,0); ensure_targets(); game->mods.ires=2;
    auto tri=[&](bool painter,int x0,int x1,float z,uint32_t seq,int vrow,int color){
      int x[3]={x0,x1,(x0+x1)/2}, y[3]={20,20,180}, u[3]={0,0,0}, v[3]={vrow,vrow,vrow};
      unsigned char c[3]={(unsigned char)(color?255:128),(unsigned char)(color?255:128),(unsigned char)(color?255:128)};
      float d[3]={z,z,z}; set_order(seq); set_vd(d);
      if(painter) draw_tritri(x,y,u,v,c,c,c,0,0,2,1,0,0,0,0,0,0,0,0,1023,511);
      else draw_tri(x[0],y[0],color?0:0,color?255:255,0,x[1],y[1],0,255,0,x[2],y[2],0,255,0, 0,0,1023,511);
    };
    auto utri=[&](int x0,int x1,float z,uint32_t seq,int r,int g,int b,bool gouraud,bool dither){
      float d[3]={z,z,z}; set_order(seq); set_vd(d);
      s_painter_item_gouraud=gouraud; s_painter_item_dither=dither;
      draw_tri(x0,20,r,g,b,x1,20,r,g,b,(x0+x1)/2,180,r,g,b, 0,0,1023,511);
    };
    lucent::info("gpu_selftest","painter ord inputs ordinary-left={:.9f} painter-near={:.9f} painter-far={:.9f} ordinary-right={:.9f}",ord3d_b(.20f,0),ord3d_b(.80f,gpu_zbias_unit()),ord3d_b(.30f,2*gpu_zbias_unit()),ord3d_b(.70f,3*gpu_zbias_unit()));
    tri(false,20,150,.20f,0,0,1); tri(false,170,310,.70f,3,0,1);
    if(!painter_begin(77)){ lucent::error("gpu_selftest","painter begin failed"); ok=0; }
    tri(true,20,310,.80f,1,0,0); utri(20,310,.30f,2,0,0,255,true,true);
    for(int x=0;x<VRAM_W;x++) pat[350*VRAM_W+x]=0;
    tri(true,20,310,.95f,4,350,0); // transparent later texel must not replace far blue winner
    // Non-overlapping flat face: dither flag is deliberately set but F3/flat must not dither.
    utri(2,18,.40f,5,129,0,0,false,true);
    // Unsaturated G3 at known native matrix cells: x=300,y=60 is -4, x=303,y=60 is +1.
    // At 2x, adjacent physical pixels within each native cell must match.
    utri(288,318,.40f,6,127,127,127,true,true);
    if(!painter_end()){ lucent::error("gpu_selftest","painter end failed"); ok=0; }
    SDL_GPUTransferBufferCreateInfo bdi={}; bdi.usage=SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD; bdi.size=VRAM_W*2*VRAM_H*2*4;
    s_painter_test_local_depth=SDL_CreateGPUTransferBuffer(s_dev,&bdi); GPUCHK(s_painter_test_local_depth,"painter local depth boundary");
    s_painter_test_main_depth=SDL_CreateGPUTransferBuffer(s_dev,&bdi); GPUCHK(s_painter_test_main_depth,"painter main depth boundary");
    SDL_GPUTransferBufferCreateInfo cbi=bdi; cbi.size=VRAM_W*2*VRAM_H*2*2;
    s_painter_test_local_color=SDL_CreateGPUTransferBuffer(s_dev,&cbi); GPUCHK(s_painter_test_local_color,"painter local color boundary");
    SDL_GPUCommandBuffer* pcmd=SDL_AcquireGPUCommandBuffer(s_dev); upload_vram(*this,pcmd,pat,kWholeVram,1);
    int qa,qb,qc; render_geom(*this,pcmd,pat,0,0,320,240,&qa,&qb,&qc,true);
    SDL_GPUTransferBufferCreateInfo ddi={}; ddi.usage=SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD; ddi.size=VRAM_W*VRAM_H*4;
    SDL_GPUTransferBuffer* ddl=SDL_CreateGPUTransferBuffer(s_dev,&ddi); GPUCHK(ddl,"painter depth dl");
    SDL_GPUCopyPass* pcp=SDL_BeginGPUCopyPass(pcmd);
    SDL_GPUTextureRegion cr={}; cr.texture=s_vram_tex; cr.w=VRAM_W; cr.h=VRAM_H; cr.d=1;
    SDL_GPUTextureTransferInfo co={}; co.transfer_buffer=s_rb_xfer; co.pixels_per_row=VRAM_W; co.rows_per_layer=VRAM_H; SDL_DownloadFromGPUTexture(pcp,&cr,&co);
    SDL_GPUTextureRegion dr=cr; dr.texture=s_depth; SDL_GPUTextureTransferInfo dto={}; dto.transfer_buffer=ddl; dto.pixels_per_row=VRAM_W; dto.rows_per_layer=VRAM_H; SDL_DownloadFromGPUTexture(pcp,&dr,&dto);
    SDL_EndGPUCopyPass(pcp); if(!gpu_submit_and_wait(pcmd,"painter selftest")) ok=0;
    const uint16_t* cpix=(const uint16_t*)SDL_MapGPUTransferBuffer(s_dev,s_rb_xfer,false);
    const float* dpix=(const float*)SDL_MapGPUTransferBuffer(s_dev,ddl,false);
    const float* ldpix=(const float*)SDL_MapGPUTransferBuffer(s_dev,s_painter_test_local_depth,false);
    const float* mdpix=(const float*)SDL_MapGPUTransferBuffer(s_dev,s_painter_test_main_depth,false);
    const uint16_t* lcpix=(const uint16_t*)SDL_MapGPUTransferBuffer(s_dev,s_painter_test_local_color,false);
    int li=60*VRAM_W+70, ri=60*VRAM_W+240, fi=60*VRAM_W+10; uint16_t lc=cpix[li], rc=cpix[ri], fc=cpix[fi]; float ld=dpix[li], rd=dpix[ri];
    const int sw=VRAM_W*2, lsi=(60*2)*sw+70*2, rsi=(60*2)*sw+240*2;
    lucent::info("gpu_selftest","painter D32 boundaries local-left={:.9f} local-right={:.9f} post-composite-left={:.9f} post-composite-right={:.9f} end-left={:.9f} end-right={:.9f}",ldpix[lsi],ldpix[rsi],mdpix[lsi],mdpix[rsi],ld,rd);
    // The 2D-foreground band following world composition intentionally starts by clearing the shared
    // depth attachment, so end-of-render D32 is not a valid witness. Assert the boundary snapshot taken
    // immediately after composite; the color target remains valid through the foreground pass.
    const float postl=mdpix[lsi], postr=mdpix[rsi];
    const int n0=(60*2)*sw+300*2, n1=n0+1, p0=(60*2)*sw+303*2, p1=p0+1;
    const uint16_t neg0=lcpix[n0], neg1=lcpix[n1], pos0=lcpix[p0], pos1=lcpix[p1];
    const bool dither_ok=neg0==0x3DEF && neg1==neg0 && pos0==0x4210 && pos1==pos0;
    // 129/255 rounds to 16 in the non-dither F3 path. The G3 winner is blue and traversed the dither
    // shader; its exact matrix-cell proof is covered by the shader readback additions below.
    bool pok=((lc>>10)&31)>20 && ((rc>>5)&31)>20 && (fc&31)==16 && postl>.31f && postl<.34f && postr>.67f;
    lucent::info("gpu_selftest","painter color+post-composite-D32 left={:04X}/{:.6f} far-winner right={:04X}/{:.6f} front-world; end-D32-cleared={:.6f}/{:.6f} => {}",lc,postl,rc,postr,ld,rd,pok?"PASS":"FAIL"); ok &= pok;
    lucent::info("gpu_selftest","painter mixed stream commands={} materials={}/{}/{}/{}/{} F3-dither-off={:04X} => {}",
      s_painter_cmd_n,s_painter_cmd_material[0],s_painter_cmd_material[1],s_painter_cmd_material[2],s_painter_cmd_material[3],s_painter_cmd_material[4],fc,
      (s_painter_cmd_n==5 && s_painter_cmd_material[0]==1 && s_painter_cmd_material[1]==0 && s_painter_cmd_material[2]==1 && s_painter_cmd_material[3]==0 && s_painter_cmd_material[4]==0 && (fc&31)==16)?"PASS":"FAIL");
    ok &= s_painter_cmd_n==5 && s_painter_cmd_material[0]==1 && s_painter_cmd_material[1]==0 && s_painter_cmd_material[2]==1 && s_painter_cmd_material[3]==0 && s_painter_cmd_material[4]==0;
    lucent::info("gpu_selftest","painter G3 dither scale2 neg={:04X}/{:04X} expect 3DEF pos={:04X}/{:04X} expect 4210 => {}",neg0,neg1,pos0,pos1,dither_ok?"PASS":"FAIL"); ok &= dither_ok;
    SDL_UnmapGPUTransferBuffer(s_dev,s_rb_xfer); SDL_UnmapGPUTransferBuffer(s_dev,ddl);
    SDL_UnmapGPUTransferBuffer(s_dev,s_painter_test_local_depth); SDL_UnmapGPUTransferBuffer(s_dev,s_painter_test_main_depth);
    SDL_UnmapGPUTransferBuffer(s_dev,s_painter_test_local_color);
    SDL_ReleaseGPUTransferBuffer(s_dev,ddl); SDL_ReleaseGPUTransferBuffer(s_dev,s_painter_test_local_depth); SDL_ReleaseGPUTransferBuffer(s_dev,s_painter_test_main_depth);
    SDL_ReleaseGPUTransferBuffer(s_dev,s_painter_test_local_color);
    s_painter_test_local_depth=nullptr; s_painter_test_main_depth=nullptr; s_painter_test_local_color=nullptr;
    SDL_GPUTexture* old=s_painter_color; ensure_painter_targets(64,64); bool resize1=s_painter_w==64&&s_painter_h==64; ensure_painter_targets(96,48);
    bool resize2=s_painter_w==96&&s_painter_h==48&&s_painter_color!=nullptr; (void)old; ok &= resize1&&resize2;
    lucent::info("gpu_selftest","painter resize 64x64 -> 96x48 => {}",resize1&&resize2?"PASS":"FAIL");
  }
  exit(ok ? 0 : 1);
}

// ---- shadow capture / dynamic-state hooks (3D-only; inert in Pass 1) --------------------------------
int  gpu_vk_shadows_active(void) { return 0; }
void gpu_vk_shadow_push_tri(Core* core, const float* v0, const float* v1, const float* v2) { (void)core; (void)v0; (void)v1; (void)v2; }
int  gpu_seen3d_this_frame(Core* core);   // defined in gpu_native.cpp
int  gpu_had3d_last_frame(Core* core);

// ---- dual-view / SBS target selection (single target in Pass 1) -------------------------------------
void gpu_vk_select_target(int t) { (void)t; }
int  gpu_vk_target_count(int t) { (void)t; return 0; }
void gpu_vk_rawdump_arm(const char* path, int frame) { (void)path; (void)frame; }

// SBS per-pane render: render ONE core's VRAM + geometry batch into s_vram_tex (NO swapchain present),
// then read the display region [sx,sy,w,h] back to host RGBA8 (`rgba` holds w*h*4). The SBS composites the
// two returned panes via gpu_vk_present_sbs2. Reuses the proven upload+geom+readback path; the engine
// screen-fade is applied (same math as dump_to / present.frag).
void gpu_vk_render_readback(Core* core, const uint16_t* vram, int sx, int sy, int w, int h, uint8_t* rgba) {
  FadeState f = fade_state_of(core);   // THIS core's fade (guest-backed, SBS-clean)
  const int s_fade_mode = f.mode;
  const uint8_t s_fade_r = f.r, s_fade_g = f.g, s_fade_b = f.b;
  if (!gpu_vk_enabled()) { memset(rgba, 0, (size_t)w * h * 4); return; }
  if (!s_inited) init_gpu(core ? core->game : nullptr);
  GpuVkState& g = core->game->gpu_vk;            // THIS core's own VRAM image + targets (no cross-core sharing)
  SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(s_dev); GPUCHK(cmd, "render_readback cmd");
  upload_vram(g, cmd, vram, kWholeVram, 1);   // SBS pane: this core's picture IS its CPU VRAM + batch
  int a, b, c; render_geom(g, cmd, vram, sx, sy, w, h, &a, &b, &c);
  gpu_submit(cmd, "gpu_vk_render_readback");                 // render into THIS core's VRAM image; NO swapchain present
  // readback_vram returns nullptr when the GPU is latched off. Refuse by NAME here: dereferencing it
  // would segfault, and pretending success would emit garbage as a measurement.
  const uint16_t* src = readback_vram(g);          // download it (RG8 bytes == uint16 1555 words)
  if (!src) { lucent::error("gpu_vk", "present: no VRAM image — GPU latched off; skipping this present "
                                      "rather than presenting stale or garbage pixels"); return; }
  if (cfg_on("PSXPORT_GPU_TRACE")) { long nz = 0; for (int yy=0; yy<h; yy++) for (int xx=0; xx<w; xx++) if (src[((sy+yy)%VRAM_H)*VRAM_W + ((sx+xx)&1023)]) nz++;
    RenderStats& st = core->rsub.stats;
    lucent::info("gpu_vk", "readback region sx={} sy={} {}x{} region-nonzero={}/{} fade={}({},{},{}) batch tri={} tex={} semi={} worldquads={}", sx, sy, w, h, nz, w*h, s_fade_mode, s_fade_r, s_fade_g, s_fade_b, a, b, c, st.dbgWorldQuads);
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
void gpu_vk_present_sbs2(Game* game, const uint8_t* rgbaA, int wA, int hA, const uint8_t* rgbaB, int wB, int hB) {
  if (!gpu_vk_enabled() || s_headless) return;
  if (!s_inited) init_gpu(game);
  if (wA < 1) wA = 1; if (hA < 1) hA = 1; if (wB < 1) wB = 1; if (hB < 1) hB = 1;
  sbs_make_tex(SBS_PANE_A, wA, hA); sbs_make_tex(SBS_PANE_B, wB, hB);
  const uint8_t* rgb[SBS_PANE_COUNT] = { rgbaA, rgbaB };
  int pw[SBS_PANE_COUNT] = { wA, wB }, ph[SBS_PANE_COUNT] = { hA, hB };

  SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(s_dev); GPUCHK(cmd, "sbs cmd");
  for (int i = 0; i < SBS_PANE_COUNT; i++) { void* p = SDL_MapGPUTransferBuffer(s_dev, s_sbs_xfer[i], true);
    memcpy(p, rgb[i], (size_t)pw[i] * ph[i] * 4); SDL_UnmapGPUTransferBuffer(s_dev, s_sbs_xfer[i]); }
  SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
  for (int i = 0; i < SBS_PANE_COUNT; i++) { SDL_GPUTextureTransferInfo si = {}; si.transfer_buffer = s_sbs_xfer[i]; si.pixels_per_row = (Uint32)pw[i]; si.rows_per_layer = (Uint32)ph[i];
    SDL_GPUTextureRegion dr = {}; dr.texture = s_sbs_tex[i]; dr.w = (Uint32)pw[i]; dr.h = (Uint32)ph[i]; dr.d = 1;
    SDL_UploadToGPUTexture(cp, &si, &dr, false); }
  SDL_EndGPUCopyPass(cp);

  SDL_GPUTexture* swaptex = NULL; Uint32 sw = 0, sh = 0;
  if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, s_win, &swaptex, &sw, &sh) || !swaptex) { gpu_submit(cmd, "gpu_vk_present_sbs2"); poll_quit(game); return; }
  SDL_GPUColorTargetInfo cti = {}; cti.texture = swaptex; cti.clear_color = (SDL_FColor){ 0, 0, 0, 1 };
  cti.load_op = SDL_GPU_LOADOP_CLEAR; cti.store_op = SDL_GPU_STOREOP_STORE;
  SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(cmd, &cti, 1, NULL);
  float fpc[4] = { 1.0f, 0, 0, 0 };   // no fade (already applied in the readback)
  SDL_PushGPUFragmentUniformData(cmd, 0, fpc, sizeof fpc);
  SDL_BindGPUGraphicsPipeline(rp, s_image_pipe);
  SDL_Rect sc = { 0, 0, (int)sw, (int)sh };
  for (int i = 0; i < SBS_PANE_COUNT; i++) {
    // A left, B right, each letterboxed inside its own half by its OWN aspect (A may be wide, B 4:3).
    SDL_GPUViewport vp = viewport_of(sbs_pane_rect(i, pw[i], ph[i], (int)sw, (int)sh));
    SDL_SetGPUViewport(rp, &vp); SDL_SetGPUScissor(rp, &sc);
    SDL_GPUTextureSamplerBinding tsb = { s_sbs_tex[i], s_samp_linear };
    SDL_BindGPUFragmentSamplers(rp, 0, &tsb, 1);
    SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
  }
  SDL_EndGPURenderPass(rp);
  gpu_submit(cmd, "gpu_vk_present_sbs2");
  poll_quit(game);
}

// ---- Public API: thin free-function wrappers over the per-instance GpuVkState methods ---------------
void gpu_vk_set_vd(Core* core, const float* d3) { core->game->gpu_vk.set_vd(d3); }
void gpu_vk_set_vd_n(Core* core, const float* d3) { core->game->gpu_vk.set_vd_n(d3); }
void gpu_vk_set_xyf(Core* core, const float* xf, const float* yf) { core->game->gpu_vk.set_xyf(xf, yf); }
void gpu_vk_set_order_override(Core* core,uint32_t seq){ core->game->gpu_vk.s_order_override=seq; }
void gpu_vk_set_painter_material(Core* core,int gouraud,int dither){core->game->gpu_vk.s_painter_item_gouraud=gouraud;core->game->gpu_vk.s_painter_item_dither=dither;}
void gpu_vk_set_order(Core* core, unsigned idx) { core->game->gpu_vk.set_order(idx); }
void gpu_vk_set_order_2d(Core* core, unsigned idx) { core->game->gpu_vk.set_order_2d(idx); }
void gpu_vk_set_order_2d_n(Core* core, unsigned idx) { core->game->gpu_vk.set_order_2d_n(idx); }
void gpu_vk_set_order_2d_bg(Core* core, unsigned idx) { core->game->gpu_vk.set_order_2d_bg(idx); }
void gpu_vk_set_order_2d_bg_n(Core* core, unsigned idx) { core->game->gpu_vk.set_order_2d_bg_n(idx); }
void gpu_vk_semi_group(Core* core, int x0, int y0, int x1, int y1) { core->game->gpu_vk.semi_group(x0, y0, x1, y1); }
void gpu_vk_stats(Core* core, int* tri, int* tex, int* semi) { core->game->gpu_vk.stats(tri, tex, semi); }
void gpu_vk_dirty(Core* core, int x, int y, int w, int h) { core->game->gpu_vk.dirty(x, y, w, h); }
void gpu_vk_present(Core* core, const uint16_t* src, int sx, int sy, int w, int h) {
  overlay_glue_frame_begin(core);
  core->game->gpu_vk.present(src, sx, sy, w, h);
  // `debug fadewatch` state-byte tap (2026-07-01, "garbage during fade" investigation): whenever the fade
  // print fires, also dump the two overlapping fade drivers' guest state — bg_scene_transition_sm's struct
  // P=0x80100400 (P+4=state,P+8,P+0xA=ramp counters,P+3=dir) and ov_sop_field_mode's sm+0x50/0x6c (outer
  // field-mode state / its own ramp counter) — to see which driver (if either) is live during the glitch
  // frame where the fade unexpectedly reads back to mode=0 mid-ramp.
  static const lucent::Channel fadewatch_state_ch{"fadewatch"};
  if (fadewatch_state_ch) {
    GpuDevice& gd = gdev();
    int& lm = gd.s_fws_lastmode; uint8_t& lr = gd.s_fws_lr; uint8_t& lg = gd.s_fws_lg; uint8_t& lb = gd.s_fws_lb;
    int& lsx = gd.s_fws_lsx; int& lsy = gd.s_fws_lsy; int& lw = gd.s_fws_lw; int& lh = gd.s_fws_lh;
    FadeState f = fade_state_of(core);
    int m = f.mode; uint8_t r = f.r, g = f.g, b = f.b;
    if (m != lm || r != lr || g != lg || b != lb || sx != lsx || sy != lsy || w != lw || h != lh) {
      uint32_t sm = core->mem_r32(0x1f800138u);
      lucent::debug("fadewatch", "[fadewatch-state] P.st={} P8={} P0xA={} P3={} | sm50={} sm6c={}",
              core->mem_r8(0x80100400u + 4), core->mem_r16(0x80100400u + 8), core->mem_r16(0x80100400u + 0xa),
              core->mem_r8(0x80100400u + 3), core->mem_r16(sm + 0x50), core->mem_r8(sm + 0x6c));
      lm=m; lr=r; lg=g; lb=b; lsx=sx; lsy=sy; lw=w; lh=h;
    }
  }
}
void gpu_vk_repaint(Core* core) {
  overlay_glue_frame_begin(core);   // the RmlUi overlay stays interactive while the game is frozen
  core->game->gpu_vk.repaint();
}
void gpu_vk_draw_tri(Core* core, int x0,int y0,int r0,int g0,int b0, int x1,int y1,int r1,int g1,int b1, int x2,int y2,int r2,int g2,int b2, int dax0,int day0,int dax1,int day1) { core->game->gpu_vk.draw_tri(x0,y0,r0,g0,b0,x1,y1,r1,g1,b1,x2,y2,r2,g2,b2,dax0,day0,dax1,day1); }
void gpu_vk_draw_tritri(Core* core, const int* xs, const int* ys, const int* us, const int* vs, const unsigned char* rs, const unsigned char* gs, const unsigned char* bs, int tpx, int tpy, int mode, int raw, int clutx, int cluty, int twmx, int twmy, int twox, int twoy, int dax0, int day0, int dax1, int day1) { core->game->gpu_vk.draw_tritri(xs,ys,us,vs,rs,gs,bs,tpx,tpy,mode,raw,clutx,cluty,twmx,twmy,twox,twoy,dax0,day0,dax1,day1); }
void gpu_vk_draw_semi(Core* core, const int* xs, const int* ys, const int* us, const int* vs, const unsigned char* rs, const unsigned char* gs, const unsigned char* bs, int tpx, int tpy, int mode, int raw, int clutx, int cluty, int twmx, int twmy, int twox, int twoy, int dax0, int day0, int dax1, int day1, int blend) { core->game->gpu_vk.draw_semi(xs,ys,us,vs,rs,gs,bs,tpx,tpy,mode,raw,clutx,cluty,twmx,twmy,twox,twoy,dax0,day0,dax1,day1,blend); }
void gpu_vk_shot(Core* core, const char* path) { core->game->gpu_vk.shot(path); }
void gpu_vk_present_shot(Core* core, const char* path) { core->game->gpu_vk.present_shot(path); }
void gpu_vk_shot_b(Core* core, const char* path) { core->game->gpu_vk.shot_b(path); }
void gpu_vk_frame_end(Core* core, const uint16_t* svram, int frame) { core->game->gpu_vk.frame_end(svram, frame); }
bool gpu_vk_painter_begin(Core* core, uint32_t object) { return core->game->gpu_vk.painter_begin(object); }
bool gpu_vk_painter_end(Core* core) { return core->game->gpu_vk.painter_end(); }

// REPL `preseq` arm (repl.cpp) — creates the dir and arms the per-present dump in present().
void gpu_vk_preseq_arm(Core* core, int n, const char* dir) {
  GpuVkState& s = core->game->gpu_vk;
  snprintf(s.s_preseq_dir, sizeof s.s_preseq_dir, "%s", dir);
  char mk[200]; snprintf(mk, sizeof mk, "mkdir -p %s", dir); if (system(mk)) {}
  s.s_preseq_idx = 0; s.s_preseq_left = n;
}
// Present index (0-based) that THIS emit pass will dump to `p<idx>.ppm` at frame_end, or -1 when no
// preseq capture is armed. The emit passes (RenderQueue::emitItem) consult this for the `preseqobj`
// per-object motion log so each logged line is keyed to the exact present frame it belongs to.
int gpu_vk_preseq_present_index(Core* core) {
  GpuVkState& s = core->game->gpu_vk;
  return s.s_preseq_left > 0 ? s.s_preseq_idx : -1;
}
