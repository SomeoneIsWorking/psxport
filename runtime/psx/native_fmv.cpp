#include "c_subsys.h"
#include "core.h"
#include "game.h"   // class Fmv lives on Game (game->fmv); this TU implements its methods
#include "gpu_vk.h" // gpu_vk_present_image — present the decoded frame as a NATIVE RGBA image
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
#include "audio_policy.h" // audio_may_open — headless implies no audio device
#include "c_subsys.h"     // gpu_windowed
#include "cfg.h"
#include "config_vars.h"
#include "fmv_decode.h" // the pure decode machinery (shared with tools/fmv_export + fmv_compare)
#include <lucent/log.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- runtime pieces we call (declared here to avoid header churn; do NOT modify them) ----

void gpu_gp1(Core *, uint32_t w);

// pad access via c->game->pad — class Pad on Game (see game.h): pollSdl(), buttons field
#define PAD_START 0x0008u // Start button bit (active-low)

static int fmv_resolve_path(DiscState *disc, const char *path, uint32_t *out_lba, uint32_t *out_size);

#define SECTOR_USER 2048u
#define SUBHDR_LEN 32u

// decode-scratch sizes (heap members on Fmv, allocated on first play)
#define FMV_PAYLOAD_BYTES (512u * 1024u) // concatenated BS payload
#define FMV_CODES_MAX (512u * 1024u)     // MDEC run-level codes

// ====================================================================================
// BS (bitstream) VLC decode -> MDEC code stream, and MDEC IDCT/YCbCr -> RGB555.
// THE DECODE IS THE SHARED PURE MACHINERY IN fmv_decode.cpp — the same functions the
// offline exporters (tools/fmv_export, tools/fmv_compare) link. These methods are thin
// pass-throughs so a bug found in a tool dump is a bug in the runtime by construction.
// The machinery (VLC table, bit reader, quant/IDCT upload, XA-ADPCM, MDEC feed/tile) lives
// there and only there; see fmv_decode.cpp for the STR/BS/MDEC pipeline walkthrough.
// ====================================================================================
int Fmv::bsDecodeFrame(
    const uint8_t *payload, uint32_t payload_size, int width, int height, uint16_t *codes, int max_codes) {
  return bs_decode_frame(payload, payload_size, width, height, codes, max_codes);
}

// ====================================================================================
// MDEC feed + macroblock tiling — in fmv_decode.cpp (mdec_decode_to_rgb555 / _to_rgb888),
// which upload the quant/IDCT tables, feed the MDEC in DMA0/DMA1 ping-pong, drain the
// frame, and tile the 16x16 macroblocks column-major. See above for the why.
// ====================================================================================
// Present the decoded movie frame as a NATIVE RGBA image, letterboxed 4:3 with black bars
// (gpu_vk_present_image) — NOT a VRAM upload. The PC renderer composites only native submits over
// black; a CPU->VRAM upload + gpu_present would be blacked out by that. Presenting the frame directly
// is also the centering fix — it pillarboxes 4:3 on widescreen instead of left-aligning in the wide FB.
//
// The frame is 24bpp because that is what retail PSX movies were authored for: Toy Story 2's FMV
// overlay asks its player for depth 3 in ITS convention (compared against the literal 3 before its
// 24-bit upload path), and RGB555 would spend two bits per channel the movie actually encodes.
static void submit_rgba(Core *core, const std::vector<uint8_t> &rgba, int width, int height) {
  gpu_vk_present_image(core, rgba.data(), width, height, 1.0f);
  // Native movie frames bypass GpuState::gpu_present_ex(), the main-presenter readiness path. They
  // are forward progress, but a startup movie does not prove the main VRAM targets are initialized.
  // After the main presenter is ready this heartbeat automatically uses the steady timeout.
  watchdog_progress();
}

static void present_rgb888(Core *core, std::vector<uint8_t> &rgba, const uint8_t *pixels, int width, int height) {
  const int npix = width * height;
  rgba.resize((size_t)npix * 4);
  for (int i = 0; i < npix; i++) {
    rgba[i * 4 + 0] = pixels[i * 3 + 0]; // already full 8-bit R,G,B from the MDEC
    rgba[i * 4 + 1] = pixels[i * 3 + 1];
    rgba[i * 4 + 2] = pixels[i * 3 + 2];
    rgba[i * 4 + 3] = 255;
  }
  submit_rgba(core, rgba, width, height);
  // Native movie frames bypass GpuState::gpu_present_ex(), the main-presenter readiness path. They
  // are forward progress, but a startup movie does not prove the main VRAM targets are initialized.
  // After the main presenter is ready this heartbeat automatically uses the steady timeout.
  watchdog_progress();
}

