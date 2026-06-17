// xa_wavdump — STANDALONE offline XA-ADPCM extractor for the Tomba!2 CHD.
//
// No game boot: opens the CHD directly (same by-LBA hunk read as runtime/recomp/disc.c) and decodes
// a chan's XA-ADPCM audio over an LBA range to a WAV. The decoder is copied verbatim from
// native_fmv.c (xa_decode_unit / xa_decode_sector) so the bytes match the port's playback path.
//
// Use: dump the gameplay area music (chan4 [84515..97979]) PLUS extra sectors past the game's end so a
// human can hear whether the song keeps flowing past our loop point (= we loop early) and measure drift.
//
//   build: see tools/build_xa_wavdump.sh
//   run:   PSXPORT_TOMBA2_DISC=<chd> tools/xa_wavdump <chan> <start_lba> <end_lba> <pad_sectors> <out.wav>
//          (the WAV holds [start .. end+pad]; it prints the timestamp where the port loops, = end+1)
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libchdr/chd.h>

// ---- CHD by-LBA raw sector read (mirrors runtime/recomp/disc.c) -------------------------------
#define RAW_FRAME 2448u
static chd_file* s_chd;
static uint32_t  s_fph, s_hcount, s_hbytes, s_cached = 0xFFFFFFFFu;
static uint8_t*  s_hbuf;

static int disc_open(const char* path) {
  if (chd_open(path, CHD_OPEN_READ, 0, &s_chd) != CHDERR_NONE) { fprintf(stderr, "cannot open CHD: %s\n", path); return 0; }
  const chd_header* h = chd_get_header(s_chd);
  s_hbytes = h->hunkbytes; s_fph = h->hunkbytes / RAW_FRAME; s_hcount = h->totalhunks;
  s_hbuf = malloc(s_hbytes);
  return s_fph > 0;
}
static int disc_read_raw(uint32_t lba, uint8_t* out, uint32_t n) {
  if (n > 2352u) n = 2352u;
  uint32_t hunk = lba / s_fph, off = (lba % s_fph) * RAW_FRAME;
  if (hunk >= s_hcount) return 0;
  if (hunk != s_cached) { if (chd_read(s_chd, hunk, s_hbuf) != CHDERR_NONE) return 0; s_cached = hunk; }
  memcpy(out, s_hbuf + off, n);
  return 1;
}

// ---- XA-ADPCM decode (copied verbatim from native_fmv.c) --------------------------------------
static const int32_t XA_W[16][2] = { {0,0},{60,0},{115,-52},{98,-55},{122,-60} };
static void xa_decode_unit(const uint8_t* in, int16_t* out, int shift, int filter) {
  for (int i = 0; i < 28; i++) {
    int32_t s = (int16_t)(in[i] << 8); s >>= shift;
    s += (out[i-1] * XA_W[filter][0] + out[i-2] * XA_W[filter][1]) >> 6;
    if (s < -32768) s = -32768; else if (s > 32767) s = 32767;
    out[i] = (int16_t)s;
  }
}
static int xa_decode_sector(const uint8_t* raw, int16_t* out, int16_t hist[2][2], int* freq) {
  int coding = raw[19];
  int ishift = (coding & 0x10) ? 0 : 1;
  int stereo = coding & 0x01;
  int units  = 4 << ishift;
  if (freq) *freq = (coding & 0x04) ? 18900 : 37800;
  static int16_t ch[2][4032 + 8];
  int cp[2] = {0,0};
  for (int group = 0; group < 18; group++) {
    const uint8_t* sg = raw + 24 + group * 128;
    for (int unit = 0; unit < units; unit++) {
      int param  = sg[(unit & 3) | ((unit & 4) << 1)];
      int pcopy  = sg[4 | (unit & 3) | ((unit & 4) << 1)];
      uint8_t ib[28];
      for (int i = 0; i < 28; i++) {
        uint8_t t = sg[16 + i * 4 + (unit >> ishift)];
        if (ishift) { t <<= (unit & 1) ? 0 : 4; t &= 0xf0; }
        ib[i] = t;
      }
      int ocn = (unit & 1) && stereo;
      int16_t ob[2 + 28];
      ob[0] = hist[ocn][0]; ob[1] = hist[ocn][1];
      xa_decode_unit(ib, ob + 2, param & 0x0F, param >> 4);
      hist[ocn][0] = ob[28]; hist[ocn][1] = ob[29];
      if (param != pcopy) memset(ob, 0, sizeof ob);
      if (stereo) { for (int s = 0; s < 28; s++) ch[ocn][cp[ocn]++] = ob[2 + s]; }
      else { for (int s = 0; s < 28; s++) { ch[0][cp[0]++] = ob[2+s]; ch[1][cp[1]++] = ob[2+s]; } }
    }
  }
  int n = cp[0];
  for (int i = 0; i < n; i++) { out[2*i] = ch[0][i]; out[2*i+1] = ch[1][i]; }
  return n;
}

