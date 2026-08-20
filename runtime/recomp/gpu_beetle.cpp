// gpu_beetle.cpp — THE REAL PSX GPU, from the vendored Beetle GPL-2 fork, as an INDEPENDENT ORACLE.
//
// WHY THIS EXISTS. USER 2026-08-20: "Try to setup a true oracle ... Because there are other oracle
// issues too ... PsyCross or beetle". The premise is right and the reason is concrete: psx_render's
// rasterizer is OUR OWN code, so calling it "the oracle" is the architecture-level version of an
// instrument that cannot show the other answer — exactly what CLAUDE.md warns about for SBS core B
// ("both cores are OUR code, so a shared wrong assumption reads as SUCCESS"). Every "oracle bug" the
// user reports is therefore as likely to be ours as the game's, and at least one already was
// (kanban #106, the bogus strip below the cutscene bars).
//
// Meanwhile the actual, widely-validated PSX GPU was already vendored in this tree and simply not
// compiled, while beetle's gte.c / mdec.c / spu.c WERE — each behind a thin adapter exactly like this
// one (gte_beetle.cpp, mdec_beetle.c, spu_beetle.cpp). The GPU was the one subsystem where we kept
// our own version and then treated it as the reference.
//
// WHAT THIS IS NOT: not a renderer swap, and not a replacement for our rasterizer. It is a TEE. Every
// GP0/GP1 word the guest writes goes to both implementations, and beetle keeps its own VRAM. That is
// what makes it an oracle rather than a second opinion nobody can check: the two VRAMs can be diffed
// on the same frame, and the answer to "is this an oracle bug or ours" stops being an argument.
//
// SCOPE, deliberately: command stream + VRAM only. No IRQs, no timers, no DMA timing, no scanout —
// all stubbed inert below. We are asking one question ("given these GP0/GP1 words, what does real
// hardware put in VRAM?") and everything not needed to answer it stays switched off. The whole-machine
// beetle oracle is a separate, much larger plan (docs/plans/oracle-against-beetle.md), whose hard part
// is that we HLE the BIOS and beetle executes it. This has no BIOS problem at all.
//
// TRUST GATE (CLAUDE.md: an instrument is trusted only once it has shown the OTHER answer):
// `debug gpubeetle` reports words fed and non-black VRAM pixels for BOTH implementations, per frame,
// with denominators. A backend that silently rasterizes nothing would otherwise read as "no
// difference found", which is the single most likely way this lands broken and looks fine.
#include "cfg.h"
#include "config.h"
#include "core.h"
#include "game.h"
#include <lucent/log.h>
#include <stdint.h>
#include <string.h>

// ---- Beetle's GPU API (mednafen/psx/gpu.h), declared locally so this file pulls no vendor headers,
// ---- the same way gte_beetle.cpp declares the GTE API.
// The scanout half of the GPU writes into an EmulateSpecStruct (surface + per-line widths). We do
// not present through beetle — we read its VRAM — but GPU_Update walks the display list every
// scanline and dereferences GPU.espec unconditionally, so a null spec is an immediate segfault
// (measured: SIGSEGV at gpu.c:2145, GPU.espec->InterlaceOn). These two vendor headers give the real
// struct layouts rather than a hand-rolled guess at them.
#include "git.h"                // EmulateSpecStruct
#include "psxport_gpu_census.h" // beetle-side command census (the only place command boundaries exist)
#include "video/surface.h"      // MDFN_Surface, MDFN_Surface_New

extern "C" {
bool GPU_Init(bool pal_clock_and_tv, int sls, int sle, uint8_t upscale_shift);
void GPU_StartFrame(EmulateSpecStruct *espec_arg);
void GPU_Power(void);
void GPU_Write(const int32_t timestamp, uint32_t A, uint32_t V);
uint32_t GPU_Read(const int32_t timestamp, uint32_t A);
uint16_t *GPU_get_vram(void);
int32_t GPU_Update(const int32_t sys_timestamp);
void GPU_Destroy(void);
}

// mdec_beetle.c owns EventCycles and pins it to 0x7FFFFFFF ("no event horizon") for the MDEC pump.
// The GPU reads the SAME global for a different purpose and cannot live with that value — see
// pump_fifo() below.
extern "C" int32_t EventCycles;

