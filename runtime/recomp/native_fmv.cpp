#include "core.h"
#include "game.h"   // class Fmv lives on Game (game->fmv); this TU implements its methods
#include "c_subsys.h"
#include "gpu_vk.h"  // gpu_vk_present_image — present the decoded frame as a NATIVE RGBA image
#include <vector>
// Native FMV player for the Tomba!2 PC port.
//
// Plays PSX .STR movies (MOVIE/LOGO.STR, MOVIE/OP.STR) entirely with our own code,
// bypassing the game's PSX StrPlayer / CD-streaming machinery. Pipeline:
//
//   disc (libchdr, disc.c)  ->  STR demux (this file)
//     ->  BS (MDEC bitstream) VLC decode (fmv_decode.cpp)
//       ->  MDEC IDCT/YCbCr (Beetle mdec.c via mdec_beetle.c, driven by fmv_decode.cpp)
//         ->  present (gpu_vk_present_image)
//
// THE DECODE IS SHARED: bs_decode_frame / mdec_decode_to_rgb555 / xa_decode_sector live in
// fmv_decode.cpp and are the SAME functions the offline exporters (tools/fmv_export,
// tools/fmv_compare) link — a bug found in a tool dump is a bug in the runtime by
// construction, and the STR/BS/MDEC pipeline walkthrough lives there.
//
// STR data-sector sub-header (32 bytes, little-endian), verified vs LOGO.STR LBA 11491:
//   [0..1]   0x0160   magic ("STR data sector")
//   [2..3]   0x8001   sub-mode marker
//   [4..5]   chunk index within this frame (0..nchunks-1)
//   [6..7]   number of chunks (sectors) making up this frame
//   [8..11]  frame number (1-based)
//   [12..15] frame BS payload size in bytes
//   [16..17] frame width in pixels   (320)
//   [18..19] frame height in pixels  (240)
//   [20..31] misc (demux id, etc.)
//   [32..]   this chunk's BS payload bytes. The 8-byte BS frame header sits at the very
//            start of chunk 0's payload.
//
// BS frame header (first 8 bytes of the concatenated payload, little-endian):
//   [0..1] number of MDEC code words in the decoded stream (informational)
//   [2..3] 0x3800 magic
//   [4..5] qscale (quantization scale, applied as the DC/AC QScale)
//   [6..7] BS version (2 here)
//
// The boot/front-end sequencers call game->fmv.play().
#include <stdint.h>
#include "cfg.h"
#include "config_vars.h"
#include "audio_policy.h"   // audio_may_open — headless implies no audio device
#include "c_subsys.h"       // gpu_windowed
#include <lucent/log.h>
#include "fmv_decode.h"   // the pure decode machinery (shared with tools/fmv_export + fmv_compare)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- runtime pieces we call (declared here to avoid header churn; do NOT modify them) ----

#include "sbs.h"                        // class Sbs — PSXPORT_SBS harness active: skip the (blocking) intro FMVs

void gpu_gp1(Core*, uint32_t w);

// pad access via c->game->pad — class Pad on Game (see game.h): pollSdl(), buttons field
#define PAD_START 0x0008u                                   // Start button bit (active-low)

static int fmv_resolve_path(DiscState* disc, const char* path, uint32_t* out_lba, uint32_t* out_size);


#define SECTOR_USER   2048u
#define SUBHDR_LEN    32u

// decode-scratch sizes (heap members on Fmv, allocated on first play)
#define FMV_PAYLOAD_BYTES (512u * 1024u)   // concatenated BS payload
#define FMV_CODES_MAX     (512u * 1024u)   // MDEC run-level codes

// ====================================================================================
// BS (bitstream) VLC decode -> MDEC code stream, and MDEC IDCT/YCbCr -> RGB555.
// THE DECODE IS THE SHARED PURE MACHINERY IN fmv_decode.cpp — the same functions the
// offline exporters (tools/fmv_export, tools/fmv_compare) link. These methods are thin
// pass-throughs so a bug found in a tool dump is a bug in the runtime by construction.
// The machinery (VLC table, bit reader, quant/IDCT upload, XA-ADPCM, MDEC feed/tile) lives
// there and only there; see fmv_decode.cpp for the STR/BS/MDEC pipeline walkthrough.
// ====================================================================================
int Fmv::bsDecodeFrame(const uint8_t* payload, uint32_t payload_size,
                       int width, int height, uint16_t* codes, int max_codes) {
  return bs_decode_frame(payload, payload_size, width, height, codes, max_codes);
}

