#include "c_subsys.h"
#include "core.h"
#include "game.h"       // class Fmv lives on Game (game->fmv); this TU implements its methods
#include "host_input.h" // psx::input::kButtonStart
#include "watchdog.h"
#include <algorithm>
#include <chrono>
#include <limits>
#include <optional>
#include <thread>
#include <vector>
// Native FMV player for the Tomba!2 PC port.
//
// Plays PSX .STR movies (MOVIE/LOGO.STR, MOVIE/OP.STR) entirely with our own code,
// bypassing the game's PSX StrPlayer / CD-streaming machinery. Pipeline:
//
//   disc (libchdr, disc.c)  ->  STR demux (this file)
//     ->  BS (MDEC bitstream) VLC decode (fmv_decode.cpp)
//       ->  MDEC IDCT/YCbCr (Beetle mdec.c via mdec_beetle.c, driven by fmv_decode.cpp)
//         ->  build (DisplayPresenter::buildHostPicture), pace, show
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
#include "number_text.h" // the pure decode machinery (shared with tools/fmv_export + fmv_compare)
#include "render_device.h"
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

// How far ahead of its play time audio is queued.
constexpr double kAudioLeadMs = 1000.0;

// ====================================================================================
// MDEC feed + macroblock tiling — in fmv_decode.cpp (psx::movie::decodeMdecToRgb555 / _to_rgb888),
// which upload the quant/IDCT tables, feed the MDEC in DMA0/DMA1 ping-pong, drain the
// frame, and tile the 16x16 macroblocks column-major. See above for the why.
// ====================================================================================
// Present the decoded movie frame as a NATIVE RGBA image, letterboxed 4:3 with black bars
// (DisplayPresenter::buildHostPicture) — NOT a VRAM upload. The PC renderer composites only native submits over
// black; a CPU->VRAM upload + gpu_present would be blacked out by that. Presenting the frame directly
// is also the centering fix — it pillarboxes 4:3 on widescreen instead of left-aligning in the wide FB.
//
// The frame is 24bpp because that is what retail PSX movies were authored for: Toy Story 2's FMV
// overlay asks its player for depth 3 in ITS convention (compared against the literal 3 before its
// 24-bit upload path), and RGB555 would spend two bits per channel the movie actually encodes.
void build_rgba(Core *core, const std::vector<uint8_t> &rgba, int width, int height) {
  core->game->display.buildHostPicture(rgba.data(), {width, height}, 1.0f);
  // Movie frames are progress, not main-present readiness.
  psx::platform::Watchdog::progress();
}

void build_rgb888(Core *core, std::vector<uint8_t> &rgba, const uint8_t *pixels, int width, int height) {
  const int npix = width * height;
  rgba.resize((size_t)npix * 4);
  for (int i = 0; i < npix; i++) {
    rgba[i * 4 + 0] = pixels[i * 3 + 0]; // already full 8-bit R,G,B from the MDEC
    rgba[i * 4 + 1] = pixels[i * 3 + 1];
    rgba[i * 4 + 2] = pixels[i * 3 + 2];
    rgba[i * 4 + 3] = 255;
  }
  build_rgba(core, rgba, width, height);
}

// One STR video chunk's sub-header (layout above).
struct VideoChunk {
  int chunk = 0;
  int chunks = 0;
  int frame = 0;
  int width = 0;
  int height = 0;
};

std::optional<VideoChunk> videoChunk(const uint8_t *raw) {
  const uint8_t *sbuf = raw + 24; // Form1 video user data
  if ((raw[18] & 0x04) != 0 || (sbuf[0] | (sbuf[1] << 8)) != 0x0160) {
    return std::nullopt; // audio, or padding
  }
  return VideoChunk{sbuf[4] | (sbuf[5] << 8),
                    sbuf[6] | (sbuf[7] << 8),
                    sbuf[8] | (sbuf[9] << 8) | (sbuf[10] << 16) | (sbuf[11] << 24),
                    sbuf[16] | (sbuf[17] << 8),
                    sbuf[18] | (sbuf[19] << 8)};
}

class DiscSectors final : public psx::movie::SectorSource {
public:
  explicit DiscSectors(psx::cd::Disc &disc) : disc_(disc) {}
  bool readRaw(uint32_t lba, uint8_t *out) override {
    return disc_.readRaw(lba, out, 2352);
  }

private:
  psx::cd::Disc &disc_;
};

} // namespace

void Fmv::buildFrame(const uint8_t *pixels, psx::present::PixelSize size) {
  build_rgb888(&game->core, rgba_scratch_, pixels, size.width, size.height);
}

