// XaStream: the stream the CD command layer drives and the clip player, resampled to 44.1 kHz for the SPU.
#include "cd_drive_timing.h"
#include "cd_position.h"
#include "cdc_state.h"
#include "disc.h"
#include "spu_field_cadence.h"
#include "xa_adpcm.h"
#include "xa_push_pacing.h"
#include "xa_state.h"
#include <cstddef>
#include <lucent/log.h>
#include <stdint.h>

namespace psx::audio {

void XaStream::reset() {
  psx::cd::Disc *const wiredDisc = disc;
  const psx::cd::CdController *const wiredDrive = drive;
  *this = XaStream{};
  disc = wiredDisc;
  drive = wiredDrive;
}

void XaStream::bindSamplePull() {
  bound_ = this;
}

void XaStream::releaseSamplePull() const {
  if (bound_ == this) {
    bound_ = nullptr;
  }
}

void XaStream::resetHistory() {
  hist[0][0] = hist[0][1] = hist[1][0] = hist[1][1] = 0;
}

void XaStream::resetBuffers() {
  wr = 0;
  rd = 0.0;
  pulls = 0;
  sectors = 0;
  primed = 0;
  held_pulls = 0;
  resetHistory();
}

void XaStream::setLoc(uint32_t value) {
  lba = value;
  lucent::debug("xa", "setloc LBA {}", lba);
}

void XaStream::start() {
  if ((drive->mode & psx::cd::kModeXaAdpcm) == 0) {
    lucent::debug("xa", "ReadS without STRSND (mode={:02X}) is a data read", drive->mode);
    return;
  }
  resetBuffers();
  end_lba = 0;
  loop = 0;
  active = 1;
  lucent::debug("xa", "start @ LBA {} (mode={:02X})", lba, drive->mode);
}

void XaStream::stop() {
  if (active) {
    lucent::debug("xa", "stop @ LBA {}: {} pull(s) from the SPU, {} audio sector(s) decoded", lba, pulls, sectors);
  }
  active = 0;
}

void XaStream::stopForMode(uint8_t mode) {
  if (active) {
    active = 0;
    lucent::debug("xa", "stop (STRSND cleared, mode={:02X})", mode);
  }
}

void XaStream::play(uint8_t chan, uint32_t first, uint32_t last, bool looping) {
  lba = first;
  end_lba = last;
  clip_start = first;
  clip_chan = chan;
  loop = looping ? 1 : 0;
  resetBuffers();
  active = 1;
  lucent::debug("xa", "play clip chan={} [{}..{}] loop={}", chan, first, last, loop);
}

bool XaStream::isLooping() const {
  return active && loop;
}

bool XaStream::isActive() const {
  return active != 0;
}

// Pull mode: decode the next audio sector at the cursor, skipping interleaved sectors. Returns frames added.
int XaStream::decodeNextSector() {
  uint8_t raw[psx::cd::kRawSectorBytes];
  if (end_lba && lba > end_lba) {
    if (loop) {
      lucent::debug("xa", "loop chan={} back to {} (passed end {})", clip_chan, clip_start, end_lba);
      lba = clip_start;
      resetHistory();
    } else {
      lucent::debug("xa", "clip done @ LBA {} (end {})", lba, end_lba);
      active = 0;
      return 0;
    }
  }
  for (int guard = 0; guard < 64; guard++) {
    if (!disc->readRaw(lba, raw, sizeof raw) || !psx::cd::isMode2(raw)) {
      active = 0;
      return 0;
    }
    const psx::cd::SectorSubheader header = psx::cd::sectorSubheader(raw);
    lba++;
    // A clip is file 1 of its channel; an open-ended stream takes what the drive would route to the SPU.
    const bool pass = end_lba != 0 ? header.audio() && header.file == 1 && header.channel == clip_chan
                                   : drive->sectorRoute(raw) == psx::cd::CdcSectorRoute::XaAudio;
    if (pass) {
      const int frames = decodeAndStoreSector(raw, lba - 1);
      // A clip ends at end_lba only: EOF markers inside its range belong to interleaved files.
      if (header.endOfFile() && end_lba == 0) {
        lucent::debug("xa", "EOF @ LBA {}", lba - 1);
        active = 0;
      }
      return frames;
    }
    // Another channel's EOF ends that channel, not this stream (Spyro reads its music past others' EOFs).
    if (header.endOfFile()) {
      lucent::debug("xa", "skipping non-matching EOF @ LBA {} (file={} chan={})", lba - 1, header.file, header.channel);
    }
  }
  return 0;
}

int XaStream::pushAudioSector(const uint8_t *raw, uint32_t drive_lba) {
  push_mode = 1;
  const uint32_t backlog = wr - static_cast<uint32_t>(rd);
  if (backlog >= kXaRingFrames - kXaMaxSectorFrames) {
    const uint32_t dropped = backlog - (kXaRingFrames - kXaMaxSectorFrames);
    rd = static_cast<double>(static_cast<uint32_t>(rd) + dropped);
    const uint32_t streak = ++overflow_streak;
    if ((streak & (streak - 1)) == 0) {
      lucent::warn("xa",
                   "ring overflow (wr={} rd={}): dropped {} oldest frames for audio sector LBA {} ({} consecutive); "
                   "the SPU is not draining CD audio",
                   wr,
                   static_cast<uint32_t>(rd),
                   dropped,
                   drive_lba,
                   streak);
    }
  }
  return decodeAndStoreSector(raw, drive_lba);
}

int XaStream::decodeAndStoreSector(const uint8_t *raw, uint32_t drive_lba) {
  int16_t pcm[std::size_t{kXaMaxSectorFrames} * 2];
  const XaFormat format = decodeXaSector(raw, pcm, hist);
  const psx::cd::SectorSubheader header = psx::cd::sectorSubheader(raw);
  src_freq = format.rate;
  for (int i = 0; i < format.frames; i++) {
    const uint32_t idx = (wr + static_cast<uint32_t>(i)) % kXaRingFrames;
    ring[idx][0] = pcm[std::size_t{2} * i];
    ring[idx][1] = pcm[std::size_t{2} * i + 1];
  }
  wr += static_cast<uint32_t>(format.frames);
  sectors++;
  active = 1;
  lucent::debug("xasec",
                "LBA {} file={} chan={} submode={:02X} n={} freq={} (wr={} rd={})",
                drive_lba,
                header.file,
                header.channel,
                header.submode,
                format.frames,
                format.rate,
                wr,
                static_cast<uint32_t>(rd));
  return format.frames;
}

// Linear interpolation over a fractional read cursor; silence when not streaming.
void XaStream::pullSample(int32_t *samples) {
  if (!active) {
    samples[0] = samples[1] = 0;
    return;
  }
  pulls++;
  // Interpolation needs the next frame. A pushed stream never reads ahead of the drive.
  while (active && !push_mode && (wr - static_cast<uint32_t>(rd)) < 2) {
    if (decodeNextSector() == 0 && !active) {
      break;
    }
  }
  const uint32_t avail = wr - static_cast<uint32_t>(rd);
  if (avail < 1) {
    primed = 0;
    held_pulls = 0;
    samples[0] = samples[1] = 0;
    return;
  }
  if (push_mode && !primed) {
    if (!XaPushPacing::readyToPlay(avail, held_pulls)) {
      ++held_pulls;
      samples[0] = samples[1] = 0;
      return;
    }
    primed = 1;
  }
  // A pushed stream trims toward its backlog against drive/SPU clock drift.
  double ratio = static_cast<double>(src_freq) / static_cast<double>(SpuFieldCadence::kSampleRateHz);
  if (push_mode) {
    ratio *= XaPushPacing::rateScale(avail);
  }
  const uint32_t i0 = static_cast<uint32_t>(rd);
  const double frac = rd - static_cast<double>(i0);
  const uint32_t a = i0 % kXaRingFrames;
  const uint32_t b = (avail >= 2) ? (i0 + 1) % kXaRingFrames : a;
  for (int ch = 0; ch < 2; ch++) {
    const double v = static_cast<double>(ring[a][ch]) * (1.0 - frac) + static_cast<double>(ring[b][ch]) * frac;
    int32_t s = static_cast<int32_t>(v >= 0 ? v + 0.5 : v - 0.5);
    if (s < -32768) {
      s = -32768;
    } else if (s > 32767) {
      s = 32767;
    }
    samples[ch] = s;
  }
  rd += ratio;
}

} // namespace psx::audio

extern "C" void CDC_GetCDAudioSample(int32_t *samples) {
  psx::audio::XaStream *const bound = psx::audio::XaStream::bound();
  if (bound == nullptr) {
    samples[0] = samples[1] = 0;
    return;
  }
  bound->pullSample(samples);
}