// ---- WAV out ---------------------------------------------------------------------------------
static void u32(FILE* f, uint32_t v){ fputc(v&255,f);fputc((v>>8)&255,f);fputc((v>>16)&255,f);fputc((v>>24)&255,f); }
static void u16(FILE* f, uint16_t v){ fputc(v&255,f);fputc((v>>8)&255,f); }

int main(int argc, char** argv) {
  if (argc < 6) { fprintf(stderr, "usage: %s <chan> <start> <end> <pad> <out.wav>  (env PSXPORT_TOMBA2_DISC)\n", argv[0]); return 2; }
  uint8_t chan = (uint8_t)atoi(argv[1]);
  uint32_t start = (uint32_t)strtoul(argv[2],0,0), end = (uint32_t)strtoul(argv[3],0,0), pad = (uint32_t)strtoul(argv[4],0,0);
  const char* out = argv[5];
  const char* disc = getenv("PSXPORT_TOMBA2_DISC"); if (!disc) disc = getenv("PSXPORT_DISC");
  if (!disc) { fprintf(stderr, "set PSXPORT_TOMBA2_DISC\n"); return 2; }
  if (!disc_open(disc)) return 1;

  FILE* f = fopen(out, "wb"); if (!f) { fprintf(stderr, "cannot write %s\n", out); return 1; }
  for (int i=0;i<44;i++) fputc(0,f);
  uint8_t raw[2352]; static int16_t pcm[4032*2]; int16_t hist[2][2]={{0,0},{0,0}};
  long frames=0, loop_frame=-1; int freq=37800;
  for (uint32_t lba=start; lba<=end+pad; lba++) {
    if (lba==end+1) loop_frame = frames;        // the port loops here (s_lba > end)
    if (!disc_read_raw(lba, raw, 2352)) break;
    if (raw[15]!=2) break;
    uint8_t file=raw[16], c=raw[17], sub=raw[18];
    if (!(sub&0x04)) continue;                  // not an audio sector
    if (!(file==1 && c==chan)) continue;        // not our channel
    int fq=freq; int n=xa_decode_sector(raw,pcm,hist,&fq); freq=fq;
    for (int i=0;i<n;i++){ u16(f,(uint16_t)pcm[2*i]); u16(f,(uint16_t)pcm[2*i+1]); }
    frames += n;
  }
  long datab = frames*4;
  fseek(f,0,SEEK_SET);
  fputs("RIFF",f); u32(f,36+datab); fputs("WAVE",f);
  fputs("fmt ",f); u32(f,16); u16(f,1); u16(f,2); u32(f,freq); u32(f,freq*4); u16(f,4); u16(f,16);
  fputs("data",f); u32(f,datab);
  fclose(f);
  fprintf(stderr, "[xa_wavdump] %s: chan%u [%u..%u]+%u -> %ld frames @ %dHz (%.2f s). PORT LOOPS at %.2f s (frame %ld)\n",
          out, chan, start, end, pad, frames, freq, (double)frames/freq,
          loop_frame<0?-1.0:(double)loop_frame/freq, loop_frame);
  return 0;
}
