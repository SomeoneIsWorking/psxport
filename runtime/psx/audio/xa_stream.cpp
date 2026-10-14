// xa_stream.cpp — native XA-ADPCM CD audio: the streamer the CD command layer drives (Setmode, Setfilter,
// Setloc, ReadS/ReadN, Stop/Pause) and the clip player, decoded on demand as Beetle's spu.c pulls
// CDC_GetCDAudioSample and resampled from 37800/18900 Hz to 44100 Hz.
#include "c_subsys.h" // xa_decode_sector
#include "disc.h"
#include "xa_push_pacing.h"
#include "xa_state.h"
#include <lucent/log.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
// ---- streaming state — PER-INSTANCE (XaState, one per Game; see xa_state.h) -------------------------
// Every xa_stream_* entry point takes its XaState* `xs` explicitly. The historical `s_*` field
// spellings are kept as macros over `xs->*` so the function bodies below are byte-unchanged.
// SANCTIONED VENDOR-INTEROP EXCEPTION: the vendored Beetle spu.c pulls samples through the
// context-free CDC_GetCDAudioSample(s32*) callback, which cannot carry an instance — `s_bound`
// (set per frame-step by xa_bind_state, hw_bind.cpp) exists ONLY for that pull.
static XaState *s_bound = 0;

void xa_state_init(XaState *s) {
  struct DiscState *disc = s->disc; // preserve wiring across re-init
  memset(s, 0, sizeof *s);
  s->src_freq = 37800;
  s->disc = disc;
}
void xa_bind_state(XaState *s) {
  s_bound = s;
}

namespace psx::audio {
void releaseXaState(const XaState &state) {
  if (s_bound == &state) {
    s_bound = nullptr;
  }
}
} // namespace psx::audio

#define s_active (xs->active)
#define s_lba (xs->lba)
#define s_mode (xs->mode)
#define s_filter_set (xs->filter_set)
#define s_filter_file (xs->filter_file)
#define s_filter_chan (xs->filter_chan)
#define s_end_lba (xs->end_lba)
#define s_clip_start (xs->clip_start)
#define s_clip_chan (xs->clip_chan)
#define s_loop (xs->loop)
#define s_ring (xs->ring)
#define s_wr (xs->wr)
#define s_rd (xs->rd)
#define s_hist (xs->hist)
#define s_src_freq (xs->src_freq)
#define s_pulls (xs->pulls)
#define s_sectors (xs->sectors)

static void xa_reset_buffers(XaState *xs) {
  s_wr = 0;
  s_rd = 0.0;
  s_pulls = 0;
  s_sectors = 0;
  xs->primed = 0;
  xs->held_pulls = 0;
  s_hist[0][0] = s_hist[0][1] = s_hist[1][0] = s_hist[1][1] = 0;
}

void xa_stream_setmode(XaState *xs, uint8_t mode) {
  s_mode = mode;
  // If XA-ADPCM output (bit6) was turned off mid-stream, stop feeding the SPU.
  if (s_active && !(mode & 0x40)) {
    s_active = 0;
    lucent::debug("xa", "stop (XA bit cleared, mode={:02X})", mode);
  }
}

void xa_stream_setfilter(XaState *xs, uint8_t file, uint8_t chan) {
  s_filter_set = 1;
  s_filter_file = file;
  s_filter_chan = chan;
  lucent::debug("xa", "setfilter file={} chan={}", file, chan);
}

// Setloc params: amm,ass,asect in BCD; MSF includes the 2s (150-frame) lead-in, so the
// CHD/ISO LBA = msf_frames - 150 (matches disc.cpp's by-LBA convention).
void xa_stream_setloc(XaState *xs, uint8_t amm, uint8_t ass, uint8_t asect) {
  int mm = (amm >> 4) * 10 + (amm & 0xF);
  int ss = (ass >> 4) * 10 + (ass & 0xF);
  int ff = (asect >> 4) * 10 + (asect & 0xF);
  s_lba = (uint32_t)((mm * 60 + ss) * 75 + ff - 150);
  lucent::debug("xa", "setloc {:02X}:{:02X}:{:02X} -> LBA {}", amm, ass, asect, s_lba);
}

