// Native in-game XA-ADPCM CD-audio streaming for the Tomba!2 PC port.
//
// WHY: the game streams CD-XA audio (opening-cutscene BGM + dialog voice acting) by
// commanding the CD controller — Setmode with the XA-ADPCM bit, Setloc, ReadS — and the
// real hardware decodes those Mode2/Form2 sectors and mixes them straight into the SPU's
// CD-audio input (no copy into main RAM). The port's CD layer (cd_override.c) replaces the
// libcd read path with synchronous data reads and ACKs every controller command, so this
// audio path was entirely missing: the prologue narration and all in-game voice were silent
// while the oracle (Beetle, full CD emulation) plays them. Sequenced (libsnd) BGM is a
// SEPARATE, working path — see docs/journal.md later-75/77.
//
// HOW: cd_override.c's CdControl wrapper (FUN_8008AC34) feeds us the controller commands:
//   Setmode (0x0E)  -> xa_stream_setmode(): track the XA-ADPCM enable (0x40) + SF filter (0x08)
//   Setfilter(0x0D) -> xa_stream_setfilter(file, channel)
//   Setloc (0x02)   -> xa_stream_setloc(): MSF (BCD) -> CHD LBA (= msf_frames - 150)
//   ReadS  (0x1B) / ReadN (0x06) -> xa_stream_start(): begin streaming from the set LBA
//   Pause  (0x09) / Stop (0x08)  -> xa_stream_stop()
// Decoding is PULL-driven: Beetle's spu.c calls CDC_GetCDAudioSample() once per 44.1kHz
// output sample (scaled by the game's CD input volume CDVol and gated on SPUControl bit0 —
// both set by the game). We drain a small decoded-XA ring; when it runs low we decode the
// next CD sector (advancing the LBA) with xa_decode_sector() [native_fmv.c, mednafen-parity].
// Pull-driven decode self-paces to realtime: we read exactly as many sectors as the SPU
// consumes, so the LBA advances at the real playback rate with no separate timer/tick.
// XA runs at 37800 (or 18900) Hz; we resample to the SPU's 44100 Hz with a fractional phase
// accumulator + linear interpolation.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// From native_fmv.c (non-static): decode one raw 2352B XA sector to interleaved S16 stereo;
// `hist` is the per-channel IIR history that MUST persist across sectors; *freq <- rate.
int xa_decode_sector(const uint8_t* raw, int16_t* out, int16_t hist[2][2], int* freq);
int disc_read_raw(uint32_t lba, uint8_t* out, uint32_t n);   // disc.c (raw 2352B sector)

// ---- streaming state -----------------------------------------------------------------
static int      s_active;                 // 1 while ReadS streaming with XA-ADPCM enabled
static uint32_t s_lba;                    // next CHD sector to read
static uint8_t  s_mode;                   // last Setmode
static int      s_filter_set;             // a Setfilter was issued
static uint8_t  s_filter_file, s_filter_chan;
static int      s_dbg = -1;

// ---- native voice/BGM clip state (xa_stream_play, the engine port of FUN_8001cfc8) ------
// The game's voice/BGM streaming used a PSX task (slot 2, FUN_8001cfc8) that issued the CD
// commands and busy-polled GetlocL for the clip end. We replace that whole task with a native
// clip player: the engine voice APIs (FUN_8001d2a8) call xa_stream_play() directly, we own task
// slot 2, and signal completion (clip head past end) so the cutscene's `while(DAT_801fe0e0!=0)`
// wait advances. No PSX coroutine, no faked GetlocL.
static int      s_owns_slot2;             // 1 while we're the native owner of voice task slot 2
static uint32_t s_end_lba;                // clip end LBA (0 = open-ended, e.g. CdControl path)
static uint32_t s_clip_start;             // clip start LBA (for idempotency + loop restart)
static uint8_t  s_clip_chan;              // clip channel (for idempotency)
static int      s_loop;                   // loop the clip when the head passes the end

// Decoded-sample ring (interleaved S16 stereo) at the XA source rate. Sized well above one
// sector (2016 frames) so we never overflow: we only decode when the ring is nearly drained.
#define XA_RING_FRAMES 16384
static int16_t  s_ring[XA_RING_FRAMES][2];
static uint32_t s_wr;                     // total frames written (monotonic); storage index = s_wr % N
static double   s_rd;                     // total frames read (monotonic, fractional)
static int16_t  s_hist[2][2];             // XA IIR history, persists across sectors
static int      s_src_freq = 37800;

static void xa_reset_buffers(void) {
  s_wr = 0; s_rd = 0.0;
  s_hist[0][0] = s_hist[0][1] = s_hist[1][0] = s_hist[1][1] = 0;
}