// ---- Externs the vendored GPU references, at faithful-first inert values. SANCTIONED VENDOR INTEROP
// ---- (the same arrangement gte_beetle.cpp documents): these are read by extern from beetle's C, and
// ---- folding them into a struct would mean editing the fork for no behavioural gain. Every one is
// ---- either a knob we deliberately pin, or a hook we deliberately silence.
//
// `gMode`, `widescreen_hack` and `widescreen_hack_aspect_ratio_setting` are NOT defined here — they
// already exist in gte_beetle.cpp and are shared with the GTE. Defining them again would be a
// duplicate-symbol link error, which is the correct outcome for a knob with one owner.
extern "C" {
// Renderer/geometry knobs — pinned to plain, faithful, software-path behaviour.
uint8_t psx_gpu_upscale_shift = 0; // 1x internal resolution: we want the HARDWARE answer
uint8_t psx_gpu_upscale_shift_hw = 0;
// TWO BUGS LIVED IN THIS ONE LINE, both mine, and they are why the oracle's dither could not be
// trusted on 3D content (kanban #113):
//   * TYPE. beetle declares `extern enum dither_mode psx_gpu_dither_mode` (gpu_common.h) — an
//     unscoped enum, so 4 bytes. Defining it as uint8_t here gave the linker a 1-byte object that
//     beetle then read 4 bytes out of, taking three bytes of whatever followed in .data. That is
//     undefined behaviour, and the value it yielded was never checked.
//   * VALUE. The enum is DITHER_NATIVE = 0, DITHER_UPSCALED = 1, DITHER_OFF = 2. "1" was written
//     here meaning "on"; 1 is UPSCALED. It happens to behave identically at dither_upscale_shift 0,
//     which is exactly why it survived — a wrong constant that is harmless today and wrong the
//     moment upscaling is switched on.
// unsigned int, because every enumerator is non-negative and that is the underlying type GCC picks.
unsigned int psx_gpu_dither_mode = 0; // DITHER_NATIVE — dither exactly as the console does
bool psx_gpu_rasterize_both_fields = false;
uint8_t line_render_mode = 0;
bool is_monkey_hero = false; // a per-title compatibility hack in the fork; not us
int32_t psx_pgxp_2d_tol = -1;
uint32_t psx_gpu_overclock_shift = 0;
uint32_t psx_overclock_factor = 0;
// Display/scanout knobs — inert: we read VRAM directly and never present through beetle.
bool crop_overscan = false;
bool fast_pal = false;
bool content_is_pal = false;
bool currently_interlaced = false;
bool aspect_ratio_dirty = false;
bool interlace_setting_dirty = false;
unsigned aspect_ratio_setting = 0;
unsigned core_timing_fps_mode = 0;
unsigned image_height = 240;
unsigned startup_frame_count = 0;
void *video_cb = nullptr;
// EventCycles is NOT defined here — mdec_beetle.c owns it (0x7FFFFFFF, the "no event horizon"
// value), and it is shared machine plumbing rather than a GPU knob.
void *PSX_FIO = nullptr;

// Machine plumbing we deliberately do not model — see SCOPE above. Each is a no-op, not a guess:
// nothing downstream of them affects what lands in VRAM for a given command stream.
// IRQ_Assert is NOT defined here either — the SPU adapter already provides it (hw_bind.cpp routes
// the SPU's interrupt line through it). One owner per symbol; a second definition is a link error,
// which is the right outcome.
void TIMER_SetVBlank(bool) {}
void TIMER_SetHRetrace(bool) {}
void TIMER_AddDotClocks(uint32_t) {}
void TIMER_ClockHRetrace(void) {}
int32_t TIMER_Update(int32_t) {
  return 0x7FFFFFFF;
}
void PSX_RequestMLExit(void) {}
void PSX_SetEventNT(const int, const int32_t) {}
void FrontIO_GPULineHook(const int32_t, const int32_t, bool, uint32_t *, uint32_t, uint32_t, uint32_t) {}
int MDFN_GetSettingI(const char *) {
  return 0;
}
uint32_t ReadMem(uint32_t) {
  return 0;
}

// PGXP is OFF for the oracle: the whole point is the INTEGER-snapped hardware result, not a smoothed
// approximation of it. gMode (gte_beetle.cpp) is already 0, so these are never reached in anger.
void PGXP_WriteFIFO(uint32_t) {}
uint32_t PGXP_ReadFIFO(void) {
  return 0;
}
void PGXP_WriteCB(uint32_t, uint32_t) {}
int PGXP_GetVertex(uint32_t, const void *, void *, int, int) {
  return 0;
}
}