void Fmv::presentFrame(const uint8_t *pixels, int width, int height) {
  present_rgb888(&game->core, rgba_scratch_, pixels, width, height);
  last_frame_width_ = width;
  last_frame_height_ = height;
}

void Fmv::replayLastFrame(Core &core) {
  if (!active_ || last_frame_width_ == 0) {
    return;
  }
  // rgba_scratch_ still holds the frame presented above: the decode buffers are separate, so nothing
  // since then has overwritten it.
  submit_rgba(&core, rgba_scratch_, last_frame_width_, last_frame_height_);
}
// CD-XA ADPCM audio decode lives in fmv_decode.cpp (xa_decode_sector) — the shared machinery
// this TU and the offline tools both call. It is declared via c_subsys.h / fmv_decode.h.

// ---- FMV audio output (dedicated SDL device at the XA rate) + audio-master pacing --------
#ifdef PSXPORT_SDL
#include <SDL3/SDL.h>
// SDL3 push-model audio stream bound to the default playback device, opened at the movie's XA rate.
void Fmv::audioOpen(int freq) {
  SDL_AudioStream *st = (SDL_AudioStream *)stream;
  // Headless implies NO AUDIO DEVICE — the same rule spu_audio.cpp applies, via the SAME predicate.
  // This line used to test only the knob, so a headless gate still played movie sound (USER,
  // 2026-08-06: "a tomba gate plays audible fmv"). See audio_policy.h for why it is shared.
  if (!audio_may_open(psx::config::cv_noaudio.get(), gpu_windowed() != 0)) {
    return;
  }
  if (st && stream_freq == freq) {
    SDL_ClearAudioStream(st);
    return;
  }
  if (st) {
    SDL_DestroyAudioStream(st);
    stream = st = 0;
  }
  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    return;
  }
  SDL_AudioSpec spec;
  SDL_memset(&spec, 0, sizeof spec);
  spec.freq = freq;
  spec.format = SDL_AUDIO_S16;
  spec.channels = 2;
  st = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
  stream = st;
  if (st) {
    stream_freq = freq;
    SDL_ResumeAudioStreamDevice(st);
  }
}
void Fmv::audioQueue(const int16_t *pcm, int frames) {
  if (stream) {
    SDL_PutAudioStreamData((SDL_AudioStream *)stream, pcm, frames * 4); // S16 stereo
  }
}
void Fmv::audioClose() {
  if (stream) {
    SDL_ClearAudioStream((SDL_AudioStream *)stream);
  }
}

// Pace playback to the AUDIO/media clock: media_frames audio sample-pairs at `freq` Hz define
// the elapsed media time; sleep until wall-clock catches up. This is the real PSX rate (the
// fixed-15fps guess was too slow). Polls input and returns 1 if Start was pressed (skip).
// uncapped (PSXPORT_FMV_FPS=0) disables pacing for fast headless dumps.
int Fmv::pace(long media_frames, int freq, uint32_t t0, int uncapped) {
  // Reads the pad OWNER'S serviced mask — nothing else. A movie host turn is a presented host frame,
  // so the frame driver's per-frame service (Pad::serviceFrame) has already run this frame and
  // resolved host, forced, replay and control-channel input into `buttons`. Polling here for input of
  // its own would be a second copy of that resolution with different rules, and it would make a movie
  // skip unreplayable: no pad frame is serviced while the mask is resolved by hand, so a recording
  // could not contain one.
  //
  // A LEVEL, not an EDGE, is the skip condition: the frame's own pad service sampled this press and
  // consumed its edge before the guest ran, so waiting for an edge here would never see it. What a
  // player means by skipping a movie is Start held while it is up. The one exception is a Start
  // already down when the movie opens, which `begin` records: that needs a release first, exactly the
  // contract the original player had.
  auto startIsHeld = [this] {
    return (game->pad.buttons & PAD_START) == 0; // the pad mask is active-low
  };
  const auto skipping = [this, &startIsHeld] {
    if (!startIsHeld()) {
      start_held_at_begin_ = false; // a release re-arms the skip for this movie
      return false;
    }
    return !start_held_at_begin_;
  };
  int pressed = skipping() ? 1 : 0;
  if (uncapped || freq <= 0) {
    return pressed;
  }
  const uint32_t target = (uint32_t)((long long)media_frames * 1000 / freq);
  while ((int)(SDL_GetTicks() - t0) < (int)target) {
    SDL_Delay(2);
    if (skipping()) {
      pressed = 1;
    }
  }
  return pressed;
}
#else
int Fmv::pace(long m, int f, uint32_t t, int u) {
  (void)m;
  (void)f;
  (void)t;
  (void)u;
  return 0;
}
#endif

