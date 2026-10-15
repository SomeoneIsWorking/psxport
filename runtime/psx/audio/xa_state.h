// xa_state.h — psx::audio::XaStream: the native XA-ADPCM CD-audio and voice-clip streamer, one per Game. The
// decoded PCM feeds the Game's SPU through Beetle's context-free CDC_GetCDAudioSample pull, which reads the
// instance bound by bindSamplePull.
#pragma once
#include <stdint.h>

namespace psx::cd {
class Disc;
}

namespace psx::audio {

// About 1.7 s of 37800 Hz audio; a smaller ring turned normal drive burst jitter into drops (Vagrant Story intro).
inline constexpr uint32_t kXaRingFrames = 65536;

class XaStream {
public:
  // Power-on defaults (src_freq=37800); keeps the disc wiring.
  void reset();
  // Bind this instance for Beetle spu.c's sample pull; set per frame step from the explicit Core.
  void bindSamplePull();
  // Clears the sample-pull binding if it still names this instance.
  void releaseSamplePull() const;
  // Decode one already-read raw Mode2 XA sector into the ring (push mode); never touches the disc cursor. A full
  // ring drops its oldest frames, because hardware's SPU CD buffer overflows without stopping the drive. Returns
  // the frames decoded.
  int pushAudioSector(const uint8_t *raw, uint32_t drive_lba);
  // The pull-mode streamer the CD command layer drives (stock libcd, or a title's own CdControl).
  void setMode(uint8_t value);
  void setFilter(uint8_t file, uint8_t chan);
  void setLoc(uint8_t amm, uint8_t ass, uint8_t asect);
  void start();
  void stop();
  // The read head while streaming; false when idle.
  bool playLba(uint32_t *head) const;
  // Streams channel `chan` over [first, last] (CHD LBAs), restarting at `first` past the end when `looping`.
  void play(uint8_t chan, uint32_t first, uint32_t last, bool looping);
  bool isLooping() const;
  bool isActive() const;
  // One stereo pair per 44.1 kHz output sample, resampled from the XA source rate.
  void pullSample(int32_t *samples);

  int active = 0;    // ReadS streaming with XA-ADPCM enabled
  int push_mode = 0; // the CDC drive decodes into this ring; pulls must not self-fetch sectors
  uint32_t lba = 0;  // next CHD sector to read
  uint8_t mode = 0;  // last Setmode
  int filter_set = 0;
  uint8_t filter_file = 0;
  uint8_t filter_chan = 0;
  uint32_t end_lba = 0;    // clip end LBA (0 = open-ended)
  uint32_t clip_start = 0; // clip start LBA (idempotency + loop restart)
  uint8_t clip_chan = 0;
  int loop = 0;
  int16_t ring[kXaRingFrames][2] = {}; // decoded interleaved S16
  uint32_t wr = 0;                     // total frames written (monotonic)
  double rd = 0.0;                     // total frames read (monotonic, fractional)
  int16_t hist[2][2] = {};             // XA IIR history, persists across sectors
  int src_freq = 37800;
  // Census for the active stream: pulls == 0 means the SPU never asked, sectors == 0 that the disc gave nothing.
  uint32_t pulls = 0;
  uint32_t sectors = 0;
  uint32_t overflow_streak = 0; // consecutive sectors on a full ring; log throttle only
  int primed = 0;               // push mode: playback released since the stream (re)started
  uint32_t held_pulls = 0;      // push mode: pulls answered with silence while not primed
  psx::cd::Disc *disc = nullptr;

private:
  void resetBuffers();
  int decodeNextSector();
  int decodeAndStoreSector(const uint8_t *raw, uint32_t drive_lba);
};

} // namespace psx::audio

// Beetle spu.c's CD-audio source: one stereo pair per 44.1 kHz output sample from the bound instance.
extern "C" void CDC_GetCDAudioSample(int32_t *samples);
