#pragma once

// Host playback buffering policy. The SPU still advances from the exact delivered display-field
// cadence; this policy only decides when already-rendered PCM is released to the audio device.
//
// A render-driven producer naturally submits two fields in a burst for a 30 Hz game. Starting an
// empty device and capping it at four fields made any longer render/load hitch an unavoidable
// underrun, then discarded the catch-up PCM (including sound-effect tails). Prime a bounded cushion
// before playback and re-prime after a confirmed empty queue instead.
class AudioQueuePolicy {
public:
  static constexpr int kSampleRateHz = 44100;
  static constexpr int kChannels = 2;
  static constexpr int kBytesPerSample = 2;
  static constexpr int kPlaybackCushionMs = 180;
  static constexpr int kQueueLimitMs = 360;

  static constexpr int bytesForMilliseconds(int milliseconds) {
    return (kSampleRateHz * kChannels * kBytesPerSample * milliseconds) / 1000;
  }

  static constexpr int kPlaybackCushionBytes =
      (kSampleRateHz * kChannels * kBytesPerSample * kPlaybackCushionMs) / 1000;
  static constexpr int kQueueLimitBytes = (kSampleRateHz * kChannels * kBytesPerSample * kQueueLimitMs) / 1000;

  [[nodiscard]] static constexpr bool readyToStart(int queuedBytes) {
    return queuedBytes >= kPlaybackCushionBytes;
  }

  [[nodiscard]] static constexpr bool shouldDrop(int queuedBytes) {
    return queuedBytes >= kQueueLimitBytes;
  }

  [[nodiscard]] static constexpr bool needsReprime(bool started, int queuedBytes) {
    return started && queuedBytes == 0;
  }
};