namespace {

bool s_inited = false;
bool s_selftest = false;
bool s_failed = false;
long s_words_gp0 = 0, s_words_gp1 = 0; // the DENOMINATOR: what we actually fed it
long s_xfer_words_fed = 0;             // CPU->VRAM pixel words inside 0xA0 transfers
// WHICH COMMANDS the oracle actually received, by top-level GP0 opcode. "The word stream arrives"
// and "the DRAWING commands arrive" are different claims, and only this can tell them apart: beetle
// visibly received the texture UPLOADS (its VRAM holds the sky texture) while drawing no polygons,
// which is a shape no total-word count can express.
// (An opcode histogram used to live here, built from the tee side. It was WRONG: this side sees
//  words, not command boundaries, so every parameter word was counted as an opcode. The real
//  census now comes from inside beetle — psxport_gpu_census.h.)
int32_t s_ts = 0; // our own monotonic clock for GPU_Update

// The scanout sink. Allocated once; its CONTENTS are never read by us — the oracle's answer is VRAM,
// not this surface. It exists so beetle's per-scanline display walk has somewhere legal to write.
// Sized to the widest PSX display mode (640) by the full VRAM height, which is more than any mode
// this game selects can ask for.
constexpr int kSinkW = 1024, kSinkH = 512;
EmulateSpecStruct s_espec = {};
MDFN_Surface *s_surface = nullptr;
int32_t s_linewidths[kSinkH] = {};

// DRAIN THE BLITTER FIFO AFTER EVERY WORD, and lend the GPU a sane event horizon while it runs.
//
// Two hardware-model details, both of which produce a SILENTLY BLACK oracle if ignored, and both of
// which were measured doing exactly that (7,145,349 GP0 words in, VRAM entirely black):
//
//  1. GP0 words do not draw. They queue into GPU_BlitterFIFO, which is 0x20 words deep and drains
//     ONLY inside GPU_Update. Feeding millions of words without ever calling Update means everything
//     past the first 32 is dropped on the floor.
//
//  2. GPU_Update clamps its drawing budget to `2*EventCycles`. mdec_beetle.c legitimately pins that
//     shared global to 0x7FFFFFFF for the MDEC's benefit, and 2*0x7FFFFFFF OVERFLOWS int32 to -2 —
//     so the GPU's DrawTimeAvail would be clamped NEGATIVE on every call and it would refuse to
//     rasterize anything, forever. We therefore lend it a sane horizon for the duration of the call
//     and hand the global straight back, rather than editing either owner's value.
//
// The timestamp advances by a fixed step per word. Its magnitude is not a timing claim — this oracle
// deliberately models no timing (see SCOPE) — it exists only to keep sys_clocks non-zero, which is
// what makes Update do its work at all.
void pump_fifo() {
  // "64 keeps draw time ample" was a guess, and the census measured it wrong: 117,804 starved
  // dispatches and 84,335 words dropped by a full FIFO over 1,120 frames. No clock constant fixes
  // that, because the draw-time model itself is the thing we do not want — see
  // psxport_gpu_grant_drawtime(). The timestamp still has to advance for GPU_Update to do any work.
  constexpr int32_t kClocksPerWord = 64;
  constexpr int32_t kSaneHorizon = 1 << 22;
  const int32_t saved = EventCycles;
  EventCycles = kSaneHorizon;
  s_ts += kClocksPerWord;
  psxport_gpu_grant_drawtime();
  GPU_Update(s_ts);
  EventCycles = saved;
}

// The tee is off unless asked for. It costs a branch per GP0 word when off, and a second full
// rasterization when on, so it is a diagnostic/oracle mode rather than something to ship enabled.
bool enabled() {
  return cfg_on("PSXPORT_GPU_BEETLE");
}

bool ensure_init() {
  if (s_failed) {
    return false;
  }
  if (s_inited) {
    return true;
  }
  // sls/sle = the visible scanline range; upscale_shift 0 = native 1x, which is the only setting
  // that can answer "what does the hardware put in VRAM".
  if (!GPU_Init(/*pal*/ false, /*sls*/ 0, /*sle*/ 239, /*upscale_shift*/ 0)) {
    // REFUSE, do not degrade. A silently absent oracle that reports "no difference" is worse than no
    // oracle at all — it manufactures agreement.
    lucent::error("gpubeetle",
                  "GPU_Init FAILED — the beetle GPU oracle is NOT running. Nothing below "
                  "this line compares anything; do not read a quiet channel as agreement.");
    s_failed = true;
    return false;
  }
  GPU_Power();
  // PSXPORT_GPU_BEETLE_SELFTEST=1 — THE POSITIVE CONTROL, shipped in the artifact rather than run once
  // by hand. It shifts every primitive beetle draws 1px right: a known, bounded, purely-rasterisation
  // change that leaves the command feed identical. (The first version disabled dithering instead and
  // was a BAD control — dither only applies when the game sets the texture-page dither bit, so "no
  // difference" would have been a legitimate outcome and the test could not fail honestly.)
  // A run with this on MUST report a non-zero pixel difference on any frame that drew a primitive. If
  // it reports 0.00% on such a frame, the comparison is not comparing
  // and every "no difference found" from this oracle is void. That is the only thing that separates a
  // working oracle from one that agrees because it rasterises nothing (which is exactly how this
  // landed the first three times).
  // PSXPORT_GPU_BEETLE_DITHER=0 — turn beetle's dithering off, as a DISCRIMINATOR rather than a
  // setting. When ours and beetle disagree in a 4x4 pattern, "one of us dithers and the other does
  // not" is the hypothesis, and reasoning cannot say WHICH: running beetle undithered can. If the
  // difference collapses, our side was the one not dithering; if it grows, ours was.
  if (cfg_int("PSXPORT_GPU_BEETLE_DITHER", 1) == 0) {
    psx_gpu_dither_mode = 2; // DITHER_OFF
    lucent::warn("gpubeetle",
                 "beetle's dithering is DISABLED (PSXPORT_GPU_BEETLE_DITHER=0). This is "
                 "a discriminator run, not a faithful one.");
  }
  s_selftest = cfg_int("PSXPORT_GPU_BEETLE_SELFTEST", 0) != 0;
  if (s_selftest) {
    psxport_gpu_selftest_bias = 1;
    lucent::warn("gpubeetle",
                 "SELFTEST MODE: every primitive beetle draws is shifted 1px right. This "
                 "run is a positive control, not a measurement — a NON-ZERO difference "
                 "on a frame that DREW something is the PASS.");
  }
  s_surface = MDFN_Surface_New(kSinkW, kSinkH, kSinkW);
  if (!s_surface) {
    lucent::error("gpubeetle",
                  "scanout surface allocation FAILED — refusing to run the oracle rather "
                  "than let GPU_Update dereference a null spec");
    s_failed = true;
    return false;
  }
  s_espec.surface = s_surface;
  s_espec.LineWidths = s_linewidths;
  s_espec.DisplayRect.x = 0;
  s_espec.DisplayRect.y = 0;
  s_espec.DisplayRect.w = kSinkW;
  s_espec.DisplayRect.h = kSinkH;
  s_espec.InterlaceOn = false;
  s_espec.InterlaceField = false;
  s_espec.skip = 0;
  GPU_StartFrame(&s_espec);
  s_inited = true;
  lucent::info("gpubeetle",
               "beetle GPU oracle ARMED (native 1x, dither {}, PGXP off) — every GP0/GP1 "
               "word is now teed to it alongside our own rasterizer",
               s_selftest ? "OFF (SELFTEST)" : "on");
  return true;
}

} // namespace

