// native_fmv.h — class Fmv — the native .STR movie player, owned by Game (`c->game->fmv`,
// back-pointer wired in Game()). Implemented in native_fmv.cpp: STR demux -> BS VLC decode ->
// MDEC IDCT/YCbCr (Beetle mdec.c) -> present, plus the interleaved XA-ADPCM audio sectors through a
// dedicated host audio stream at the XA rate. Each sector and frame is due on the stream's sector clock,
// which is the audio clock (movie_clock.h).
//
// STEPPED, NOT BLOCKING. A movie is played by begin() + one step() per HOST TURN, each presenting at
// most one frame. Playing a whole movie inside one turn looked simpler and was wrong three ways: it
// starved the frame loop, the control channel and window events for the movie's whole duration; a
// Start press could not reach the pad while it ran, so the skip this class implements could not be
// exercised at all; and nothing could observe a movie frame. One frame per turn costs a handful of
// instructions and makes all three true.
#pragma once
#include "movie_clock.h"
#include "pixel_size.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>
class Core;
class Game;

namespace psx::movie {

// Where a movie's raw 2352-byte sectors come from: the disc, in the product.
class SectorSource {
public:
  virtual ~SectorSource() = default;
  virtual bool readRaw(std::uint32_t lba, std::uint8_t *out) = 0;
};

} // namespace psx::movie

class Fmv {
public:
  Game *game = nullptr;

  // Open the movie at `lba` for `size_bytes` and arm playback. Returns false when the extent cannot
  // be opened, which leaves the player finished (a caller then answers as the retail player would for
  // a movie that never played).
  bool begin(uint32_t lba, uint32_t size_bytes);
  // The same, reading `source`, which must outlive the movie.
  bool begin(psx::movie::SectorSource &source, uint32_t lba, uint32_t size_bytes);
  // Decode and present AT MOST ONE frame, then return. Ends the movie (end of stream, or a Start
  // press) and closes the audio stream; finished() says which happened.
  void step();
  bool finished() const {
    return finished_;
  }
  // True between begin() and the movie's end. The movie paces and presents its own frames, so a
  // title's frame driver presents nothing else meanwhile.
  bool presenting() const {
    return active_;
  }
  bool skipped() const {
    return skipped_;
  }
  int framesPlayed() const {
    return frames_;
  }

  // Resolve `path` on the disc (ISO9660) and OPEN it, without playing: the caller then drives
  // step(). This is the entry for a title whose guest can be interrupted mid-movie; play() below is
  // the blocking convenience built on it.
  bool beginPath(const char *path);
  // play(path): resolve and play to completion. Returns frames played, -1 when the path cannot be
  // resolved. These block the calling turn on purpose: they are for callers with nothing else to do
  // until the movie ends (the boot movie).
  int play(const char *path);
  int playLba(uint32_t lba, uint32_t size_bytes);

  ~Fmv();

private:
  // host audio sink: SDL3 push-model stream bound to the default playback device, opened at the
  // movie's XA rate. Held opaquely so this header stays SDL-free.
  void *stream = nullptr; // SDL_AudioStream* (was s_fmv_stream)
  int stream_freq = 0;    // rate the stream was opened at (was s_fmv_freq)
  void audioOpen(int freq);
  void audioQueue(const int16_t *pcm, int frames);
  void audioClose();
  // The blocking loop behind play()/playLba(): pumps host input and steps until the movie ends.
  int playToEnd();
  // Learn the stream's sector clock and frame slot from its leading sectors.
  void learnLayout();
  // Wait until `dueMs` after the movie began, feeding audio meanwhile; returns 1 if Start is held (skip).
  int pace(double dueMs);
  double elapsedMs() const;
  // When the next XA sector is queued; nullopt when none is left. Loads it into audio_raw_.
  std::optional<double> nextAudioQueueMs();
  // Queue every XA sector whose queue time is at or before `nowMs`.
  void feedAudio(double nowMs);
  // How far the audio device's position trails the movie clock, in ms; nullopt without audio.
  std::optional<double> audioLagMs() const;
  // Present one decoded 24-bit frame, the previous one having been held for `heldFields`.
  void presentFrame(const uint8_t *pixels, psx::present::PixelSize size, int heldFields);
  // Allocate the multi-megabyte decode scratch on first use.
  void ensureScratch();
  // End the movie: endAtEndOfStream is the ordinary end (stream exhausted, or the dev cap), and
  // endWithSkip is the guest asking out with Start. Both are idempotent.
  void endAtEndOfStream();
  void endWithSkip();

  // decode scratch, heap-allocated on first play (multi-MB; was file-scope static buffers).
  // The MDEC in/out word buffers live inside fmv_decode.cpp (function-static), since the
  // offline tools use the same decode entry points.
  uint8_t *payload_buf = nullptr;  // concatenated BS payload (512 KB)
  uint16_t *codes_buf = nullptr;   // MDEC run-level code stream (512K codes)
  uint8_t *pixels24_buf = nullptr; // decoded RGB888 frame (1024x512 px, 3 bytes each)
  // RGBA expansion scratch for presentation; re-harness cpp_policy forbids a function-local static.
  std::vector<uint8_t> rgba_scratch_;
  int16_t *xa_pcm = nullptr; // one sector's decoded stereo PCM (4032 frames)

  // --- playback cursors, so step() can stop between frames -------------------------------------
  std::unique_ptr<psx::movie::SectorSource> disc_source_;
  psx::movie::SectorSource *source_ = nullptr;
  // Audio reads ahead of video on its own cursor; audio_raw_ holds the XA sector at audio_sector_.
  uint32_t audio_sector_ = 0;
  bool audio_loaded_ = false;
  std::vector<uint8_t> audio_raw_ = std::vector<uint8_t>(2352);
  uint32_t base_lba_ = 0;
  uint32_t sector_count_ = 0;
  uint32_t sector_ = 0;
  int frame_in_progress_ = -1; // STR frame number currently being assembled
  uint32_t payload_len_ = 0;
  int expected_chunks_ = 0;
  int got_chunks_ = 0;
  int frame_width_ = 320;
  int frame_height_ = 240;
  long media_frames_ = 0; // cumulative audio sample-pairs = the media clock
  int xa_freq_ = 37800;
  int16_t xa_hist_[2][2] = {{0, 0}, {0, 0}};
  psx::movie::MovieClock clock_;
  psx::movie::MovieFieldGrid grid_{1.0}; // replaced at begin() with the display's field period
  std::chrono::steady_clock::time_point origin_{};
  // Audio lag over the movie: a drift shows as last moving away from first.
  struct AudioLag {
    double first = 0.0;
    double last = 0.0;
    double min = 0.0;
    double max = 0.0;
    int samples = 0;
    void add(double lagMs);
  } audio_lag_;
  std::uint64_t intervals_at_begin_ = 0;
  std::uint64_t missed_at_begin_ = 0;
  int max_frames_ = 0; // PSXPORT_FMV_MAXFRAMES dev cap; 0 = play the whole movie
  bool uncapped_ = false;
  bool finished_ = true;
  bool active_ = false;
  bool skipped_ = false;
  int frames_ = 0;
  bool start_held_at_begin_ = false; // Start already down when the movie opened: needs a release first
};