void xa_stream_setmode(uint8_t mode) {
  if (s_dbg < 0) s_dbg = getenv("PSXPORT_XA_DBG") ? atoi(getenv("PSXPORT_XA_DBG")) : 0;
  s_mode = mode;
  // If XA-ADPCM output (bit6) was turned off mid-stream, stop feeding the SPU.
  if (s_active && !(mode & 0x40)) { s_active = 0; if (s_dbg) fprintf(stderr, "[xa] stop (XA bit cleared, mode=%02X)\n", mode); }
}

void xa_stream_setfilter(uint8_t file, uint8_t chan) {
  s_filter_set = 1; s_filter_file = file; s_filter_chan = chan;
  if (s_dbg > 0) fprintf(stderr, "[xa] setfilter file=%u chan=%u\n", file, chan);
}

// Setloc params: amm,ass,asect in BCD; MSF includes the 2s (150-frame) lead-in, so the
// CHD/ISO LBA = msf_frames - 150 (matches disc.c's by-LBA convention).
void xa_stream_setloc(uint8_t amm, uint8_t ass, uint8_t asect) {
  int mm = (amm >> 4) * 10 + (amm & 0xF);
  int ss = (ass >> 4) * 10 + (ass & 0xF);
  int ff = (asect >> 4) * 10 + (asect & 0xF);
  s_lba = (uint32_t)((mm * 60 + ss) * 75 + ff - 150);
  if (s_dbg > 0) fprintf(stderr, "[xa] setloc %02X:%02X:%02X -> LBA %u\n", amm, ass, asect, s_lba);
}

void xa_stream_start(void) {
  if (s_dbg < 0) s_dbg = getenv("PSXPORT_XA_DBG") ? atoi(getenv("PSXPORT_XA_DBG")) : 0;
  if (!(s_mode & 0x40)) {            // ReadS without XA-ADPCM enabled = data read (not our concern)
    if (s_dbg) fprintf(stderr, "[xa] ReadS but XA bit not set (mode=%02X) - ignoring\n", s_mode);
    return;
  }
  xa_reset_buffers();
  s_active = 1;
  if (s_dbg) fprintf(stderr, "[xa] START streaming @ LBA %u (mode=%02X filter=%d)\n",
                     s_lba, s_mode, s_filter_set);
}

void xa_stream_stop(void) {
  if (s_active && s_dbg) fprintf(stderr, "[xa] STOP @ LBA %u\n", s_lba);
  s_active = 0;
  s_owns_slot2 = 0;
}

// ---- native voice/BGM clip player (engine port of the FUN_8001cfc8 task) ----------------
// Play the XA clip on `chan` spanning [start..end] (CHD LBAs), looping if `loop`. Called by
// the FUN_8001d2a8 override (which all the by-index voice APIs funnel through). Idempotent: a
// repeat call for the clip already playing is a no-op, so the dialog re-issuing "play line N"
// every frame can NOT reset the ring (that was the "first note repeats" bug). Marks us the
// owner of task slot 2 so the native scheduler skips the (now unused) recomp coroutine and the
// cutscene's `while (DAT_801fe0e0 != 0)` wait is driven by clip completion below.
void xa_stream_play(uint8_t chan, uint32_t start, uint32_t end, int loop) {
  if (s_dbg < 0) s_dbg = getenv("PSXPORT_XA_DBG") ? atoi(getenv("PSXPORT_XA_DBG")) : 0;
  if (s_active && s_owns_slot2 && chan == s_clip_chan && start == s_clip_start) {
    s_owns_slot2 = 1;                       // same clip already playing: idempotent
    return;
  }
  s_filter_set = 1; s_filter_file = 1; s_filter_chan = chan;   // game always uses file 1
  s_mode = 0xC8;                            // Speed | ADPCM | SF-filter (as the game sets)
  s_lba = start; s_end_lba = end; s_clip_start = start; s_clip_chan = chan; s_loop = loop;
  xa_reset_buffers();
  s_active = 1; s_owns_slot2 = 1;
  if (s_dbg) fprintf(stderr, "[xa] PLAY clip chan=%u [%u..%u] loop=%d\n", chan, start, end, loop);
}

int  xa_stream_owns_slot2(void) { return s_owns_slot2; }
int  xa_stream_voice_busy(void) { return s_owns_slot2 && s_active; }
// For the dialog-vs-ingame-music coordination in cd_override.c: distinguish a LOOPING clip
// (ingame/background music) from a one-shot (voice/narration), and whether anything streams.
int  xa_stream_is_looping(void) { return s_active && s_loop; }
int  xa_stream_is_active(void)  { return s_active; }
void xa_stream_voice_release(void) { s_owns_slot2 = 0; }

