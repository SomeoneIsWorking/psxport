#pragma once

#include "spu_field_cadence.h"

#include <algorithm>

// Host playback buffering policy. The SPU still advances from the exact delivered display-field
// cadence; this policy only decides when already-rendered PCM is released to the audio device and
// how fast the device consumes it.
//
// The queue is held at one target level. Playback starts once the target is primed, re-primes after
// a confirmed empty queue, and in between the device's consumption rate is trimmed toward the target
// so a burst or a hitch is absorbed without a drop or a gap. Rendered PCM is dropped only above a
// hard limit no steady producer reaches.
class AudioQueuePolicy {
public:
  static constexpr int kSampleRateHz = static_cast<int>(SpuFieldCadence::kSampleRateHz);
  static constexpr int kChannels = 2;
  static constexpr int kBytesPerSample = 2;
  // Four NTSC fields of producer gap (a 30 Hz title missing two frames) plus one device period:
  // SDL's default 1024 frames, 23 ms at 44.1 kHz, which PipeWire resamples into its 1024-frame
  // 48 kHz quantum.
  static constexpr int kPlaybackTargetMs = 90;
  static constexpr int kQueueLimitMs = 2 * kPlaybackTargetMs;
  // Largest consumption-rate trim: 8.6 cents, below what a listener hears as pitch.
  static constexpr double kMaxRateTrim = 0.005;

  static constexpr int kBytesPerFrame = kChannels * kBytesPerSample;
  static constexpr int kBytesPerSecond = kSampleRateHz * kBytesPerFrame;
  static constexpr int kPlaybackTargetBytes = (kBytesPerSecond * kPlaybackTargetMs) / 1000;
  static constexpr int kQueueLimitBytes = (kBytesPerSecond * kQueueLimitMs) / 1000;

  [[nodiscard]] static constexpr bool readyToStart(int queuedBytes) {
    return queuedBytes >= kPlaybackTargetBytes;
  }

  [[nodiscard]] static constexpr bool shouldDrop(int queuedBytes) {
    return queuedBytes >= kQueueLimitBytes;
  }

  [[nodiscard]] static constexpr bool needsReprime(bool started, int queuedBytes) {
    return started && queuedBytes == 0;
  }

  // Device consumption ratio for a smoothed queue level: 1 at the target, the full trim at an empty
  // queue (slower) or at the drop limit (faster).
  [[nodiscard]] static constexpr double consumptionRatio(double smoothedQueuedBytes) {
    const double error = (smoothedQueuedBytes - kPlaybackTargetBytes) / kPlaybackTargetBytes;
    return 1.0 + kMaxRateTrim * std::clamp(error, -1.0, 1.0);
  }
};

// Smooths the queue level over about one second of puts before it steers the consumption ratio, so
// the device-period sawtooth in each reading never reaches the pitch.
class AudioQueueSteer {
public:
  static constexpr double kSmoothing = 1.0 / 60.0;

  void reset(int queuedBytes) {
    mLevel = queuedBytes;
  }

  // One reading per put, taken after the put. Returns the ratio to hand the device stream.
  double observe(int queuedBytes) {
    mLevel += (queuedBytes - mLevel) * kSmoothing;
    return AudioQueuePolicy::consumptionRatio(mLevel);
  }

  [[nodiscard]] double level() const {
    return mLevel;
  }

private:
  double mLevel = AudioQueuePolicy::kPlaybackTargetBytes;
};