// ---- The tee. Called from GpuState::gpu_gp0 / gpu_gp1, which are the single funnels every guest
// ---- command word already passes through.
void gpu_beetle_gp0(uint32_t w, int is_xfer_data) {
  if (!enabled() || !ensure_init()) {
    return;
  }
  s_words_gp0++;
  if (is_xfer_data) {
    s_xfer_words_fed++;
  }
  psxport_gpu_grant_drawtime();    // GPU_Write drains — and can DROP — before pump_fifo runs
  GPU_Write(s_ts, 0x1F801810u, w); // A&4 == 0 selects GP0 ("Data")
  pump_fifo();
}

void gpu_beetle_gp1(uint32_t w) {
  if (!enabled() || !ensure_init()) {
    return;
  }
  s_words_gp1++;
  GPU_Write(s_ts, 0x1F801814u, w); // A&4 != 0 selects GP1 ("Control")
  pump_fifo();
}

// NATIVE VRAM UPLOADS MUST BE TEED TOO, or the oracle is comparing against a different VRAM.
//
// GpuState::gpu_native_load_image writes *vram(...) straight from guest RAM and never goes near
// gpu_gp0 — so beetle saw none of it. That is what the first comparison actually found: at frame 4
// our VRAM held 97,540 non-black pixels after only FOURTEEN GP0 words, which is arithmetically
// impossible through the command port and was the tell that a second writer existed.
//
// Replayed as the GP0 sequence the hardware would have received (0xA0, destination, extent, then the
// packed pixel stream) rather than poked into beetle's VRAM behind its back: the transfer is a real
// state machine in there (INCMD_FBWRITE), and driving it through the front door keeps beetle's idea
// of itself consistent — and keeps this a tee of the COMMAND STREAM, which is the thing being
// compared.
void gpu_beetle_load_image(int x, int y, int w, int h, const uint16_t *pixels) {
  if (!enabled() || !ensure_init() || !pixels || w <= 0 || h <= 0) {
    return;
  }
  gpu_beetle_gp0(0xA0000000u, 0); // CPU -> VRAM blit
  gpu_beetle_gp0(((uint32_t)(y & 0x1FF) << 16) | (uint32_t)(x & 0x3FF), 0);
  gpu_beetle_gp0(((uint32_t)h << 16) | (uint32_t)w, 0);
  const long n = (long)w * h;
  for (long i = 0; i < n; i += 2) {
    const uint32_t lo = pixels[i];
    const uint32_t hi = (i + 1 < n) ? pixels[i + 1] : 0u; // odd pixel count pads, as the port does
    gpu_beetle_gp0(lo | (hi << 16), 1);
  }
}

