// class Fmv: the native .STR movie player, one per Game. STR demux, BS decode and MDEC (fmv_decode) present
// each frame on its sector-clock due field; the interleaved XA audio plays through its own host stream at the XA
// rate, which is that clock (movie_clock.h). begin() then one step() per host turn, each presenting at most one
// frame, so the frame loop, the control channel and a Start skip all run between frames.
#pragma once
#include "cd_position.h"
#include "fmv_decode.h"
#include "host_audio_output.h"
#include "host_clock.h"
#include "movie_clock.h"
#include "pixel_size.h"
#include "xa_adpcm.h"

#include <array>
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

  // Open the movie at `lba` for `size_bytes` and arm playback. False leaves the player finished, and the
  // caller answers as the retail player would for a movie that never played.
  bool begin(uint32_t lba, uint32_t size_bytes);
  // The same, reading `source`, which must outlive the movie.
  bool begin(psx::movie::SectorSource &source, uint32_t lba, uint32_t size_bytes);
  // Decode and present at most one frame. Ends the movie at end of stream or on a Start press.
  void step();
  bool finished() const {
    return finished_;
  }
  // Between begin() and the movie's end the movie presents its own frames.
  bool presenting() const {
    return active_;
  }
  bool skipped() const {
    return skipped_;
  }
  int framesPlayed() const {
    return frames_;
  }

  // Resolve `path` on the disc and open it; the caller then drives step().
  bool beginPath(const char *path);
  // Resolve and play to the end, blocking the turn (a boot movie). Returns frames played, -1 when unresolved.
  int play(const char *path);

private:
  void audioOpen(int freq);
  // Pumps host input and steps until the movie ends.
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
  void buildFrame(const uint8_t *pixels, psx::present::PixelSize size);
  // Show the built frame, the previous one having been held for `heldFields`.
  void showFrame(int heldFields);
  void ensureScratch();
  // The ordinary end (stream exhausted, or the dev cap) and the Start skip; both idempotent.
  void endAtEndOfStream();
  void endWithSkip();

  psx::audio::HostAudioOutput::Stream audio_;
  psx::movie::FrameAssembler assembler_;
  psx::movie::FrameDecoder decoder_;
  std::vector<uint16_t> codes_;
  std::vector<uint8_t> pixels24_; // decoded RGB888 frame, VRAM-sized
  std::vector<uint8_t> rgba_scratch_;
  std::array<int16_t, std::size_t{psx::audio::kXaMaxSectorFrames} * 2> xa_pcm_{};

  std::unique_ptr<psx::movie::SectorSource> disc_source_;
  psx::movie::SectorSource *source_ = nullptr;
  // Audio reads ahead of video on its own cursor; audio_raw_ holds the XA sector at audio_sector_.
  uint32_t audio_sector_ = 0;
  bool audio_loaded_ = false;
  std::vector<uint8_t> audio_raw_ = std::vector<uint8_t>(psx::cd::kRawSectorBytes);
  uint32_t base_lba_ = 0;
  uint32_t sector_count_ = 0;
  uint32_t sector_ = 0;
  long media_frames_ = 0; // audio sample-pairs queued: the media clock
  int xa_freq_ = psx::audio::kXaDefaultRateHz;
  int16_t xa_hist_[2][2] = {{0, 0}, {0, 0}};
  psx::movie::MovieClock clock_;
  psx::movie::MovieFieldGrid grid_{1.0}; // replaced at begin() with the display's field period
  psx::platform::HostClock::time_point origin_{};
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
  int max_frames_ = 0; // PSXPORT_FMV_MAXFRAMES; 0 plays the whole movie
  bool uncapped_ = false;
  bool finished_ = true;
  bool active_ = false;
  bool skipped_ = false;
  int frames_ = 0;
  bool start_held_at_begin_ = false; // a Start down when the movie opened needs a release first
};