// ====================================================================================
// MDEC feed (16bpp) + RGB555 extraction — in fmv_decode.cpp (mdec_decode_to_rgb555),
// which uploads the quant/IDCT tables, feeds the MDEC in DMA0/DMA1 ping-pong, drains the
// frame, and tiles the 16x16 macroblocks column-major. See above for the why.
// ====================================================================================
int Fmv::mdecDecodeToRgb555(const uint16_t* codes, int ncodes,
                            int width, int height, uint16_t* pixels) {
  return mdec_decode_to_rgb555(codes, ncodes, width, height, pixels);
}

// Present the decoded movie frame as a NATIVE RGBA image, letterboxed 4:3 with black bars
// (gpu_vk_present_image) — NOT a VRAM upload. The PC renderer composites only native submits over
// black; a CPU->VRAM upload + gpu_present would be blacked out by that. Presenting the frame directly
// is also the centering fix — it pillarboxes 4:3 on widescreen instead of left-aligning in the wide FB.
static void present_rgb555(Core* core, const uint16_t* pixels, int width, int height) {
  static std::vector<uint8_t> rgba;
  const int npix = width * height;
  rgba.resize((size_t)npix * 4);
  for (int i = 0; i < npix; i++) {
    const uint16_t p = pixels[i];                                  // PSX 555: bit0-4 R, 5-9 G, 10-14 B
    const uint8_t r5 = p & 0x1f, g5 = (p >> 5) & 0x1f, b5 = (p >> 10) & 0x1f;
    rgba[i * 4 + 0] = (uint8_t)((r5 << 3) | (r5 >> 2));            // 5 -> 8 bit expand
    rgba[i * 4 + 1] = (uint8_t)((g5 << 3) | (g5 >> 2));
    rgba[i * 4 + 2] = (uint8_t)((b5 << 3) | (b5 >> 2));
    rgba[i * 4 + 3] = 255;
  }
  gpu_vk_present_image(core, rgba.data(), width, height, 1.0f);
}

// CD-XA ADPCM audio decode lives in fmv_decode.cpp (xa_decode_sector) — the shared machinery
// this TU and the offline tools both call. It is declared via c_subsys.h / fmv_decode.h.

// ---- FMV audio output (dedicated SDL device at the XA rate) + audio-master pacing --------
#ifdef PSXPORT_SDL
#include <SDL3/SDL.h>
// SDL3 push-model audio stream bound to the default playback device, opened at the movie's XA rate.
void Fmv::audioOpen(int freq) {
  SDL_AudioStream* st = (SDL_AudioStream*)stream;
  // Headless implies NO AUDIO DEVICE — the same rule spu_audio.cpp applies, via the SAME predicate.
  // This line used to test only the knob, so a headless gate still played movie sound (USER,
  // 2026-08-06: "a tomba gate plays audible fmv"). See audio_policy.h for why it is shared.
  if (!audio_may_open(psx::config::cv_noaudio.get(), gpu_windowed() != 0)) return;
  if (st && stream_freq == freq) { SDL_ClearAudioStream(st); return; }
  if (st) { SDL_DestroyAudioStream(st); stream = st = 0; }
  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) return;
  SDL_AudioSpec spec; SDL_memset(&spec, 0, sizeof spec);
  spec.freq = freq; spec.format = SDL_AUDIO_S16; spec.channels = 2;
  st = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
  stream = st;
  if (st) { stream_freq = freq; SDL_ResumeAudioStreamDevice(st); }
}
void Fmv::audioQueue(const int16_t* pcm, int frames) {
  if (stream) SDL_PutAudioStreamData((SDL_AudioStream*)stream, pcm, frames * 4);   // S16 stereo
}
void Fmv::audioClose() { if (stream) { SDL_ClearAudioStream((SDL_AudioStream*)stream); } }