void xa_stream_start(XaState *xs) {
  if (!(s_mode & 0x40)) { // ReadS without XA-ADPCM enabled = data read (not our concern)
    lucent::debug("xa", "ReadS but XA bit not set (mode={:02X}) - ignoring", s_mode);
    return;
  }
  xa_reset_buffers(xs);
  s_end_lba = 0;
  s_loop = 0; // open-ended CdControl stream: no clip end -> EOF terminates it
  s_active = 1;
  lucent::debug("xa", "START streaming @ LBA {} (mode={:02X} filter={})", s_lba, s_mode, s_filter_set);
}

void xa_stream_stop(XaState *xs) {
  // Report the census, not just the position. A stream that stops having decoded nothing is the
  // silent-audio symptom, and `pulls` says WHICH side failed (see XaState::pulls).
  if (s_active) {
    lucent::debug(
        "xa", "STOP @ LBA {} — {} pull(s) from the SPU, {} audio sector(s) decoded", s_lba, s_pulls, s_sectors);
  }
  s_active = 0;
}

// ---- clip player: one channel over [start..end], looping back to start when asked ----
void xa_stream_play(XaState *xs, uint8_t chan, uint32_t start, uint32_t end, int loop) {
  s_filter_set = 1;
  s_filter_file = 1;
  s_filter_chan = chan; // game always uses file 1
  s_mode = 0xC8;        // Speed | ADPCM | SF-filter (as the game sets)
  s_lba = start;
  s_end_lba = end;
  s_clip_start = start;
  s_clip_chan = chan;
  s_loop = loop;
  xa_reset_buffers(xs);
  s_active = 1;
  lucent::debug("xa", "PLAY clip chan={} [{}..{}] loop={}", chan, start, end, loop);
}

int xa_stream_is_looping(XaState *xs) {
  return s_active && s_loop;
}
int xa_stream_is_active(XaState *xs) {
  return s_active;
}

int xa_stream_play_lba(XaState *xs, uint32_t *lba) {
  if (!s_active) {
    return 0;
  }
  if (lba) {
    *lba = s_lba; // s_lba = next sector to read = the physical read-head position
  }
  return 1;
}

