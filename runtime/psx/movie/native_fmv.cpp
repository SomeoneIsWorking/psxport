#include "native_fmv.h"

#include "config_vars.h"
#include "core.h"
#include "diagnostic_config.h"
#include "field_rate.h"
#include "game.h"
#include "host_input.h"
#include "number_text.h"
#include "render_device.h"
#include "watchdog.h"
#include <algorithm>
#include <chrono>
#include <limits>
#include <lucent/log.h>
#include <optional>
#include <thread>

namespace {

// How far ahead of its play time audio is queued.
constexpr double kAudioLeadMs = 1000.0;

class DiscSectors final : public psx::movie::SectorSource {
public:
  explicit DiscSectors(psx::cd::Disc &disc) : disc_(disc) {}
  bool readRaw(uint32_t lba, uint8_t *out) override {
    return disc_.readRaw(lba, out, psx::cd::kRawSectorBytes);
  }

private:
  psx::cd::Disc &disc_;
};

} // namespace

// The movie is a native RGBA picture pillarboxed 4:3, not a VRAM upload: the renderer composites only native
// submits. 24bpp is what retail movies were authored for (Toy Story 2's overlay asks for its depth 3).
void Fmv::buildFrame(psx::present::PixelSize size) {
  game->display.buildHostPicture(rgba_.data(), size, 1.0f);
  // Movie frames are progress, not main-present readiness.
  psx::platform::Watchdog::progress();
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
  uint8_t raw[psx::cd::kRawSectorBytes];
  const uint32_t end = std::min(sector_count_, psx::movie::MovieClock::kLearnSectors);
  for (uint32_t sector = 0; sector < end && !clock_.learned(); ++sector) {
    if (!source_->readRaw(base_lba_ + sector, raw)) {
      break;
    }
    psx::movie::SectorFacts facts;
    if (psx::cd::sectorSubheader(raw).audio()) {
      facts.audio = psx::audio::xaSectorFormat(raw);
    } else if (const std::optional<psx::movie::VideoChunk> chunk = psx::movie::videoChunk(raw)) {
      facts.videoFrame = chunk->frame;
    }
    clock_.learn(facts);
  }
  lucent::info("fmv",
               "clock: {:.4f} ms per sector, frame slot {} sectors (0 = due at its last chunk)",
               clock_.sectorMs(),
               clock_.slotSectors());
}

// Headless and an unclaimed session open no device.
void Fmv::audioOpen(int freq) {
  if (!game->spu_audio.output().mayOpen(game->renderDevice.shown())) {
    return;
  }
  if (audio_.isOpen() && audio_.rateHz() == freq) {
    audio_.clear();
    return;
  }
  if (audio_.open(freq)) {
    audio_.resume();
  }
}

std::optional<double> Fmv::audioLagMs() const {
  const std::optional<uint32_t> first = clock_.firstAudioSector();
  const int queuedBytes = audio_.queuedBytes();
  if (!first || queuedBytes < 0 || xa_freq_ <= 0) {
    return std::nullopt;
  }
  const long queuedFrames = queuedBytes / psx::audio::HostAudioOutput::kBytesPerFrame;
  const double playedMs = static_cast<double>(media_frames_ - queuedFrames) * 1000.0 / xa_freq_;
  return elapsedMs() - (clock_.sectorDeliveredMs(*first) + playedMs);
}

// The frame's pad service already resolved host, replay and control input into `buttons`; polling here would
// make a skip unreplayable. The skip is a level, because that service consumed the edge; a Start already down
// at begin needs a release first, as the original player required. Uncapped (PSXPORT_FMV_FPS=0) never waits.
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
    std::this_thread::sleep_until(origin_ + std::chrono::duration_cast<psx::platform::HostClock::duration>(
                                                std::chrono::duration<double, std::milli>(wakeMs)));
  }
  feedAudio(elapsedMs());
  return pressed;
}

double Fmv::elapsedMs() const {
  return psx::platform::msBetween(origin_, psx::platform::HostClock::now());
}

