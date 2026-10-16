#include "c_subsys.h"
#include "core.h"
#include "game.h" // class Fmv lives on Game (game->fmv); this TU implements its methods
#include "gpu_vk.h"
#include "host_input.h" // psx::input::kButtonStart
#include "watchdog.h"
#include <algorithm>
#include <cmath>
#include <vector>
// Native FMV player for the Tomba!2 PC port.
//
// Plays PSX .STR movies (MOVIE/LOGO.STR, MOVIE/OP.STR) entirely with our own code,
// bypassing the game's PSX StrPlayer / CD-streaming machinery. Pipeline:
//
//   disc (libchdr, disc.c)  ->  STR demux (this file)
//     ->  BS (MDEC bitstream) VLC decode (fmv_decode.cpp)
//       ->  MDEC IDCT/YCbCr (Beetle mdec.c via mdec_beetle.c, driven by fmv_decode.cpp)
//         ->  present (DisplayPresenter::presentHostPicture)
//
// THE DECODE IS SHARED: psx::movie::decodeBsFrame / psx::movie::decodeMdecToRgb555 / psx::movie::decodeXaSector live in
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
#include "config_vars.h"
#include "diagnostic_config.h"
#include "fmv_decode.h"
#include "gpu_vk_device.h"
#include "number_text.h" // the pure decode machinery (shared with tools/fmv_export + fmv_compare)
#include <lucent/log.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// pad access via c->game->pad — class Pad on Game (see game.h): pollHostInput(), buttons field

#define SECTOR_USER 2048u
#define SUBHDR_LEN 32u

// decode-scratch sizes (heap members on Fmv, allocated on first play)

namespace {

// ====================================================================================
// MDEC feed + macroblock tiling — in fmv_decode.cpp (psx::movie::decodeMdecToRgb555 / _to_rgb888),
// which upload the quant/IDCT tables, feed the MDEC in DMA0/DMA1 ping-pong, drain the
// frame, and tile the 16x16 macroblocks column-major. See above for the why.
// ====================================================================================
// Present the decoded movie frame as a NATIVE RGBA image, letterboxed 4:3 with black bars
// (DisplayPresenter::presentHostPicture) — NOT a VRAM upload. The PC renderer composites only native submits over
// black; a CPU->VRAM upload + gpu_present would be blacked out by that. Presenting the frame directly
// is also the centering fix — it pillarboxes 4:3 on widescreen instead of left-aligning in the wide FB.
//
// The frame is 24bpp because that is what retail PSX movies were authored for: Toy Story 2's FMV
// overlay asks its player for depth 3 in ITS convention (compared against the literal 3 before its
// 24-bit upload path), and RGB555 would spend two bits per channel the movie actually encodes.
void submit_rgba(Core *core, const std::vector<uint8_t> &rgba, int width, int height) {
  core->game->display.presentHostPicture(rgba.data(), {width, height}, 1.0f);
  // Movie frames are progress, not main-present readiness.
  psx::platform::Watchdog::progress();
}

void present_rgb888(Core *core, std::vector<uint8_t> &rgba, const uint8_t *pixels, int width, int height) {
  const int npix = width * height;
  rgba.resize((size_t)npix * 4);
  for (int i = 0; i < npix; i++) {
    rgba[i * 4 + 0] = pixels[i * 3 + 0]; // already full 8-bit R,G,B from the MDEC
    rgba[i * 4 + 1] = pixels[i * 3 + 1];
    rgba[i * 4 + 2] = pixels[i * 3 + 2];
    rgba[i * 4 + 3] = 255;
  }
  submit_rgba(core, rgba, width, height);
}

} // namespace

int Fmv::heldFields() const {
  if (media_frames_ <= 0 || xa_freq_ <= 0) {
    return 1;
  }
  // The movie's frame period on its media clock, in display fields.
  const double frameMs = static_cast<double>(media_frames_) * 1000.0 / xa_freq_ / (frames_ + 1);
  const double fieldMs = 1000000.0 / game->framePacer.fieldRateMilliHz(game->core);
  return std::max(1, static_cast<int>(std::lround(frameMs / fieldMs)));
}