// Pace playback to the AUDIO/media clock: media_frames audio sample-pairs at `freq` Hz define
// the elapsed media time; sleep until wall-clock catches up. This is the real PSX rate (the
// fixed-15fps guess was too slow). Polls input and returns 1 if Start was pressed (skip).
// uncapped (PSXPORT_FMV_FPS=0) disables pacing for fast headless dumps.
int Fmv::pace(long media_frames, int freq, uint32_t t0, int uncapped) {
  game->pad.pollSdl();
  int pressed = ((game->pad.buttons & PAD_START) == 0) && !start_prev;
  start_prev = (game->pad.buttons & PAD_START) == 0;
  if (uncapped || freq <= 0) return pressed;
  uint32_t target = (uint32_t)((long long)media_frames * 1000 / freq);
  while ((int)(SDL_GetTicks() - t0) < (int)target) {
    SDL_Delay(2);
    game->pad.pollSdl();
    if (((game->pad.buttons & PAD_START) == 0) && !start_prev) pressed = 1;
    start_prev = (game->pad.buttons & PAD_START) == 0;
  }
  return pressed;
}
#else
void Fmv::audioOpen(int freq) { (void)freq; }
void Fmv::audioQueue(const int16_t* p, int n) { (void)p; (void)n; }
void Fmv::audioClose() {}
int Fmv::pace(long m, int f, uint32_t t, int u) { (void)m;(void)f;(void)t;(void)u; return 0; }
#endif

Fmv::~Fmv() {
  free(payload_buf); free(codes_buf); free(pixels_buf);
  free(xa_pcm);
}