// The first XA sector is queued when the drive delivers it, which starts the audio clock; each later
// one kAudioLeadMs ahead of that, so a host stall shorter than the lead does not starve the device.
std::optional<double> Fmv::nextAudioQueueMs() {
  while (!audio_loaded_ && audio_sector_ < sector_count_) {
    if (!source_->readRaw(base_lba_ + audio_sector_, audio_raw_.data())) {
      audio_sector_ = sector_count_;
      break;
    }
    if (psx::cd::sectorSubheader(audio_raw_.data()).audio()) {
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
    const psx::audio::XaFormat format = psx::audio::decodeXaSector(audio_raw_.data(), xa_pcm_.data(), xa_hist_);
    xa_freq_ = format.rate;
    if (media_frames_ == 0) {
      audioOpen(xa_freq_);
    }
    audio_.put(xa_pcm_.data(), format.frames);
    media_frames_ += format.frames;
    audio_loaded_ = false;
    ++audio_sector_;
  }
}

void Fmv::ensureScratch() {
  if (codes_.empty()) {
    codes_.resize(psx::movie::kMaxCodes);
    rgba_.resize(psx::movie::kMaxFramePixels * 4);
  }
}

bool Fmv::begin(uint32_t lba, uint32_t size_bytes) {
  disc_source_ = std::make_unique<DiscSectors>(game->disc);
  return begin(*disc_source_, lba, size_bytes);
}

bool Fmv::begin(psx::movie::SectorSource &source, uint32_t lba, uint32_t size_bytes) {
  game->mdec.power();
  decoder_.start(game->mdec);
  source_ = &source;
  ensureScratch();

  base_lba_ = lba;
  sector_count_ = (size_bytes + psx::cd::kDataSectorBytes - 1) / psx::cd::kDataSectorBytes;
  sector_ = 0;
  audio_sector_ = 0;
  audio_loaded_ = false;
  assembler_.reset();
  media_frames_ = 0;
  xa_freq_ = psx::audio::kXaDefaultRateHz;
  xa_hist_[0][0] = xa_hist_[0][1] = xa_hist_[1][0] = xa_hist_[1][1] = 0;
  frames_ = 0;
  skipped_ = false;
  finished_ = false;
  active_ = true;

  // Pacing is what the movie does, so headless paces too; PSXPORT_FMV_FPS=0 asks for the fast-forward.
  uncapped_ = false;
  {
    const std::string fps = psx::config::movie_fps();
    if (!fps.empty()) {
      uncapped_ = psx::config::parseInt(fps).value_or(0) == 0;
    }
  }
  if (!uncapped_ && game->fastForwarding()) {
    uncapped_ = true;
    lucent::info("fmv", "uncapped: the run is fast-forwarding (resume replay or host pre-roll)");
  }
  max_frames_ = static_cast<int>(psx::config::movie_max_frames());

  // A Start held as the movie begins does not skip it; see pace().
  start_held_at_begin_ = (game->pad.buttons & psx::input::kButtonStart) == 0;
  game->pad.resetButtonEdges(game->pad.buttons);
  lucent::info("fmv", "begin: LBA {}, {} bytes ({} sectors)", lba, size_bytes, sector_count_);
  learnLayout();
  grid_ = psx::movie::MovieFieldGrid(
      psx::frame::fieldIntervalMs(psx::frame::fieldRateMilliHz(game->timing.displayFieldRate()), 1, 1));
  audio_lag_ = {};
  intervals_at_begin_ = game->perf.presentSeries().intervals;
  missed_at_begin_ = game->perf.presentSeries().missed;
  origin_ = psx::platform::HostClock::now();
  return true;
}

void Fmv::step() {
  if (finished_) {
    return;
  }
  uint8_t raw[psx::cd::kRawSectorBytes];
  while (sector_ < sector_count_) {
    if (!source_->readRaw(base_lba_ + sector_, raw)) {
      break;
    }
    const uint32_t at = sector_++;
    // Audio is read by feedAudio on its own cursor.
    const std::optional<psx::movie::VideoChunk> video = psx::movie::videoChunk(raw);
    if (!video) {
      continue;
    }
    const std::optional<psx::movie::FrameAssembler::Frame> frame = assembler_.add(*video, raw);
    if (!frame) {
      continue;
    }
    const int ncodes = psx::movie::decodeBsFrame(frame->payload, frame->size, codes_);
    if (ncodes > 0 && decoder_.decode(game->mdec,
                                      {codes_.data(), static_cast<std::size_t>(ncodes)},
                                      frame->size,
                                      psx::movie::FramePixels::Rgba32,
                                      rgba_.data()) > 0) {
      const psx::movie::MovieFieldGrid::Show show = grid_.show(clock_.frameDueMs({frame->number, at}));
      buildFrame(frame->size);
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
                    frame->number,
                    frame->size.width,
                    frame->size.height,
                    frame->payload.size(),
                    ncodes,
                    show.atMs,
                    show.heldFields,
                    lag.value_or(0.0));
      if (max_frames_ && frames_ >= max_frames_) {
        endAtEndOfStream();
        return;
      }
    }
    return;
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

// Every exit blacks the display once, so no movie residue shows under a front end still loading its 2D layer.
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
               static_cast<double>(media_frames_) / xa_freq_,
               xa_freq_,
               game->perf.presentSeries().intervals - intervals_at_begin_,
               game->perf.presentSeries().missed - missed_at_begin_,
               audio_lag_.first,
               audio_lag_.last,
               audio_lag_.min,
               audio_lag_.max);
  audio_.clear();
  game->display.build(psx::present::PresentCompletion::Transition);
  game->display.show();
}

// The movie ends inside a pad frame, so the guest cannot act on the press's edge; no consumption wait is needed.
void Fmv::endWithSkip() {
  if (finished_) {
    return;
  }
  skipped_ = true;
  endAtEndOfStream();
  lucent::info("fmv", "skipped by Start at frame {}", frames_);
}

// No frame driver services the pad while this blocks, so it pumps host input for the skip.
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
