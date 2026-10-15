// xa_stream.cpp — native XA-ADPCM CD audio: the streamer the CD command layer drives (Setmode, Setfilter,
// Setloc, ReadS/ReadN, Stop/Pause) and the clip player, decoded on demand as Beetle's spu.c pulls
// CDC_GetCDAudioSample and resampled from 37800/18900 Hz to 44100 Hz.
#include "disc.h"
#include "fmv_decode.h"
#include "xa_push_pacing.h"
#include "xa_state.h"
#include <lucent/log.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
namespace {

// Beetle spu.c pulls through the context-free CDC_GetCDAudioSample, so that pull alone reads a bound instance.
psx::audio::XaStream *s_bound = nullptr;

} // namespace

namespace psx::audio {

void XaStream::reset() {
  psx::cd::Disc *const wiredDisc = disc;
  *this = XaStream{};
  disc = wiredDisc;
}

void XaStream::bindSamplePull() {
  s_bound = this;
}

void XaStream::releaseSamplePull() const {
  if (s_bound == this) {
    s_bound = nullptr;
  }
}

void XaStream::resetBuffers() {
  wr = 0;
  rd = 0.0;
  pulls = 0;
  sectors = 0;
  primed = 0;
  held_pulls = 0;
  hist[0][0] = hist[0][1] = hist[1][0] = hist[1][1] = 0;
}

void XaStream::setMode(uint8_t value) {
  mode = value;
  // If XA-ADPCM output (bit6) was turned off mid-stream, stop feeding the SPU.
  if (active && !(value & 0x40)) {
    active = 0;
    lucent::debug("xa", "stop (XA bit cleared, mode={:02X})", value);
  }
}

void XaStream::setFilter(uint8_t file, uint8_t chan) {
  filter_set = 1;
  filter_file = file;
  filter_chan = chan;
  lucent::debug("xa", "setfilter file={} chan={}", file, chan);
}

// Setloc params: amm,ass,asect in BCD; MSF includes the 2s (150-frame) lead-in, so the
// CHD/ISO LBA = msf_frames - 150 (matches disc.cpp's by-LBA convention).
void XaStream::setLoc(uint8_t amm, uint8_t ass, uint8_t asect) {
  int mm = (amm >> 4) * 10 + (amm & 0xF);
  int ss = (ass >> 4) * 10 + (ass & 0xF);
  int ff = (asect >> 4) * 10 + (asect & 0xF);
  lba = (uint32_t)((mm * 60 + ss) * 75 + ff - 150);
  lucent::debug("xa", "setloc {:02X}:{:02X}:{:02X} -> LBA {}", amm, ass, asect, lba);
}

void XaStream::start() {
  if (!(mode & 0x40)) { // ReadS without XA-ADPCM enabled = data read (not our concern)
    lucent::debug("xa", "ReadS but XA bit not set (mode={:02X}) - ignoring", mode);
    return;
  }
  resetBuffers();
  end_lba = 0;
  loop = 0; // open-ended CdControl stream: no clip end -> EOF terminates it
  active = 1;
  lucent::debug("xa", "START streaming @ LBA {} (mode={:02X} filter={})", lba, mode, filter_set);
}

void XaStream::stop() {
  // Report the census, not just the position. A stream that stops having decoded nothing is the
  // silent-audio symptom, and `pulls` says WHICH side failed (see XaStream::pulls).
  if (active) {
    lucent::debug("xa", "STOP @ LBA {} — {} pull(s) from the SPU, {} audio sector(s) decoded", lba, pulls, sectors);
  }
  active = 0;
}

// ---- clip player: one channel over [start..end], looping back to start when asked ----
void XaStream::play(uint8_t chan, uint32_t first, uint32_t last, bool looping) {
  filter_set = 1;
  filter_file = 1;
  filter_chan = chan; // game always uses file 1
  mode = 0xC8;        // Speed | ADPCM | SF-filter (as the game sets)
  lba = first;
  end_lba = last;
  clip_start = first;
  clip_chan = chan;
  loop = looping ? 1 : 0;
  resetBuffers();
  active = 1;
  lucent::debug("xa", "PLAY clip chan={} [{}..{}] loop={}", chan, first, last, loop);
}

bool XaStream::isLooping() const {
  return active && loop;
}
bool XaStream::isActive() const {
  return active != 0;
}

bool XaStream::playLba(uint32_t *head) const {
  if (!active) {
    return false;
  }
  if (head) {
    *head = lba; // the next sector to read is the physical read-head position
  }
  return true;
}

// Decode the next audio sector into the ring. Skips interleaved non-audio sectors (advancing
// the LBA); stops the stream at end-of-file / non-Mode2 / read failure. Returns frames added.
int XaStream::decodeNextSector() {
  uint8_t raw[2352];
  // Past the clip's end LBA a looping clip restarts and a one-shot one stops.
  if (end_lba && lba > end_lba) {
    if (loop) {
      lucent::debug("xa",
                    "LOOP chan={} back to {} (passed end {}, span={} sectors)",
                    clip_chan,
                    clip_start,
                    end_lba,
                    end_lba - clip_start);
      lba = clip_start;
      hist[0][0] = hist[0][1] = hist[1][0] = hist[1][1] = 0;
    } else {
      lucent::debug("xa", "clip done @ LBA {} (end {})", lba, end_lba);
      active = 0;
      return 0;
    }
  }
  for (int guard = 0; guard < 64; guard++) { // bounded scan past any interleaved data sectors
    if (!disc->readRaw(lba, raw, 2352)) {
      active = 0;
      return 0;
    }
    uint8_t modebyte = raw[15]; // CD header: 12 sync + min/sec/frame/MODE
    if (modebyte != 2) {
      active = 0;
      return 0;
    } // ran off the Mode2 stream
    uint8_t file = raw[16], chan = raw[17], submode = raw[18];
    lba++;
    int is_audio = (submode & 0x04) != 0; // submode bit2 = ADPCM audio sector
    int eof = (submode & 0x80) != 0;      // submode bit7 = end-of-file
    int pass = is_audio;
    if (pass && filter_set && (mode & 0x08)) { // SF filter: match subheader file+channel
      pass = (file == filter_file && chan == filter_chan);
    }
    if (pass) {
      int16_t pcm[4032 * 2]; // mono sectors yield up to 4032 frames (see psx::movie::decodeXaSector)
      int freq = src_freq;
      int n = psx::movie::decodeXaSector(raw, pcm, hist, &freq);
      src_freq = freq;
      for (int i = 0; i < n; i++) {
        uint32_t idx = (wr + (uint32_t)i) % kXaRingFrames;
        ring[idx][0] = pcm[std::size_t{2} * i];
        ring[idx][1] = pcm[std::size_t{2} * i + 1];
      }
      wr += (uint32_t)n;
      sectors++;
      lucent::debug("xasec",
                    " sector LBA {} file={} chan={} submode={:02X} n={} freq={} (wr={} rd={})",
                    lba - 1,
                    file,
                    chan,
                    submode,
                    n,
                    freq,
                    wr,
                    (uint32_t)rd);
      // EOF (submode bit7) ends only an OPEN-ENDED stream. A BOUNDED clip ([start..end], e.g. the
      // looping area music) ends strictly at end_lba (handled above): EOF markers inside the range
      // belong to OTHER files/channels interleaved in the same stream and must NOT cut our clip.
      if (eof && !end_lba) {
        lucent::debug("xa", "EOF @ LBA {}", lba - 1);
        active = 0;
      }
      return n;
    }
    // An EOF on a NON-matching (other file/channel's) sector belongs to THAT stream, not ours: a
    // real drive skips it and keeps playing (the narration/voice file that ends mid-range, e.g. at
    // LBA 95338 of Tomba!2's [84515..97979] chan4 music clip, must not cut the music). Terminating
    // here also killed every OPEN-ENDED CdControl stream at the first foreign EOF: Spyro's sound
    // driver sets XA mode + a file/chan filter and reads without a Setloc — it scans forward and
    // polls GetlocL — so the stream started at LBA 0 and died at the first other-channel EOF 17
    // sectors later, leaving no music and a DC-only sink. Only our OWN channel's EOF ends the
    // stream (handled above, for open-ended streams only); a bounded clip ends at end_lba.
    if (eof) {
      lucent::debug(
          "xa", "skipping non-matching EOF @ LBA {} (file={} chan={}) — still streaming", lba - 1, file, chan);
    }
  }
  return 0; // 64 consecutive non-passing sectors: give up this pump, try again next sample
}

// Push mode: the CDC drive model (cdc_native.cpp) has already read the sector from the disc and
// decided it is XA-ADPCM audio under beetle's routing rule; decode it into the ring here. The disc
// cursor belongs to the drive — this NEVER touches lba. `drive_lba` is diagnostic-only.
// Returns frames decoded (>0) or 0 when the sector is not decodable audio.
//
// The ring overflows when audio is produced faster than the SPU drains it. On hardware the decoded
// XA audio lands in the SPU's own CD buffer, and when THAT overflows the SPU drops/glitches samples
// while the drive keeps spinning at its selected speed — so an overflow discards the OLDEST decoded
// audio (the oldest is the least useful delay), takes the new sector, and never stops the disc. A
// hold-for-retry would instead stall the sectors delivering the guest's VIDEO from the same stream.
int XaStream::pushAudioSector(const uint8_t *raw, uint32_t drive_lba) {
  if (!push_mode || raw[15] != 2 || !(raw[18] & 0x04)) {
    return 0;
  }
  const uint32_t backlog = (uint32_t)(wr - (uint32_t)rd);
  if (backlog >= kXaRingFrames - 4096) {
    const uint32_t dropped = backlog - (kXaRingFrames - 4096);
    rd = (double)((uint32_t)rd + dropped); // oldest audio first; the drive never stops
    const uint32_t streak = ++overflow_streak;
    if ((streak & (streak - 1)) == 0) { // report at 1,2,4,8... — every doubling
      lucent::warn("xa",
                   "ring OVERFLOW (wr={} rd={}) — dropped {} oldest frames, took audio sector LBA {} "
                   "anyway ({} consecutive); the SPU is not draining CD audio",
                   wr,
                   (uint32_t)rd,
                   dropped,
                   drive_lba,
                   streak);
    }
    return decodeAndStoreSector(raw, drive_lba);
  }
  return decodeAndStoreSector(raw, drive_lba);
}

// Decode one already-read audio sector into the ring and account it. Shared by the normal push and
// by the overflow path above, so both store, count, log and mark the stream live identically.
int XaStream::decodeAndStoreSector(const uint8_t *raw, uint32_t drive_lba) {
  int16_t pcm[4032 * 2]; // mono sectors yield up to 4032 frames (see psx::movie::decodeXaSector)
  int freq = src_freq;
  int n = psx::movie::decodeXaSector(raw, pcm, hist, &freq);
  src_freq = freq;
  for (int i = 0; i < n; i++) {
    uint32_t idx = (wr + (uint32_t)i) % kXaRingFrames;
    ring[idx][0] = pcm[std::size_t{2} * i];
    ring[idx][1] = pcm[std::size_t{2} * i + 1];
  }
  wr += (uint32_t)n;
  sectors++;
  active = 1; // pulls see a live stream as long as the drive keeps delivering
  lucent::debug("xasec",
                "push LBA {} file={} chan={} submode={:02X} n={} freq={} (wr={} rd={})",
                drive_lba,
                raw[16],
                raw[17],
                raw[18],
                n,
                freq,
                wr,
                (uint32_t)rd);
  return n;
}

// Beetle spu.c CD-audio source: one stereo pair per 44.1kHz output sample. Both channels are
// always written (silence when not streaming). Resamples the XA source rate to 44100 via a
// fractional read cursor with linear interpolation.
void XaStream::pullSample(int32_t *samples) {
  if (!active) {
    samples[0] = samples[1] = 0;
    return;
  }
  pulls++;

  // Keep at least 2 frames decoded ahead of the read cursor (linear interp needs the next one).
  // PUSH mode never self-fetches: the CDC drive owns the cursor and fills the ring (xa_push_audio_sector);
  // a pull-side fetch here would read sectors the drive has not reached and desync A/V.
  while (active && !push_mode && (wr - (uint32_t)rd) < 2) {
    if (decodeNextSector() == 0 && !active) {
      break;
    }
  }
  uint32_t avail = wr - (uint32_t)rd;
  if (avail < 1) {
    primed = 0; // starved: a push stream rebuilds its backlog before playing again
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
  // Push mode trims the rate toward XaPushPacing's backlog against drive/SPU clock drift; pull mode
  // keeps the exact ratio because its cursor is the drive cursor.
  double ratio = (double)src_freq / 44100.0;
  if (push_mode) {
    ratio *= XaPushPacing::rateScale(avail);
  }

  uint32_t i0 = (uint32_t)rd;
  double frac = rd - (double)i0;
  uint32_t a = i0 % kXaRingFrames;
  uint32_t b = (avail >= 2) ? (i0 + 1) % kXaRingFrames : a;
  for (int ch = 0; ch < 2; ch++) {
    double v = (double)ring[a][ch] * (1.0 - frac) + (double)ring[b][ch] * frac;
    int32_t s = (int32_t)(v >= 0 ? v + 0.5 : v - 0.5);
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
  if (s_bound == nullptr) {
    samples[0] = samples[1] = 0;
    return;
  }
  s_bound->pullSample(samples);
}