// ====================================================================================
// STR demux + top-level play
// ====================================================================================
int Fmv::playLba(uint32_t lba, uint32_t size_bytes) {
  Core* core = &game->core;
  // PSXPORT_SBS: the side-by-side debugger compares the FIELD (gameplay + render); the intro movies are
  // identical pre-field content, and this player is a BLOCKING decode loop whose Start-skip reads the raw
  // host pad (pad_poll_sdl), which the harness's auto-skip (repl-injected Start) can't drive — so leaving
  // it in would freeze both panes in the FMV. Skip it entirely (like a headless run does at the call site),
  // so the concurrent-lockstep nav reaches free-roam. Both cores skip identically, so they stay in step.
  if (game->sbs) return 0;   // SBS: skip FMV (see comment above)
  game->gpu.gpu_native_init();
  mdec_init();

  uint32_t nsectors = (size_bytes + SECTOR_USER - 1) / SECTOR_USER;
  if (!payload_buf) {
    payload_buf = (uint8_t*)malloc(FMV_PAYLOAD_BYTES);
    codes_buf   = (uint16_t*)malloc(FMV_CODES_MAX * 2);
    pixels_buf  = (uint16_t*)malloc(1024 * 512 * 2);
    xa_pcm      = (int16_t*)malloc(4032 * 2 * 2);   // mono sectors yield up to 4032 frames (see xa_decode_sector)
  }
  uint8_t*  payload = payload_buf;
  uint16_t* codes   = codes_buf;
  uint16_t* pixels  = pixels_buf;

  int frames = 0;
  uint32_t sec = 0;
  int cur_frame = -1;
  uint32_t paylen = 0;
  int fwidth = 320, fheight = 240;
  int expected_chunks = 0, got_chunks = 0;

  // Optional dev cap: PSXPORT_FMV_MAXFRAMES bounds how many frames to play (0/unset = all).
  // Used by the standalone proof to decode just the first frame quickly; harmless in prod.
  int max_frames = 0;
  { const char* mf = cfg_str("PSXPORT_FMV_MAXFRAMES"); if (mf && *mf) max_frames = atoi(mf); }

  // Audio: STR interleaves XA-ADPCM sectors with the video sectors. Decode them, play through
  // a dedicated SDL device at the XA rate, and pace VIDEO to the audio/media clock (the real
  // PSX rate). uncapped = PSXPORT_FMV_FPS=0 (headless dumps: no pacing, no audio device).
  // FMV pacing is asked for explicitly, never inferred from the render sink.
  //
  // This used to auto-uncap on PSXPORT_VK_HEADLESS. USER RULE: "Headless and windowed should never
  // be different code paths" — and pacing is not a sink concern, it is what the movie DOES. A
  // headless run that silently fast-forwards is measuring a different program from the one the user
  // watches, which is how a black intro was measured green all day while the user still saw black.
  // It also makes every headless timing/sync number about the movies meaningless by construction,
  // including the audio/video sync question that is still open on this port.
  //
  // The wall-clock saving is real and is still available — ask for it: PSXPORT_FMV_FPS=0. A probe
  // that wants to fast-forward says so, and its log then records that it did.
  int uncapped = 0; { const char* f = cfg_str("PSXPORT_FMV_FPS"); if (f && *f) uncapped = (atoi(f) == 0); }
  int xa_freq = 37800;
  int16_t xa_hist[2][2] = {{0,0},{0,0}};
  long media_frames = 0;                       // cumulative audio sample-pairs = media clock
  start_prev = 1;                              // assume Start may be held from a prior movie
  uint32_t t0 = 0;
#ifdef PSXPORT_SDL
  t0 = SDL_GetTicks();
#endif
  int skipped = 0;
  uint8_t raw[2352];
  while (sec < nsectors) {
    if (!disc_read_raw(&game->disc, lba + sec, raw, 2352)) break;
    sec++;
    int submode = raw[18];

    if (submode & 0x04) {                       // XA-ADPCM audio sector
      int n = xa_decode_sector(raw, xa_pcm, xa_hist, &xa_freq);
      if (sec == 1 || media_frames == 0) audioOpen(xa_freq);
      audioQueue(xa_pcm, n);
      media_frames += n;
      if (pace(media_frames, xa_freq, t0, uncapped)) { skipped = 1; break; }
      continue;
    }

    const uint8_t* sbuf = raw + 24;             // Form1 video user data
    uint16_t magic = (uint16_t)(sbuf[0] | (sbuf[1] << 8));
    if (magic != 0x0160) continue;              // not a video data sector (padding)

    int chunk_idx = sbuf[4]  | (sbuf[5]  << 8);
    int nchunks   = sbuf[6]  | (sbuf[7]  << 8);
    int framenum  = sbuf[8]  | (sbuf[9]  << 8) | (sbuf[10] << 16) | (sbuf[11] << 24);
    int w         = sbuf[16] | (sbuf[17] << 8);
    int h         = sbuf[18] | (sbuf[19] << 8);

    if (chunk_idx == 0) {
      cur_frame = framenum; paylen = 0;
      expected_chunks = nchunks; got_chunks = 0;
      fwidth = w ? w : 320; fheight = h ? h : 240;
    }
    if (cur_frame != framenum) continue;        // out of sync; wait for next chunk-0

    uint32_t plen = SECTOR_USER - SUBHDR_LEN;
    if (paylen + plen <= FMV_PAYLOAD_BYTES) {
      memcpy(payload + paylen, sbuf + SUBHDR_LEN, plen);
      paylen += plen;
    }
    got_chunks++;

    if (expected_chunks > 0 && got_chunks >= expected_chunks) {
      int ncodes = bsDecodeFrame(payload, paylen, fwidth, fheight, codes,
                                 (int)FMV_CODES_MAX);
      lucent::debug("fmv", "frame {}: {}x{}, {} payload bytes, {} codes",
                    framenum, fwidth, fheight, paylen, ncodes);
      if (ncodes > 0) {
        int np = mdecDecodeToRgb555(codes, ncodes, fwidth, fheight, pixels);
        if (np > 0) {
          present_rgb555(core, pixels, fwidth, fheight); frames++;
          // Pace video to the audio/media clock (no audio sector here, so just gate on it).
          if (pace(media_frames, xa_freq, t0, uncapped)) { skipped = 1; break; }
        }
      }
      cur_frame = -1; expected_chunks = 0; got_chunks = 0; paylen = 0;
      if (max_frames && frames >= max_frames) break;
    }
  }
  if (skipped) {
    lucent::info("fmv", "skipped by Start at frame {}", frames);
    // CONSUME the skip press: the title front-end polls the pad the instant this returns, so if Start is
    // still held it reads as a fresh menu press and auto-selects New Game. Wait for Start to be RELEASED
    // (bounded, ~1s safety cap) before handing back — the game's own StrPlayer consumed it the same way.
    for (int guard = 0; guard < 250 && (game->pad.buttons & PAD_START) == 0; guard++) {
      game->pad.pollSdl(); SDL_Delay(4);
    }
    start_prev = 0;
  }
  lucent::debug("fmv", "done: {} video frames, {} audio sample-pairs ({:.2f}s @ {}Hz)",
                frames, media_frames, media_frames / (double)(xa_freq ? xa_freq : 37800), xa_freq);
  audioClose();
  // FMV teardown (issues #7/#11): EVERY exit (normal end AND Start-skip break) leaves the FMV's last
  // (possibly partial) frame in the display FB. Black it + present once so no FMV residue is revealed
  // under the front-end's still-loading 2D layer. Engine-owned deterministic hand-off, no sleep/retry.
  void gpu_clear_display(Core*);
  gpu_clear_display(core);
  return frames;
}