void Fmv::presentFrame(const uint8_t *pixels, int width, int height) {
  game->perf.heldFor(heldFields());
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
// CD-XA ADPCM audio decode lives in fmv_decode.cpp (psx::movie::decodeXaSector) — the shared machinery
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
  if (!psx::audio::mayOpenOutput(psx::config::no_audio(), game->gpu_dev.windowed())) {
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
int Fmv::pace() {
  const long media_frames = media_frames_;
  const int freq = xa_freq_;
  const uint32_t t0 = clock_origin_ms_;
  const bool uncapped = uncapped_;
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
    return (game->pad.buttons & psx::input::kButtonStart) == 0; // the pad mask is active-low
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
int Fmv::pace() {
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
  payload_buf = (uint8_t *)malloc(psx::movie::kPayloadBytes);
  codes_buf = (uint16_t *)malloc(psx::movie::kMaxCodes * sizeof(uint16_t));
  pixels24_buf = (uint8_t *)malloc(std::size_t{1024} * 512 * 3);
  xa_pcm = (int16_t *)malloc(std::size_t{4032} * 2 *
                             sizeof(int16_t)); // mono sectors yield up to 4032 frames (see psx::movie::decodeXaSector)
}

// ====================================================================================
// STR demux + playback, stepped
// ====================================================================================
bool Fmv::begin(uint32_t lba, uint32_t size_bytes) {
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
    const std::string fps = psx::config::movie_fps();
    if (!fps.empty()) {
      uncapped_ = psx::config::parseInt(fps).value_or(0) == 0;
    }
  }
  // A resume replay or a host pre-roll asked for the fast-forward by name; the log line records it.
  if (!uncapped_ && game->fastForwarding()) {
    uncapped_ = true;
    lucent::info("fmv", "uncapped: the run is fast-forwarding (resume replay or host pre-roll)");
  }
  // Optional dev cap: PSXPORT_FMV_MAXFRAMES bounds how many frames to play (0/unset = all).
  // Used by the standalone proof to decode just the first frame quickly; harmless in prod.
  max_frames_ = 0;
  {
    max_frames_ = static_cast<int>(psx::config::movie_max_frames());
  }

  // A Start already held as the movie begins must not skip it: the skip needs a fresh press, which
  // is what the original player required. Recorded as a LEVEL (the pad mask is active-low), because
  // the edge for a press that arrives later belongs to the frame whose pad service produced it — see
  // pace(). The mask is this frame's serviced one: the frame driver services input before the guest
  // runs, so this is the same mask every other consumer of the frame sees.
  start_held_at_begin_ = (game->pad.buttons & psx::input::kButtonStart) == 0;
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
    if (!game->disc.readRaw(base_lba_ + sector_, raw, 2352)) {
      break;
    }
    sector_++;
    const int submode = raw[18];

    if (submode & 0x04) { // XA-ADPCM audio sector
      const int n = psx::movie::decodeXaSector(raw, xa_pcm, xa_hist_, &xa_freq_);
      if (sector_ == 1 || media_frames_ == 0) {
        audioOpen(xa_freq_);
      }
      audioQueue(xa_pcm, n);
      media_frames_ += n;
      if (pace()) {
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
    if (payload_len_ + plen <= psx::movie::kPayloadBytes) {
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
      const int ncodes = psx::movie::decodeBsFrame(
          {payload_buf, frame_payload}, {frame_width_, frame_height_}, {codes_buf, psx::movie::kMaxCodes});
      lucent::debug("fmv",
                    "frame {}: {}x{}, {} payload bytes, {} codes",
                    framenum,
                    frame_width_,
                    frame_height_,
                    frame_payload,
                    ncodes);
      if (ncodes > 0 && psx::movie::decodeMdecToRgb888({codes_buf, static_cast<std::size_t>(ncodes)},
                                                       {frame_width_, frame_height_},
                                                       pixels24_buf) > 0) {
        presentFrame(pixels24_buf, frame_width_, frame_height_);
        frames_++;
        // Pace video to the audio/media clock (no audio sector here, so just gate on it).
        if (pace()) {
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
                static_cast<double>(media_frames_) / (double)(xa_freq_ ? xa_freq_ : 37800),
                xa_freq_);
  audioClose();
  game->display.present(psx::present::PresentCompletion::Transition);
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
    game->pad.pollHostInput();
    step();
  }
  return frames_;
}

bool Fmv::beginPath(const char *path) {
  const std::optional<psx::cd::DiscFile> file = path ? game->disc.findFile(path) : std::nullopt;
  if (!file) {
    lucent::info("fmv", "could not resolve {} on disc", path ? path : "(null)");
    finished_ = true;
    active_ = false;
    return false;
  }
  lucent::info("fmv", "{} -> LBA {}, {} bytes", path, file->lba, file->size);
  return begin(file->lba, file->size);
}

int Fmv::play(const char *path) {
  if (!beginPath(path)) {
    return -1;
  }
  return playToEnd();
}