Fmv::~Fmv() {
  free(payload_buf);
  free(codes_buf);
  free(pixels24_buf);
  free(xa_pcm);
}

// ====================================================================================
// STR demux + playback, stepped
// ====================================================================================
void Fmv::ensureScratch() {
  if (payload_buf) {
    return;
  }
  payload_buf = (uint8_t *)malloc(FMV_PAYLOAD_BYTES);
  codes_buf = (uint16_t *)malloc(FMV_CODES_MAX * 2);
  pixels24_buf = (uint8_t *)malloc(1024 * 512 * 3);
  xa_pcm = (int16_t *)malloc(4032 * 2 * 2); // mono sectors yield up to 4032 frames (see xa_decode_sector)
}

// ====================================================================================
// STR demux + playback, stepped
// ====================================================================================
bool Fmv::begin(uint32_t lba, uint32_t size_bytes) {
  game->gpu.gpu_native_init();
  mdec_init();
  ensureScratch();

  base_lba_ = lba;
  sector_count_ = (size_bytes + SECTOR_USER - 1) / SECTOR_USER;
  sector_ = 0;
  frame_in_progress_ = -1;
  payload_len_ = 0;
  expected_chunks_ = 0;
  got_chunks_ = 0;
  frame_width_ = 320;
  frame_height_ = 240;
  media_frames_ = 0;
  xa_freq_ = 37800;
  xa_hist_[0][0] = xa_hist_[0][1] = xa_hist_[1][0] = xa_hist_[1][1] = 0;
  frames_ = 0;
  skipped_ = false;
  finished_ = false;
  active_ = true;

  // Audio: STR interleaves XA-ADPCM sectors with the video sectors. Decode them, play through a
  // dedicated SDL device at the XA rate, and pace VIDEO to the audio/media clock (the real PSX
  // rate). uncapped = PSXPORT_FMV_FPS=0 (headless dumps: no pacing, no audio device).
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
  uncapped_ = false;
  {
    const char *f = cfg_str("PSXPORT_FMV_FPS");
    if (f && *f) {
      uncapped_ = (atoi(f) == 0);
    }
  }
  // A RESUME run (PSXPORT_PAD_RESUME) is replaying its way back to where the player left off, and the
  // boot movies are ~77 s of that journey. This is not the inferred fast-forward the comment above
  // rejects: the user asked for it by name, it ends when the recording does, and the log line below
  // records that this run's movies were uncapped.
  if (!uncapped_ && game->pad.fastForwarding()) {
    uncapped_ = true;
    lucent::info("fmv", "uncapped: PSXPORT_PAD_RESUME is fast-forwarding to the end of the recording");
  }
  // Optional dev cap: PSXPORT_FMV_MAXFRAMES bounds how many frames to play (0/unset = all).
  // Used by the standalone proof to decode just the first frame quickly; harmless in prod.
  max_frames_ = 0;
  {
    const char *mf = cfg_str("PSXPORT_FMV_MAXFRAMES");
    if (mf && *mf) {
      max_frames_ = atoi(mf);
    }
  }

  // A Start already held as the movie begins must not skip it: the skip needs a fresh press, which
  // is what the original player required. Recorded as a LEVEL (the pad mask is active-low), because
  // the edge for a press that arrives later belongs to the frame whose pad service produced it — see
  // pace(). The mask is this frame's serviced one: the frame driver services input before the guest
  // runs, so this is the same mask every other consumer of the frame sees.
  start_held_at_begin_ = (game->pad.buttons & PAD_START) == 0;
  game->pad.resetButtonEdges(game->pad.buttons);
  clock_origin_ms_ = 0;
#ifdef PSXPORT_SDL
  clock_origin_ms_ = SDL_GetTicks();
#endif
  lucent::info("fmv", "begin: LBA {}, {} bytes ({} sectors)", lba, size_bytes, sector_count_);
  return true;
}