// Current drive-head LBA while streaming, for the engine's GetlocL position poll
// (FUN_8001cfc8 waits for the head to pass the clip's end LBA to know the voice/BGM clip
// finished and advance the cutscene). Returns 1 + *lba when streaming, 0 when idle.
int xa_stream_play_lba(uint32_t* lba) {
  if (!s_active) return 0;
  if (lba) *lba = s_lba;          // s_lba = next sector to read = the physical read-head position
  return 1;
}

// Decode the next audio sector into the ring. Skips interleaved non-audio sectors (advancing
// the LBA); stops the stream at end-of-file / non-Mode2 / read failure. Returns frames added.
static int xa_decode_next_sector(void) {
  uint8_t raw[2352];
  // Clip end: when the read head passes the clip's end LBA, the clip is done. Loop clips
  // restart at the start; one-shot clips stop (which clears busy -> cutscene advances).
  if (s_end_lba && s_lba > s_end_lba) {
    if (s_loop) { s_lba = s_clip_start; s_hist[0][0]=s_hist[0][1]=s_hist[1][0]=s_hist[1][1]=0; }
    else { if (s_dbg) fprintf(stderr, "[xa] clip done @ LBA %u (end %u)\n", s_lba, s_end_lba); s_active = 0; return 0; }
  }
  for (int guard = 0; guard < 64; guard++) {     // bounded scan past any interleaved data sectors
    if (!disc_read_raw(s_lba, raw, 2352)) { s_active = 0; return 0; }
    uint8_t modebyte = raw[15];                   // CD header: 12 sync + min/sec/frame/MODE
    if (modebyte != 2) { s_active = 0; return 0; }// ran off the Mode2 stream
    uint8_t file = raw[16], chan = raw[17], submode = raw[18];
    s_lba++;
    int is_audio = (submode & 0x04) != 0;         // submode bit2 = ADPCM audio sector
    int eof      = (submode & 0x80) != 0;         // submode bit7 = end-of-file
    int pass = is_audio;
    if (pass && s_filter_set && (s_mode & 0x08))  // SF filter: match subheader file+channel
      pass = (file == s_filter_file && chan == s_filter_chan);
    if (pass) {
      static int16_t pcm[4032 * 2];   // mono sectors yield up to 4032 frames (see xa_decode_sector)
      int freq = s_src_freq;
      int n = xa_decode_sector(raw, pcm, s_hist, &freq);
      s_src_freq = freq;
      for (int i = 0; i < n; i++) {
        uint32_t idx = (s_wr + (uint32_t)i) % XA_RING_FRAMES;
        s_ring[idx][0] = pcm[2 * i];
        s_ring[idx][1] = pcm[2 * i + 1];
      }
      s_wr += (uint32_t)n;
      if (s_dbg > 1) fprintf(stderr, "[xa]  sector LBA %u file=%u chan=%u submode=%02X n=%d freq=%d (wr=%u rd=%u)\n",
                             s_lba - 1, file, chan, submode, n, freq, s_wr, (uint32_t)s_rd);
      if (eof) { if (s_dbg) fprintf(stderr, "[xa] EOF @ LBA %u\n", s_lba - 1); s_active = 0; }
      return n;
    }
    if (eof) { if (s_dbg) fprintf(stderr, "[xa] EOF (non-audio) @ LBA %u\n", s_lba - 1); s_active = 0; return 0; }
  }
  return 0;   // 64 consecutive non-passing sectors: give up this pump, try again next sample
}

// Beetle spu.c CD-audio source: one stereo pair per 44.1kHz output sample. Both channels are
// always written (silence when not streaming). Resamples the XA source rate to 44100 via a
// fractional read cursor with linear interpolation.
void CDC_GetCDAudioSample(int32_t* samples) {
  if (!s_active) { samples[0] = samples[1] = 0; return; }

  // Keep at least 2 frames decoded ahead of the read cursor (linear interp needs the next one).
  while (s_active && (s_wr - (uint32_t)s_rd) < 2) {
    if (xa_decode_next_sector() == 0 && !s_active) break;
  }
  uint32_t avail = s_wr - (uint32_t)s_rd;
  if (avail < 1) { samples[0] = samples[1] = 0; return; }

  uint32_t i0 = (uint32_t)s_rd;
  double frac = s_rd - (double)i0;
  uint32_t a = i0 % XA_RING_FRAMES;
  uint32_t b = (avail >= 2) ? (i0 + 1) % XA_RING_FRAMES : a;
  for (int ch = 0; ch < 2; ch++) {
    double v = (double)s_ring[a][ch] * (1.0 - frac) + (double)s_ring[b][ch] * frac;
    int32_t s = (int32_t)(v >= 0 ? v + 0.5 : v - 0.5);
    if (s < -32768) s = -32768; else if (s > 32767) s = 32767;
    samples[ch] = s;
  }
  s_rd += (double)s_src_freq / 44100.0;   // advance source cursor at the resample ratio
}