void Fmv::showFrame(int heldFields) {
  game->perf.heldFor(heldFields);
  game->display.show();
}

void Fmv::AudioLag::add(double lagMs) {
  first = samples == 0 ? lagMs : first;
  min = samples == 0 ? lagMs : std::min(min, lagMs);
  max = samples == 0 ? lagMs : std::max(max, lagMs);
  last = lagMs;
  ++samples;
}

void Fmv::learnLayout() {
  clock_ = {};
  uint8_t raw[2352];
  const uint32_t end = std::min(sector_count_, psx::movie::MovieClock::kLearnSectors);
  for (uint32_t sector = 0; sector < end && !clock_.learned(); ++sector) {
    if (!source_->readRaw(base_lba_ + sector, raw)) {
      break;
    }
    psx::movie::SectorFacts facts;
    if ((raw[18] & 0x04) != 0) {
      facts.audio = psx::movie::xaSectorFormat(raw);
    } else if (const std::optional<VideoChunk> chunk = videoChunk(raw)) {
      facts.videoFrame = chunk->frame;
    }
    clock_.learn(facts);
  }
  lucent::info("fmv",
               "clock: {:.4f} ms per sector, frame slot {} sectors (0 = due at its last chunk)",
               clock_.sectorMs(),
               clock_.slotSectors());
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
  if (!psx::audio::mayOpenOutput(psx::config::no_audio(), game->renderDevice.windowed())) {
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

std::optional<double> Fmv::audioLagMs() const {
  const std::optional<uint32_t> first = clock_.firstAudioSector();
  const int queuedBytes = stream ? SDL_GetAudioStreamQueued((SDL_AudioStream *)stream) : -1;
  if (!first || queuedBytes < 0 || xa_freq_ <= 0) {
    return std::nullopt;
  }
  const long queuedFrames = queuedBytes / 4; // S16 stereo
  const double playedMs = static_cast<double>(media_frames_ - queuedFrames) * 1000.0 / xa_freq_;
  return elapsedMs() - (clock_.sectorDeliveredMs(*first) + playedMs);
}
#endif

// Wait for `dueMs` on the movie clock. Polls nothing: the frame's pad service has already resolved
// host, forced, replay and control-channel input into `buttons`, and a second resolution here would
// make a movie skip unreplayable.
//
// A LEVEL, not an EDGE, is the skip condition: the frame's own pad service consumed the press's edge
// before the guest ran. A Start already down when the movie opens, which `begin` records, needs a
// release first, the contract the original player had. Uncapped (PSXPORT_FMV_FPS=0) never waits.
int Fmv::pace(double dueMs) {
  const bool startHeld = (game->pad.buttons & psx::input::kButtonStart) == 0; // the pad mask is active-low
  if (!startHeld) {
    start_held_at_begin_ = false; // a release re-arms the skip for this movie
  }
  const int pressed = startHeld && !start_held_at_begin_ ? 1 : 0;
  if (uncapped_) {
    feedAudio(dueMs);
    return pressed;
  }
  while (true) {
    const double now = elapsedMs();
    if (now >= dueMs) {
      break;
    }
    feedAudio(now);
    const double wakeMs = std::min(dueMs, nextAudioQueueMs().value_or(dueMs));
    std::this_thread::sleep_until(origin_ + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                                std::chrono::duration<double, std::milli>(wakeMs)));
  }
  feedAudio(elapsedMs());
  return pressed;
}

double Fmv::elapsedMs() const {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - origin_).count();
}

// The first XA sector is queued when the drive delivers it, which starts the audio clock; each later
// one kAudioLeadMs ahead of that, so a host stall shorter than the lead does not starve the device.
std::optional<double> Fmv::nextAudioQueueMs() {
  while (!audio_loaded_ && audio_sector_ < sector_count_) {
    if (!source_->readRaw(base_lba_ + audio_sector_, audio_raw_.data())) {
      audio_sector_ = sector_count_;
      break;
    }
    if ((audio_raw_[18] & 0x04) != 0) {
      audio_loaded_ = true;
    } else {
      ++audio_sector_;
    }
  }
  if (!audio_loaded_) {
    return std::nullopt;
  }
  const double leadMs = media_frames_ == 0 ? 0.0 : kAudioLeadMs;
  return clock_.sectorDeliveredMs(audio_sector_) - leadMs;
}

