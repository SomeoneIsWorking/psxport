// psx::audio::HostAudioOutput: whether one session may be heard, and the S16 stereo host streams it opens.
#pragma once

#include "audio_queue_policy.h"

#include <cstdint>

struct SDL_AudioStream;

namespace psx::audio {

class HostAudioOutput {
public:
  static constexpr int kBytesPerFrame = AudioQueuePolicy::kBytesPerFrame;

  // One push-model stream on the default playback device, opened paused.
  class Stream {
  public:
    Stream() = default;
    ~Stream();
    Stream(const Stream &) = delete;
    Stream &operator=(const Stream &) = delete;
    Stream(Stream &&) = delete;
    Stream &operator=(Stream &&) = delete;

    // Opens at `rateHz`, replacing a stream at another rate; false (and closed) when the device cannot open.
    bool open(int rateHz);
    void close();
    bool isOpen() const {
      return stream_ != nullptr;
    }
    int rateHz() const {
      return rateHz_;
    }
    void put(const std::int16_t *stereo, int frames);
    void clear();
    void pause();
    void resume();
    // Bytes waiting for the device; -1 when closed.
    int queuedBytes() const;
    void setConsumptionRatio(double ratio);
    double consumptionRatio() const;

  private:
    SDL_AudioStream *stream_ = nullptr;
    int rateHz_ = 0;
  };

  // A process that runs several sessions lets only the claimed one reach the speakers; the guest's audio
  // advances either way.
  void setClaimed(bool claimed);
  bool claimed() const {
    return claimed_;
  }
  // A device may open: audio is not disabled, the run has a real window, and this session is claimed.
  bool mayOpen(bool windowed) const;

private:
  bool claimed_ = true;
};

} // namespace psx::audio
