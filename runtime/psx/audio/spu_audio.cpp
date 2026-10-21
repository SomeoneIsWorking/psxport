#include "spu_audio.h"
#include "audio_queue_policy.h"
#include "config_vars.h"
#include "diagnostic_config.h"
#include "frame_pacer.h"
#include "game.h"
#include "gpu_device.h"
#include "host_clock.h"
#include "render_device.h"
#include "spu_beetle.h"
#include "xa_state.h"
#include <lucent/log.h>

#include <cstring>

namespace {

struct PcmSummary {
  uint32_t hash = 2166136261u;
  uint32_t nonzero = 0;
  uint32_t peak = 0;
};

PcmSummary summarizePcm(const int16_t *samples, int frames) {
  PcmSummary summary;
  const size_t count = static_cast<size_t>(frames > 0 ? frames : 0) * 2u;
  for (size_t i = 0; i < count; ++i) {
    const uint16_t value = static_cast<uint16_t>(samples[i]);
    summary.hash ^= value & 0xffu;
    summary.hash *= 16777619u;
    summary.hash ^= value >> 8;
    summary.hash *= 16777619u;
    if (samples[i] != 0) {
      ++summary.nonzero;
    }
    const int magnitude = samples[i] < 0 ? -static_cast<int>(samples[i]) : samples[i];
    if (magnitude > summary.peak) {
      summary.peak = static_cast<uint32_t>(magnitude);
    }
  }
  return summary;
}

double msSince(std::uint64_t fromNs, std::uint64_t toNs) {
  return static_cast<double>(toNs - fromNs) / 1e6;
}

constexpr lucent::Channel kSpuProf{"spuprof"};
constexpr lucent::Channel kAudioRate{"audiorate"};
constexpr lucent::Channel kAudioField{"audiofield"};

} // namespace

// Ten minutes, so a runaway run cannot fill the disk.
constexpr uint32_t kWavMaxBytes = 600u * SpuFieldCadence::kSampleRateHz * AudioQueuePolicy::kBytesPerFrame;

// PSXPORT_WAV captures the mixed SPU output independent of the device, so it works headless.
void SpuAudio::wavOpen(const char *path) {
  mWav.emplace(path, SpuFieldCadence::kSampleRateHz);
  mWavSynced = 0;
  if (!mWav->isOpen()) {
    mWav.reset();
    return;
  }
  lucent::info("spu_wav", "capturing SPU output -> {}", path);
}

void SpuAudio::wavReopen(const char *path) {
  mWav.reset();
  wavOpen(path);
}

void SpuAudio::init() {
  if (mState != 0) {
    return;
  }
  const std::string wavPath = psx::config::wav_capture_path();
  if (!wavPath.empty() && !mWav) {
    wavOpen(wavPath.c_str());
  }
  // An unclaimed session opens no second stream on the one device; frame() opens it once claimed.
  mState = 1;
  if (output_.claimed()) {
    openHostDevice();
  }
}

void SpuAudio::openHostDevice() {
  if (!output_.mayOpen(game->renderDevice.shown()) || !mStream.open(static_cast<int>(SpuFieldCadence::kSampleRateHz))) {
    mState = -1;
  }
}