// Decode the next audio sector into the ring. Skips interleaved non-audio sectors (advancing
// the LBA); stops the stream at end-of-file / non-Mode2 / read failure. Returns frames added.
static int xa_decode_next_sector(XaState *xs) {
  uint8_t raw[2352];
  // Past the clip's end LBA a looping clip restarts and a one-shot one stops.
  if (s_end_lba && s_lba > s_end_lba) {
    if (s_loop) {
      lucent::debug("xa",
                    "LOOP chan={} back to {} (passed end {}, span={} sectors)",
                    s_clip_chan,
                    s_clip_start,
                    s_end_lba,
                    s_end_lba - s_clip_start);
      s_lba = s_clip_start;
      s_hist[0][0] = s_hist[0][1] = s_hist[1][0] = s_hist[1][1] = 0;
    } else {
      lucent::debug("xa", "clip done @ LBA {} (end {})", s_lba, s_end_lba);
      s_active = 0;
      return 0;
    }
  }
  for (int guard = 0; guard < 64; guard++) { // bounded scan past any interleaved data sectors
    if (!disc_read_raw(xs->disc, s_lba, raw, 2352)) {
      s_active = 0;
      return 0;
    }
    uint8_t modebyte = raw[15]; // CD header: 12 sync + min/sec/frame/MODE
    if (modebyte != 2) {
      s_active = 0;
      return 0;
    } // ran off the Mode2 stream
    uint8_t file = raw[16], chan = raw[17], submode = raw[18];
    s_lba++;
    int is_audio = (submode & 0x04) != 0; // submode bit2 = ADPCM audio sector
    int eof = (submode & 0x80) != 0;      // submode bit7 = end-of-file
    int pass = is_audio;
    if (pass && s_filter_set && (s_mode & 0x08)) { // SF filter: match subheader file+channel
      pass = (file == s_filter_file && chan == s_filter_chan);
    }
    if (pass) {
      int16_t pcm[4032 * 2]; // mono sectors yield up to 4032 frames (see xa_decode_sector)
      int freq = s_src_freq;
      int n = xa_decode_sector(raw, pcm, s_hist, &freq);
      s_src_freq = freq;
      for (int i = 0; i < n; i++) {
        uint32_t idx = (s_wr + (uint32_t)i) % XA_RING_FRAMES;
        s_ring[idx][0] = pcm[2 * i];
        s_ring[idx][1] = pcm[2 * i + 1];
      }
      s_wr += (uint32_t)n;
      s_sectors++;
      lucent::debug("xasec",
                    " sector LBA {} file={} chan={} submode={:02X} n={} freq={} (wr={} rd={})",
                    s_lba - 1,
                    file,
                    chan,
                    submode,
                    n,
                    freq,
                    s_wr,
                    (uint32_t)s_rd);
      // EOF (submode bit7) ends only an OPEN-ENDED stream. A BOUNDED clip ([start..end], e.g. the
      // looping area music) ends strictly at end_lba (handled above): EOF markers inside the range
      // belong to OTHER files/channels interleaved in the same stream and must NOT cut our clip.
      if (eof && !s_end_lba) {
        lucent::debug("xa", "EOF @ LBA {}", s_lba - 1);
        s_active = 0;
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
          "xa", "skipping non-matching EOF @ LBA {} (file={} chan={}) — still streaming", s_lba - 1, file, chan);
    }
  }
  return 0; // 64 consecutive non-passing sectors: give up this pump, try again next sample
}

// Push mode: the CDC drive model (cdc_native.cpp) has already read the sector from the disc and
// decided it is XA-ADPCM audio under beetle's routing rule; decode it into the ring here. The disc
// cursor belongs to the drive — this NEVER touches s_lba. `drive_lba` is diagnostic-only.
// Returns frames decoded (>0) or 0 when the sector is not decodable audio.
//
// The ring overflows when audio is produced faster than the SPU drains it. On hardware the decoded
// XA audio lands in the SPU's own CD buffer, and when THAT overflows the SPU drops/glitches samples
// while the drive keeps spinning at its selected speed — so an overflow discards the OLDEST decoded
// audio (the oldest is the least useful delay), takes the new sector, and never stops the disc. A
// hold-for-retry would instead stall the sectors delivering the guest's VIDEO from the same stream.
static int xa_decode_and_store_sector(XaState *xs, const uint8_t *raw, uint32_t drive_lba);

int xa_push_audio_sector(XaState *xs, const uint8_t *raw, uint32_t drive_lba) {
  if (!xs->push_mode || raw[15] != 2 || !(raw[18] & 0x04)) {
    return 0;
  }
  const uint32_t backlog = (uint32_t)(s_wr - (uint32_t)s_rd);
  if (backlog >= XA_RING_FRAMES - 4096) {
    const uint32_t dropped = backlog - (XA_RING_FRAMES - 4096);
    s_rd = (double)((uint32_t)s_rd + dropped); // oldest audio first; the drive never stops
    const uint32_t streak = ++xs->overflow_streak;
    if ((streak & (streak - 1)) == 0) { // report at 1,2,4,8... — every doubling
      lucent::warn("xa",
                   "ring OVERFLOW (wr={} rd={}) — dropped {} oldest frames, took audio sector LBA {} "
                   "anyway ({} consecutive); the SPU is not draining CD audio",
                   s_wr,
                   (uint32_t)s_rd,
                   dropped,
                   drive_lba,
                   streak);
    }
    return xa_decode_and_store_sector(xs, raw, drive_lba);
  }
  return xa_decode_and_store_sector(xs, raw, drive_lba);
}