void Fmv::feedAudio(double nowMs) {
  for (std::optional<double> queueMs = nextAudioQueueMs(); queueMs && *queueMs <= nowMs; queueMs = nextAudioQueueMs()) {
    const int n = psx::movie::decodeXaSector(audio_raw_.data(), xa_pcm, xa_hist_, &xa_freq_);
    if (media_frames_ == 0) {
      audioOpen(xa_freq_);
    }
    audioQueue(xa_pcm, n);
    media_frames_ += n;
    audio_loaded_ = false;
    ++audio_sector_;
  }
}

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
  disc_source_ = std::make_unique<DiscSectors>(game->disc);
  return begin(*disc_source_, lba, size_bytes);
}

bool Fmv::begin(psx::movie::SectorSource &source, uint32_t lba, uint32_t size_bytes) {
  mdec_init();
  source_ = &source;
  ensureScratch();

  base_lba_ = lba;
  sector_count_ = (size_bytes + SECTOR_USER - 1) / SECTOR_USER;
  sector_ = 0;
  audio_sector_ = 0;
  audio_loaded_ = false;
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
  // dedicated SDL device at the XA rate; every sector and frame is due on the stream's sector clock,
  // which the XA interleave ties to the audio. uncapped = PSXPORT_FMV_FPS=0 (headless dumps: no pacing).
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
  lucent::info("fmv", "begin: LBA {}, {} bytes ({} sectors)", lba, size_bytes, sector_count_);
  learnLayout();
  grid_ = psx::movie::MovieFieldGrid(1000000.0 / game->framePacer.fieldRateMilliHz(game->core));
  audio_lag_ = {};
  intervals_at_begin_ = game->perf.presentSeries().intervals;
  missed_at_begin_ = game->perf.presentSeries().missed;
  origin_ = std::chrono::steady_clock::now();
  return true;
}

void Fmv::step() {
  if (finished_) {
    return;
  }
  uint8_t raw[2352];
  while (sector_ < sector_count_) {
    if (!source_->readRaw(base_lba_ + sector_, raw)) {
      break;
    }
    const uint32_t at = sector_++;
    const std::optional<VideoChunk> video = videoChunk(raw);
    if (!video) {
      continue; // audio, which feedAudio reads on its own cursor, or padding
    }
    const int chunk_idx = video->chunk;
    const int nchunks = video->chunks;
    const int framenum = video->frame;
    const int w = video->width;
    const int h = video->height;

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
      memcpy(payload_buf + payload_len_, raw + 24 + SUBHDR_LEN, plen);
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
      if (ncodes > 0 && psx::movie::decodeMdecToRgb888({codes_buf, static_cast<std::size_t>(ncodes)},
                                                       {frame_width_, frame_height_},
                                                       pixels24_buf) > 0) {
        const psx::movie::MovieFieldGrid::Show show = grid_.show(clock_.frameDueMs({framenum, at}));
        buildFrame(pixels24_buf, {frame_width_, frame_height_});
        if (pace(show.atMs)) {
          endWithSkip();
          return;
        }
        showFrame(show.heldFields);
        frames_++;
        const std::optional<double> lag = audioLagMs();
        if (lag) {
          audio_lag_.add(*lag);
        }
        lucent::debug("fmv",
                      "frame {}: {}x{}, {} payload bytes, {} codes, shown at {:.1f} ms after {} fields, audio lag "
                      "{:.1f} ms",
                      framenum,
                      frame_width_,
                      frame_height_,
                      frame_payload,
                      ncodes,
                      show.atMs,
                      show.heldFields,
                      lag.value_or(0.0));
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
  // The movie ends when its audio has played.
  feedAudio(std::numeric_limits<double>::infinity());
  const std::optional<uint32_t> firstAudio = clock_.firstAudioSector();
  const double audioEndMs =
      firstAudio ? clock_.sectorDeliveredMs(*firstAudio) + static_cast<double>(media_frames_) * 1000.0 / xa_freq_ : 0.0;
  if (pace(audioEndMs)) {
    endWithSkip();
    return;
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
  lucent::info("fmv",
               "done: {} video frames, {} audio sample-pairs ({:.2f}s @ {}Hz), presents {} missed {}, audio lag "
               "first {:.1f} last {:.1f} min {:.1f} max {:.1f} ms",
               frames_,
               media_frames_,
               static_cast<double>(media_frames_) / (double)(xa_freq_ ? xa_freq_ : 37800),
               xa_freq_,
               game->perf.presentSeries().intervals - intervals_at_begin_,
               game->perf.presentSeries().missed - missed_at_begin_,
               audio_lag_.first,
               audio_lag_.last,
               audio_lag_.min,
               audio_lag_.max);
  audioClose();
  game->display.build(psx::present::PresentCompletion::Transition);
  game->display.show();
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