// The SPU advances every field whether or not it is heard: guest logic waits on voice ends and the XA head.
void SpuAudio::frame() {
  const uint64_t traceField = ++mTraceField;
  if (mState == 1 && output_.claimed() && !mStream.isOpen()) {
    openHostDevice();
  }
  const bool pal = game->core.gpuDevice().displayArea().pal;
  const SpuFieldAdvance advance = mCadence.advance(psx::frame::displayFieldRate(pal));
  const uint32_t xaWrBefore = game->xa.wr;
  const double xaRdBefore = game->xa.rd;
  const uint32_t xaPullsBefore = game->xa.pulls;
  const uint32_t xaSectorsBefore = game->xa.sectors;
  int16_t *buf = mMixBuf;

  if (kSpuProf) {
    const std::uint64_t a = psx::platform::hostNowNs();
    if (mProfPrevNs) {
      mProfLoopMs += msSince(*mProfPrevNs, a);
    }
    psx::audio::spu::update(static_cast<int32_t>(advance.clocks));
    mProfAccumMs += msSince(a, psx::platform::hostNowNs());
    mProfPrevNs = a;
    if (++mProfN >= 60) {
      lucent::info("spu_prof",
                   "spu::update {:.4f} ms | full frame iter {:.4f} ms | spu share {:.1f}%",
                   mProfAccumMs / mProfN,
                   mProfLoopMs / mProfN,
                   mProfLoopMs > 0 ? 100.0 * mProfAccumMs / mProfLoopMs : 0.0);
      mProfAccumMs = 0;
      mProfLoopMs = 0;
      mProfN = 0;
    }
  } else {
    psx::audio::spu::update(static_cast<int32_t>(advance.clocks));
  }

  const int renderedFrames = psx::audio::spu::render(buf, static_cast<int>(advance.samples + kRenderSlack));
  int frames = renderedFrames;
  // A field the SPU rendered nothing for is a field of silence, so music and the sink still get one.
  if (frames <= 0) {
    frames = static_cast<int>(advance.samples);
    memset(buf, 0, static_cast<size_t>(frames) * 2 * sizeof(int16_t));
  }
  if (kAudioField) {
    const PcmSummary pcm = summarizePcm(buf, frames);
    lucent::debug(kAudioField,
                  "field={} pal={} expected_samples={} expected_clocks={} rendered_samples={} queued_samples={} "
                  "pcm_hash={:08X} pcm_nonzero={} pcm_peak={} xa_wr={} xa_rd={:.3f} xa_pulls={} xa_sectors={} "
                  "delta_wr={} delta_rd={:.3f} delta_pulls={} delta_sectors={}",
                  traceField,
                  pal ? 1 : 0,
                  advance.samples,
                  advance.clocks,
                  renderedFrames,
                  frames,
                  pcm.hash,
                  pcm.nonzero,
                  pcm.peak,
                  game->xa.wr,
                  game->xa.rd,
                  game->xa.pulls,
                  game->xa.sectors,
                  game->xa.wr - xaWrBefore,
                  game->xa.rd - xaRdBefore,
                  game->xa.pulls - xaPullsBefore,
                  game->xa.sectors - xaSectorsBefore);
  }

  if (psx::game::TitleAudio *titleAudio = game->runtime.audio(game->core)) {
    titleAudio->mixFrame(buf, frames);
  }

  if (mWav && mWav->dataBytes() < kWavMaxBytes) {
    mWav->append(buf, static_cast<uint32_t>(frames));
    // About once a second: the watchdog ends a killed run with _exit, which skips every destructor.
    if (mWav->dataBytes() - mWavSynced >= static_cast<uint32_t>(AudioQueuePolicy::kBytesPerSecond)) {
      mWavSynced = mWav->dataBytes();
      mWav->syncSizes();
    }
  }

  if (output_.claimed() && mState == 1 && mStream.isOpen()) {
    feedHost(buf, frames);
  }
}

// The device queue is steered toward AudioQueuePolicy's target by trimming its consumption rate; rendered
// samples are dropped only above the policy's hard limit.
void SpuAudio::feedHost(const int16_t *buf, int frames) {
  if (kAudioRate) {
    const std::uint64_t now = psx::platform::hostNowNs();
    if (!mRateT0Ns) {
      mRateT0Ns = now;
    }
    mRateSamp += frames;
    mRateCalls++;
    if (AudioQueuePolicy::shouldDrop(mStream.queuedBytes())) {
      mRateDrops++;
    }
    const double dt = msSince(*mRateT0Ns, now) / 1000.0;
    if (dt >= 2.0) {
      lucent::info("audio_rate",
                   "{:.0f} samples/s (want {}), {} calls/{:.1f}s, drops={}, backlog={}, level={:.0f}, ratio={:.5f}, "
                   "run reprimes={} drops={}",
                   static_cast<double>(mRateSamp) / dt,
                   SpuFieldCadence::kSampleRateHz,
                   mRateCalls,
                   dt,
                   mRateDrops,
                   mStream.queuedBytes(),
                   mSteer.level(),
                   mStream.consumptionRatio(),
                   mReprimes,
                   mDrops);
      mRateT0Ns = now;
      mRateSamp = 0;
      mRateCalls = 0;
      mRateDrops = 0;
    }
  }
  int queuedBytes = mStream.queuedBytes();
  if (AudioQueuePolicy::needsReprime(mStreamStarted, queuedBytes)) {
    mStream.pause();
    mStreamStarted = false;
    ++mReprimes;
    lucent::debug("audio", "[spu_audio] queue ran empty: re-priming");
  }
  if (AudioQueuePolicy::shouldDrop(queuedBytes)) {
    ++mDrops;
    return;
  }
  // Seconds of audio produced in a moment of fast-forward would be a shriek or a backlog.
  if (game->fastForwarding()) {
    return;
  }
  mStream.put(buf, frames);
  queuedBytes = mStream.queuedBytes();
  const double pacedSpeed = psx::frame::FramePacer::pacedSpeed(*game);
  if (mStreamStarted) {
    mStream.setConsumptionRatio(mSteer.observe(queuedBytes, pacedSpeed));
  } else if (AudioQueuePolicy::readyToStart(queuedBytes)) {
    mSteer.reset(queuedBytes);
    mStream.setConsumptionRatio(pacedSpeed);
    mStream.resume();
    mStreamStarted = true;
  }
  lucent::debug("audio", "[spu_audio] rendered {} frames, queued={} bytes", frames, queuedBytes);
}

void SpuAudio::reportQueue() const {
  lucent::info("playcheck", "audio: reprimes={} drops={}", mReprimes, mDrops);
}
