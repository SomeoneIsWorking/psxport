// psx::audio::XaStream: the XA-ADPCM CD-audio and voice-clip streamer, one per Game. Beetle spu.c pulls its
// PCM through the context-free CDC_GetCDAudioSample, which reads the instance bound by bindSamplePull.
#pragma once
#include "xa_adpcm.h"

#include <stdint.h>

namespace psx::cd {
class CdController;
class Disc;
} // namespace psx::cd

namespace psx::audio {

// About 1.7 s of 37800 Hz audio, to absorb drive burst jitter.
inline constexpr uint32_t kXaRingFrames = 65536;

class XaStream {
public:
  // Power-on defaults; keeps the disc and drive wiring.
  void reset();
  void bindSamplePull();
  void releaseSamplePull() const;
  static XaStream *bound() {
    return bound_;
  }
  // Decode a sector the drive routed to the SPU; the drive owns the cursor from then on. A full ring drops its
  // oldest frames, as the SPU CD buffer overflows without stopping the drive. Returns the frames decoded.
  int pushAudioSector(const uint8_t *raw, uint32_t drive_lba);
  // The stream a ReadS starts, read ahead from `lba` until the drive pushes its first sector.
  void setLoc(uint32_t lba);
  void start();
  void stop();
  // A Setmode that clears STRSND stops the stream.
  void stopForMode(uint8_t mode);
  // Streams channel `chan` of file 1 over [first, last], restarting at `first` past the end when `looping`.
  void play(uint8_t chan, uint32_t first, uint32_t last, bool looping);
  bool isLooping() const;
  bool isActive() const;
  // One stereo pair per 44.1 kHz output sample, resampled from the XA source rate.
  void pullSample(int32_t *samples);

  int active = 0;
  int push_mode = 0;    // the drive decodes into this ring; pulls must not read ahead
  uint32_t lba = 0;     // next sector a pull reads
  uint32_t end_lba = 0; // clip end (0 = open-ended)
  uint32_t clip_start = 0;
  uint8_t clip_chan = 0;
  int loop = 0;
  int16_t ring[kXaRingFrames][2] = {};
  uint32_t wr = 0; // frames written, monotonic
  double rd = 0.0; // frames read, monotonic and fractional
  int16_t hist[2][2] = {};
  int src_freq = kXaDefaultRateHz;
  // pulls == 0 means the SPU never asked; sectors == 0 that the disc gave nothing.
  uint32_t pulls = 0;
  uint32_t sectors = 0;
  uint32_t overflow_streak = 0;
  int primed = 0;          // push mode: playback released since the stream (re)started
  uint32_t held_pulls = 0; // push mode: pulls answered with silence while not primed
  psx::cd::Disc *disc = nullptr;
  const psx::cd::CdController *drive = nullptr;

private:
  inline static XaStream *bound_ = nullptr;

  void resetHistory();
  void resetBuffers();
  int decodeNextSector();
  int decodeAndStoreSector(const uint8_t *raw, uint32_t drive_lba);
};

} // namespace psx::audio

extern "C" void CDC_GetCDAudioSample(int32_t *samples);