// Decode one already-read audio sector into the ring and account it. Shared by the normal push and
// by the overflow path above, so both store, count, log and mark the stream live identically.
static int xa_decode_and_store_sector(XaState *xs, const uint8_t *raw, uint32_t drive_lba) {
  int16_t pcm[4032 * 2]; // mono sectors yield up to 4032 frames (see xa_decode_sector)
  int freq = s_src_freq;
  int n = xa_decode_sector(raw, pcm, s_hist, &freq);
  s_src_freq = freq;
  for (int i = 0; i < n; i++) {
    uint32_t idx = (s_wr + (uint32_t)i) % XA_RING_FRAMES;
    s_ring[idx][0] = pcm[2 * i];
    s_ring[idx][1] = pcm[2 * i + 1];
  }
  s_wr += (uint32_t)n;
  s_sectors++;
  xs->active = 1; // pulls see a live stream as long as the drive keeps delivering
  lucent::debug("xasec",
                "push LBA {} file={} chan={} submode={:02X} n={} freq={} (wr={} rd={})",
                drive_lba,
                raw[16],
                raw[17],
                raw[18],
                n,
                freq,
                s_wr,
                (uint32_t)s_rd);
  return n;
}

// Beetle spu.c CD-audio source: one stereo pair per 44.1kHz output sample. Both channels are
// always written (silence when not streaming). Resamples the XA source rate to 44100 via a
// fractional read cursor with linear interpolation.
extern "C" void CDC_GetCDAudioSample(int32_t *samples) { // called from the vendored spu.c (C)
  XaState *xs = s_bound;                                 // vendor-interop pull: reads the bound instance (see above)
  if (!xs || !s_active) {
    samples[0] = samples[1] = 0;
    return;
  }
  s_pulls++;

  // Keep at least 2 frames decoded ahead of the read cursor (linear interp needs the next one).
  // PUSH mode never self-fetches: the CDC drive owns the cursor and fills the ring (xa_push_audio_sector);
  // a pull-side fetch here would read sectors the drive has not reached and desync A/V.
  while (s_active && !xs->push_mode && (s_wr - (uint32_t)s_rd) < 2) {
    if (xa_decode_next_sector(xs) == 0 && !s_active) {
      break;
    }
  }
  uint32_t avail = s_wr - (uint32_t)s_rd;
  if (avail < 1) {
    xs->primed = 0; // starved: a push stream rebuilds its backlog before playing again
    xs->held_pulls = 0;
    samples[0] = samples[1] = 0;
    return;
  }
  if (xs->push_mode && !xs->primed) {
    if (!XaPushPacing::readyToPlay(avail, xs->held_pulls)) {
      ++xs->held_pulls;
      samples[0] = samples[1] = 0;
      return;
    }
    xs->primed = 1;
  }
  // Push mode trims the rate toward XaPushPacing's backlog against drive/SPU clock drift; pull mode
  // keeps the exact ratio because its cursor is the drive cursor.
  double ratio = (double)s_src_freq / 44100.0;
  if (xs->push_mode) {
    ratio *= XaPushPacing::rateScale(avail);
  }

  uint32_t i0 = (uint32_t)s_rd;
  double frac = s_rd - (double)i0;
  uint32_t a = i0 % XA_RING_FRAMES;
  uint32_t b = (avail >= 2) ? (i0 + 1) % XA_RING_FRAMES : a;
  for (int ch = 0; ch < 2; ch++) {
    double v = (double)s_ring[a][ch] * (1.0 - frac) + (double)s_ring[b][ch] * frac;
    int32_t s = (int32_t)(v >= 0 ? v + 0.5 : v - 0.5);
    if (s < -32768) {
      s = -32768;
    } else if (s > 32767) {
      s = 32767;
    }
    samples[ch] = s;
  }
  s_rd += ratio;
}