const uint16_t *gpu_beetle_vram() {
  if (!enabled() || !s_inited) {
    return nullptr;
  }
  return GPU_get_vram();
}

bool gpu_beetle_active() {
  return enabled() && s_inited;
}

// ---- THE TRUST GATE. Per-frame, both implementations, with denominators — so "the two agree" can be
// ---- told apart from "the oracle drew nothing", which look identical in a diff and mean opposite
// ---- things. Called from the frame boundary.
void gpu_beetle_frame_report(int frame, const uint16_t *ours, int vram_w, int vram_h, long our_prims) {
  // The DUMP is deliberately NOT behind the log channel. It was, briefly, and that produced the
  // classic stale-artefact trap: a run with the channel off wrote no file, the previous run's file
  // was still on disk, and it read as this run's output. A capture knob must answer for itself.
  const char *df = cfg_str("PSXPORT_GPU_BEETLE_DUMP");
  const bool want_dump = df && *df && frame == atoi(df) && gpu_beetle_active();
  if (!lucent::channel_on("gpubeetle") && !want_dump) {
    return;
  }
  if (!gpu_beetle_active()) {
    lucent::debug("gpubeetle",
                  "f{} oracle NOT active (PSXPORT_GPU_BEETLE off or init failed) — "
                  "no comparison was made this frame",
                  frame);
    return;
  }
  const uint16_t *theirs = gpu_beetle_vram();
  const long total = (long)vram_w * vram_h;
  long ours_nz = 0, theirs_nz = 0, differ = 0;
  for (long i = 0; i < total; i++) {
    const uint16_t a = ours ? (uint16_t)(ours[i] & 0x7FFF) : 0;
    const uint16_t b = theirs ? (uint16_t)(theirs[i] & 0x7FFF) : 0;
    if (a) {
      ours_nz++;
    }
    if (b) {
      theirs_nz++;
    }
    if (a != b) {
      differ++;
    }
  }
  lucent::info("gpubeetle",
               "f{} fed gp0={} gp1={} | VRAM non-black: ours {}/{} ({:.1f}%), beetle {}/{} ({:.1f}%) | "
               "differing px {}/{} ({:.2f}%)",
               frame,
               s_words_gp0,
               s_words_gp1,
               ours_nz,
               total,
               100.0 * ours_nz / (double)total,
               theirs_nz,
               total,
               100.0 * theirs_nz / (double)total,
               differ,
               total,
               100.0 * differ / (double)total);
  // PSXPORT_GPU_BEETLE_DUMP=<frame> — write the oracle's VRAM at one frame, so the answer can be
  // LOOKED AT rather than inferred from a pixel count. A count proves the backend is doing something;
  // only the image proves it is doing the RIGHT something, and those are different claims.
  {
    if (want_dump && theirs) {
      // BOTH VRAMs, same frame, same format — a single-sided dump cannot answer "who is right".
      for (int which = 0; which < 2; which++) {
        const uint16_t *src = which ? ours : theirs;
        if (!src) {
          continue;
        }
        char path[256];
        snprintf(path, sizeof path, "scratch/screenshots/%s_vram_f%d.ppm", which ? "ours" : "beetle", frame);
        FILE *f = fopen(path, "wb");
        if (f) {
          fprintf(f, "P6\n%d %d\n255\n", vram_w, vram_h);
          for (long i = 0; i < total; i++) {
            const uint16_t px = src[i];
            unsigned char rgb[3] = {(unsigned char)((px & 0x1F) << 3),
                                    (unsigned char)(((px >> 5) & 0x1F) << 3),
                                    (unsigned char)(((px >> 10) & 0x1F) << 3)};
            fwrite(rgb, 1, 3, f);
          }
          fclose(f);
          lucent::info("gpubeetle", "f{} VRAM dumped -> {}", frame, path);
        }
      }
    }
  }
  // ---- THE FEED CENSUS. This is what makes a VRAM difference mean anything.
  //
  // A pixel difference is evidence about RASTERIZATION only once the FEED is known to be complete.
  // Twice already, "beetle did not draw X" turned out to be "beetle was never asked to draw X" —
  // first the undrained FIFO, then gpu_native_load_image writing VRAM directly and bypassing GP0
  // entirely. Both looked exactly like a rasterizer disagreement.
  //
  // The numbers below come from INSIDE beetle (psxport_gpu_census.h), because they cannot be
  // computed out here: this side sees a flat word stream with no command boundaries, and the
  // per-opcode length table lives in gpu.c. An earlier attempt to histogram opcodes from the tee
  // side counted parameter words as opcodes and was discarded; this replaces it.
  //
  // Every loss channel is named, so "beetle drew nothing" always says WHY rather than printing a
  // zero and letting it read as agreement.
  {
    static unsigned long prev[PGC_N] = {0};
    unsigned long d[PGC_N];
    for (int i = 0; i < PGC_N; i++) {
      d[i] = psxport_gpu_census[i] - prev[i];
      prev[i] = psxport_gpu_census[i];
    }
    d[PGC_NOP_LAST] = psxport_gpu_census[PGC_NOP_LAST]; // a VALUE, not a counter — never difference it
    // PRIMITIVES, not dispatches: a quad's continuation packet (PGC_POLY_CONT) is beetle rasterising
    // the second triangle of a primitive it already counted, not a second primitive.
    const unsigned long drawn = d[PGC_POLY] + d[PGC_LINE] + d[PGC_SPRITE];
    const unsigned long queued = psxport_gpu_fifo_depth();
    lucent::info("gpubeetle",
                 "f{} FEED: words accepted {} dropped {} | cmds {} = poly {} + line {} + sprite {} + "
                 "xfer {} + fill {} + env {} + nop0 {} + unknown {} | cont: quad {} line {} | starved {} null-func {} "
                 "| fifo queued {}",
                 frame,
                 d[PGC_WORDS_ACCEPTED],
                 d[PGC_WORDS_DROPPED],
                 d[PGC_CMDS_DISPATCHED],
                 d[PGC_POLY],
                 d[PGC_LINE],
                 d[PGC_SPRITE],
                 d[PGC_XFER],
                 d[PGC_FILL],
                 d[PGC_STATE],
                 d[PGC_NOP0],
                 d[PGC_NOP],
                 d[PGC_POLY_CONT],
                 d[PGC_LINE_CONT],
                 d[PGC_STARVED],
                 d[PGC_NULL_FUNC],
                 queued);
    // The comparison the whole oracle rests on: primitives WE drew vs primitives beetle dispatched.
    // Equal counts mean a pixel difference is a rasterizer difference. Unequal counts mean the tee
    // is lossy and NOTHING about the pixels may be read as a verdict yet.
    if (drawn == (unsigned long)our_prims) {
      lucent::info("gpubeetle",
                   "f{} FEED COMPLETE: ours drew {} prim(s), beetle dispatched {} — "
                   "a pixel difference from here IS a rasterizer difference",
                   frame,
                   our_prims,
                   drawn);
    } else {
      lucent::warn("gpubeetle",
                   "f{} FEED INCOMPLETE: ours drew {} prim(s) but beetle dispatched {} "
                   "({:+}). The tee is lossy, so any VRAM difference this frame is NOT yet "
                   "evidence about either rasterizer.",
                   frame,
                   our_prims,
                   drawn,
                   (long)drawn - our_prims);
    }
    if (d[PGC_WORDS_DROPPED]) {
      lucent::warn("gpubeetle",
                   "f{} {} GP0 word(s) DISCARDED by a full FIFO — "
                   "silent data loss on the oracle side",
                   frame,
                   d[PGC_WORDS_DROPPED]);
    }
    if (d[PGC_NOP]) {
      lucent::warn("gpubeetle",
                   "f{} {} command(s) dispatched as an opcode beetle has no "
                   "handler for (last: 0x{:02X}) — a mangled tee, not an empty scene",
                   frame,
                   d[PGC_NOP],
                   psxport_gpu_census[PGC_NOP_LAST]);
    }
    if (d[PGC_NULL_FUNC]) {
      lucent::warn("gpubeetle",
                   "f{} {} command(s) accepted then NOT rasterized "
                   "(null specialisation for abr/TexMode)",
                   frame,
                   d[PGC_NULL_FUNC]);
    }
    if (queued) {
      lucent::warn("gpubeetle",
                   "f{} {} word(s) still queued at the frame boundary — "
                   "the FIFO did not drain",
                   frame,
                   queued);
    }
  }
  // The verdict is only meaningful on a frame that DREW something. Boot and upload-only frames draw
  // no primitives, so a 1px shift cannot change a pixel there and "FAIL" would be a lie about the
  // instrument rather than a finding — say so instead of scoring it.
  if (s_selftest) {
    if (our_prims <= 0) {
      lucent::debug("gpubeetle",
                    "f{} SELFTEST not applicable: 0 primitives drawn, so a drawing-offset "
                    "shift cannot change any pixel",
                    frame);
    } else {
      lucent::info("gpubeetle",
                   "f{} SELFTEST verdict: {} — {} prim(s) were drawn with a 1px offset "
                   "injected into beetle only.",
                   frame,
                   differ ? "PASS (a difference was seen)"
                          : "FAIL (0 differing pixels despite a KNOWN injected rasterizer difference)",
                   our_prims);
    }
  }
  if (theirs_nz == 0 && s_words_gp0 > 0) {
    lucent::warn("gpubeetle",
                 "f{} beetle's VRAM is ENTIRELY BLACK after {} GP0 word(s). That is the "
                 "backend not rasterizing, NOT the two implementations agreeing — treat "
                 "every comparison from this run as void until it is fixed.",
                 frame,
                 s_words_gp0);
  }
}
