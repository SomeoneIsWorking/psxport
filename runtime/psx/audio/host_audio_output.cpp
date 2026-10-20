#include "host_audio_output.h"

#include "config_vars.h"

#include <lucent/log.h>
#include <SDL3/SDL.h>

namespace psx::audio {

void HostAudioOutput::setClaimed(bool claimed) {
  if (claimed_ == claimed) {
    return;
  }
  claimed_ = claimed;
  lucent::info("audio", "host output {} by this session", claimed ? "claimed" : "released");
}

bool HostAudioOutput::mayOpen(bool windowed) const {
  return claimed_ && !psx::config::no_audio() && windowed;
}

HostAudioOutput::Stream::~Stream() {
  close();
}

bool HostAudioOutput::Stream::open(int rateHz) {
  if (stream_ != nullptr && rateHz_ == rateHz) {
    return true;
  }
  close();
  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    lucent::error("audio", "SDL_InitSubSystem(AUDIO) failed: {}; audio disabled", SDL_GetError());
    return false;
  }
  SDL_AudioSpec spec{};
  spec.freq = rateHz;
  spec.format = SDL_AUDIO_S16;
  spec.channels = AudioQueuePolicy::kChannels;
  stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
  if (stream_ == nullptr) {
    lucent::error("audio", "SDL_OpenAudioDeviceStream({} Hz) failed: {}; audio disabled", rateHz, SDL_GetError());
    return false;
  }
  rateHz_ = rateHz;
  SDL_AudioSpec device;
  int deviceFrames = 0;
  if (SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(stream_), &device, &deviceFrames)) {
    lucent::info("audio", "{} Hz stream on a {} Hz device, {} frames per period", rateHz, device.freq, deviceFrames);
  }
  return true;
}

void HostAudioOutput::Stream::close() {
  if (stream_ != nullptr) {
    SDL_DestroyAudioStream(stream_);
    stream_ = nullptr;
    rateHz_ = 0;
  }
}

void HostAudioOutput::Stream::put(const std::int16_t *stereo, int frames) {
  if (stream_ != nullptr) {
    SDL_PutAudioStreamData(stream_, stereo, frames * kBytesPerFrame);
  }
}

void HostAudioOutput::Stream::clear() {
  if (stream_ != nullptr) {
    SDL_ClearAudioStream(stream_);
  }
}

void HostAudioOutput::Stream::pause() {
  if (stream_ != nullptr) {
    SDL_PauseAudioStreamDevice(stream_);
  }
}

void HostAudioOutput::Stream::resume() {
  if (stream_ != nullptr) {
    SDL_ResumeAudioStreamDevice(stream_);
  }
}

int HostAudioOutput::Stream::queuedBytes() const {
  return stream_ != nullptr ? SDL_GetAudioStreamQueued(stream_) : -1;
}

void HostAudioOutput::Stream::setConsumptionRatio(double ratio) {
  if (stream_ != nullptr) {
    SDL_SetAudioStreamFrequencyRatio(stream_, static_cast<float>(ratio));
  }
}

double HostAudioOutput::Stream::consumptionRatio() const {
  return stream_ != nullptr ? SDL_GetAudioStreamFrequencyRatio(stream_) : 1.0;
}

} // namespace psx::audio