void Fmv::step() {
  if (finished_) {
    return;
  }
  uint8_t raw[2352];
  while (sector_ < sector_count_) {
    if (!disc_read_raw(&game->disc, base_lba_ + sector_, raw, 2352)) {
      break;
    }
    sector_++;
    const int submode = raw[18];

    if (submode & 0x04) { // XA-ADPCM audio sector
      const int n = xa_decode_sector(raw, xa_pcm, xa_hist_, &xa_freq_);
      if (sector_ == 1 || media_frames_ == 0) {
        audioOpen(xa_freq_);
      }
      audioQueue(xa_pcm, n);
      media_frames_ += n;
      if (pace(media_frames_, xa_freq_, clock_origin_ms_, uncapped_)) {
        endWithSkip();
        return;
      }
      continue;
    }

    const uint8_t *sbuf = raw + 24; // Form1 video user data
    const uint16_t magic = (uint16_t)(sbuf[0] | (sbuf[1] << 8));
    if (magic != 0x0160) {
      continue; // not a video data sector (padding)
    }

    const int chunk_idx = sbuf[4] | (sbuf[5] << 8);
    const int nchunks = sbuf[6] | (sbuf[7] << 8);
    const int framenum = sbuf[8] | (sbuf[9] << 8) | (sbuf[10] << 16) | (sbuf[11] << 24);
    const int w = sbuf[16] | (sbuf[17] << 8);
    const int h = sbuf[18] | (sbuf[19] << 8);

    if (chunk_idx == 0) {
      frame_in_progress_ = framenum;
      payload_len_ = 0;
      expected_chunks_ = nchunks;
      got_chunks_ = 0;
      frame_width_ = w ? w : 320;
      frame_height_ = h ? h : 240;
    }
    if (frame_in_progress_ != framenum) {
      continue; // out of sync; wait for next chunk-0
    }

    const uint32_t plen = SECTOR_USER - SUBHDR_LEN;
    if (payload_len_ + plen <= FMV_PAYLOAD_BYTES) {
      memcpy(payload_buf + payload_len_, sbuf + SUBHDR_LEN, plen);
      payload_len_ += plen;
    }
    got_chunks_++;

    if (expected_chunks_ > 0 && got_chunks_ >= expected_chunks_) {
      const uint32_t frame_payload = payload_len_;
      frame_in_progress_ = -1;
      expected_chunks_ = 0;
      got_chunks_ = 0;
      payload_len_ = 0;
      const int ncodes =
          bsDecodeFrame(payload_buf, frame_payload, frame_width_, frame_height_, codes_buf, (int)FMV_CODES_MAX);
      lucent::debug("fmv",
                    "frame {}: {}x{}, {} payload bytes, {} codes",
                    framenum,
                    frame_width_,
                    frame_height_,
                    frame_payload,
                    ncodes);
      if (ncodes > 0 && mdec_decode_to_rgb888(codes_buf, ncodes, frame_width_, frame_height_, pixels24_buf) > 0) {
        presentFrame(pixels24_buf, frame_width_, frame_height_);
        frames_++;
        // Pace video to the audio/media clock (no audio sector here, so just gate on it).
        if (pace(media_frames_, xa_freq_, clock_origin_ms_, uncapped_)) {
          endWithSkip();
          return;
        }
        if (max_frames_ && frames_ >= max_frames_) {
          endAtEndOfStream();
          return;
        }
      }
      // ONE frame presented (or attempted) per call: hand the turn back so the frame loop, the
      // control channel and window events all get a slice between movie frames.
      return;
    }
  }
  endAtEndOfStream();
}

// Every exit from a movie — end of stream, the dev cap, and a Start skip — leaves the movie's last
// (possibly partial) frame in the display FB. Black it + present once so no FMV residue is revealed
// under a front end that is still loading its 2D layer. Deterministic hand-off, no sleep/retry.
void Fmv::endAtEndOfStream() {
  if (finished_) {
    return;
  }
  finished_ = true;
  active_ = false;
  lucent::debug("fmv",
                "done: {} video frames, {} audio sample-pairs ({:.2f}s @ {}Hz)",
                frames_,
                media_frames_,
                media_frames_ / (double)(xa_freq_ ? xa_freq_ : 37800),
                xa_freq_);
  audioClose();
  void gpu_clear_display(Core *);
  gpu_clear_display(&game->core);
}

void Fmv::endWithSkip() {
  if (finished_) {
    return;
  }
  skipped_ = true;
  endAtEndOfStream();
  lucent::info("fmv", "skipped by Start at frame {}", frames_);
  // No press-consumption wait is needed here, and adding one would be wrong: this movie ends INSIDE a
  // pad frame, so no further frame is serviced until the guest resumes, and a wait loop would spin
  // against a mask that cannot change. The original player waited because its own poll consumed the
  // press; here the press belongs to the frame whose pad service produced it, and the guest was not
  // running in that frame, so it cannot act on the edge. A Start still held afterwards yields no
  // further edge, which is exactly the state the original player waited for.
}