int Fmv::play(const char* path) {
  uint32_t lba = 0, size = 0;
  if (!fmv_resolve_path(&game->disc, path, &lba, &size)) {
    lucent::info("fmv", "could not resolve {} on disc", path ? path : "(null)");
    return -1;
  }
  lucent::info("fmv", "{} -> LBA {}, {} bytes", path ? path : "(null)", lba, size);
  return playLba(lba, size);
}

// ---- ISO9660 path resolution (walks directories via disc_read_sector) ----------------
static uint32_t le32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void iso_name(const uint8_t* rec, int nlen, char* out, int outsz) {
  int j = 0;
  for (int i = 0; i < nlen && j < outsz - 1; i++) {
    char c = (char)rec[i];
    if (c == ';') break;
    if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
    out[j++] = c;
  }
  out[j] = 0;
}
static int iso_find_child(DiscState* disc, uint32_t dir_lba, uint32_t dir_size, const char* name,
                          int want_dir, uint32_t* clba, uint32_t* csize) {
  char upper[256]; int n = 0;
  for (const char* p = name; *p && n < 255; p++) {
    char c = *p; if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
    upper[n++] = c;
  }
  upper[n] = 0;

  uint32_t nsec = (dir_size + SECTOR_USER - 1) / SECTOR_USER;
  uint8_t sbuf[SECTOR_USER];
  for (uint32_t s = 0; s < nsec; s++) {
    if (!disc_read_sector(disc, dir_lba + s, sbuf)) return 0;
    uint32_t pos = 0;
    while (pos < SECTOR_USER) {
      uint8_t len = sbuf[pos];
      if (len == 0) break;
      if (pos + len > SECTOR_USER) break;
      uint8_t flags = sbuf[pos + 25];
      uint8_t nlen  = sbuf[pos + 32];
      uint32_t e_lba = le32(&sbuf[pos + 2]);
      uint32_t e_size = le32(&sbuf[pos + 10]);
      if (!(nlen == 1 && (sbuf[pos + 33] == 0 || sbuf[pos + 33] == 1))) {
        char nm[256]; iso_name(&sbuf[pos + 33], nlen, nm, sizeof nm);
        int is_dir = (flags & 0x02) ? 1 : 0;
        if (is_dir == want_dir && strcmp(nm, upper) == 0) {
          *clba = e_lba; *csize = e_size; return 1;
        }
      }
      pos += len;
    }
  }
  return 0;
}
static int fmv_resolve_path(DiscState* disc, const char* path, uint32_t* out_lba, uint32_t* out_size) {
  uint8_t pvd[SECTOR_USER];
  if (!disc_read_sector(disc, 16, pvd)) return 0;
  if (memcmp(pvd + 1, "CD001", 5) != 0) return 0;
  uint32_t dir_lba  = le32(pvd + 156 + 2);
  uint32_t dir_size = le32(pvd + 156 + 10);

  char comp[256]; const char* p = path;
  while (*p) {
    int n = 0;
    while (*p && *p != '/' && *p != '\\' && n < 255) comp[n++] = *p++;
    comp[n] = 0;
    while (*p == '/' || *p == '\\') p++;
    int last = (*p == 0);
    uint32_t clba = 0, csize = 0;
    if (!iso_find_child(disc, dir_lba, dir_size, comp, last ? 0 : 1, &clba, &csize)) return 0;
    if (last) { *out_lba = clba; *out_size = csize; return 1; }
    dir_lba = clba; dir_size = csize;
  }
  return 0;
}