int Fmv::playLba(uint32_t lba, uint32_t size_bytes) {
  if (!begin(lba, size_bytes)) {
    return -1;
  }
  return playToEnd();
}

// A blocking caller owns the whole turn until the movie ends, so no frame driver services the pad in
// between: this loop pumps host input itself (host only — no pad-frame clock, replay or record tick),
// which is what lets the player's Start reach the skip check that step() reads from `pad.buttons`.
int Fmv::playToEnd() {
  while (!finished()) {
    game->pad.pumpHostInput();
    step();
  }
  return frames_;
}

bool Fmv::beginPath(const char *path) {
  uint32_t lba = 0, size = 0;
  if (!fmv_resolve_path(&game->disc, path, &lba, &size)) {
    lucent::info("fmv", "could not resolve {} on disc", path ? path : "(null)");
    finished_ = true;
    active_ = false;
    return false;
  }
  lucent::info("fmv", "{} -> LBA {}, {} bytes", path ? path : "(null)", lba, size);
  return begin(lba, size);
}

int Fmv::play(const char *path) {
  if (!beginPath(path)) {
    return -1;
  }
  return playToEnd();
}

// ---- ISO9660 path resolution (walks directories via disc_read_sector) ----------------
static uint32_t le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void iso_name(const uint8_t *rec, int nlen, char *out, int outsz) {
  int j = 0;
  for (int i = 0; i < nlen && j < outsz - 1; i++) {
    char c = (char)rec[i];
    if (c == ';') {
      break;
    }
    if (c >= 'a' && c <= 'z') {
      c = c - 'a' + 'A';
    }
    out[j++] = c;
  }
  out[j] = 0;
}
static int iso_find_child(DiscState *disc,
                          uint32_t dir_lba,
                          uint32_t dir_size,
                          const char *name,
                          int want_dir,
                          uint32_t *clba,
                          uint32_t *csize) {
  char upper[256];
  int n = 0;
  for (const char *p = name; *p && n < 255; p++) {
    char c = *p;
    if (c >= 'a' && c <= 'z') {
      c = c - 'a' + 'A';
    }
    upper[n++] = c;
  }
  upper[n] = 0;

  uint32_t nsec = (dir_size + SECTOR_USER - 1) / SECTOR_USER;
  uint8_t sbuf[SECTOR_USER];
  for (uint32_t s = 0; s < nsec; s++) {
    if (!disc_read_sector(disc, dir_lba + s, sbuf)) {
      return 0;
    }
    uint32_t pos = 0;
    while (pos < SECTOR_USER) {
      uint8_t len = sbuf[pos];
      if (len == 0) {
        break;
      }
      if (pos + len > SECTOR_USER) {
        break;
      }
      uint8_t flags = sbuf[pos + 25];
      uint8_t nlen = sbuf[pos + 32];
      uint32_t e_lba = le32(&sbuf[pos + 2]);
      uint32_t e_size = le32(&sbuf[pos + 10]);
      if (!(nlen == 1 && (sbuf[pos + 33] == 0 || sbuf[pos + 33] == 1))) {
        char nm[256];
        iso_name(&sbuf[pos + 33], nlen, nm, sizeof nm);
        int is_dir = (flags & 0x02) ? 1 : 0;
        if (is_dir == want_dir && strcmp(nm, upper) == 0) {
          *clba = e_lba;
          *csize = e_size;
          return 1;
        }
      }
      pos += len;
    }
  }
  return 0;
}
static int fmv_resolve_path(DiscState *disc, const char *path, uint32_t *out_lba, uint32_t *out_size) {
  uint8_t pvd[SECTOR_USER];
  if (!disc_read_sector(disc, 16, pvd)) {
    return 0;
  }
  if (memcmp(pvd + 1, "CD001", 5) != 0) {
    return 0;
  }
  uint32_t dir_lba = le32(pvd + 156 + 2);
  uint32_t dir_size = le32(pvd + 156 + 10);

  char comp[256];
  const char *p = path;
  while (*p) {
    int n = 0;
    while (*p && *p != '/' && *p != '\\' && n < 255) {
      comp[n++] = *p++;
    }
    comp[n] = 0;
    while (*p == '/' || *p == '\\') {
      p++;
    }
    int last = (*p == 0);
    uint32_t clba = 0, csize = 0;
    if (!iso_find_child(disc, dir_lba, dir_size, comp, last ? 0 : 1, &clba, &csize)) {
      return 0;
    }
    if (last) {
      *out_lba = clba;
      *out_size = csize;
      return 1;
    }
    dir_lba = clba;
    dir_size = csize;
  }
  return 0;
}
