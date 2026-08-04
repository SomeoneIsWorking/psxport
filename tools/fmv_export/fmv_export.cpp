// fmv_export — STANDALONE offline .STR (FMV) exporter for the Tomba!2 CHD.
//
// No game boot: opens the CHD directly (same by-LBA hunk read as runtime/recomp/disc.c) and
// decodes an .STR movie (MOVIE/LOGO.STR, MOVIE/OP.STR) to PNG frames + a CD-XA audio WAV + an
// index manifest. The decode uses the SAME functions the runtime player runs — bs_decode_frame /
// mdec_decode_to_rgb555 / xa_decode_sector from runtime/recomp/fmv_decode.cpp — so a frame that
// dumps wrong here is a runtime frame that plays wrong, by construction. There is no forked copy.
//
// Output (all under <out-dir>):
//   frames/frame_NNNNN.png   one RGB24 PNG per video frame (STR 320x240)
//   audio.wav                the movie's XA-ADPCM soundtrack, S16 stereo
//   index.txt                per-video-frame manifest (STR frame, samples, media time)
//
// build: see tools/fmv_export/build.sh  (links fmv_decode.cpp + mdec_beetle.c + mednafen mdec.c +
//        lucent + cfg + libchdr + zlib — the runtime decode, no game/SDL)
// run:   PSXPORT_TOMBA2_DISC=<chd> tools/fmv_export/fmv_export <out-dir> [movie-path]
//
// Env/config (lucent, prefix-free names — cfg.cpp reads the same vars the same way):
//   PSXPORT_TOMBA2_DISC / PSXPORT_DISC / a *.chd arg  — the disc image (env or ./.env line)
//   PSXPORT_FMV_MAXFRAMES  — bound the number of video frames dumped (0/unset = all)
//   PSXPORT_DEBUG=fmv      — per-frame decode detail (also turns on the shared decoder's channels)
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <filesystem>
#include <string>

#include <zlib.h>
#include <libchdr/chd.h>

#include <lucent/config.h>
#include <lucent/log.h>

#include "cfg.h"              // cfg_on (PSXPORT_FMV_DUMPCODES)
#include "fmv_decode.h"       // bs_decode_frame / mdec_decode_to_rgb555 / xa_decode_sector
#include "c_subsys.h"         // mdec_init

// Vestigial Beetle savestate hook the vendored mdec.c references (state_helpers.h expands
// MDFNSS_StateAction into a real call inside MDEC_StateAction, which an offline decode never
// enters). Same `return 1` stub the runtime keeps in runtime/recomp/pgxp.cpp.
extern "C" int MDFNSS_StateAction(void* st, int load, int data_only, void* sf, const char* name) {
  (void)st; (void)load; (void)data_only; (void)sf; (void)name; return 1;
}

#define SECTOR_USER 2048u
#define SUBHDR_LEN  32u
#define RAW_FRAME   2448u     // 2352 raw + 96 subcode, CHD CD unit

#define FMV_PAYLOAD_BYTES (512u * 1024u)   // concatenated BS payload (same caps as the runtime)
#define FMV_CODES_MAX     (512u * 1024u)   // MDEC run-level codes
#define FMV_MAX_PIXELS    (1024u * 512u)   // RGB555 frame scratch (VRAM-sized, like the runtime)

// ====================================================================================
// Disc — CHD by-LBA raw sector read (mirrors runtime/recomp/disc.c disc_read_raw)
// ====================================================================================
static chd_file* s_chd;
static uint32_t  s_fph, s_hcount, s_hbytes, s_cached = 0xFFFFFFFFu;
static uint8_t*  s_hbuf;

static bool disc_open(const std::string& path) {
  if (chd_open(path.c_str(), CHD_OPEN_READ, 0, &s_chd) != CHDERR_NONE) {
    lucent::error("fmv", "cannot open CHD: {}", path);
    return false;
  }
  const chd_header* h = chd_get_header(s_chd);
  s_hbytes = h->hunkbytes;
  s_fph    = h->hunkbytes / RAW_FRAME;
  s_hcount = h->totalhunks;
  s_hbuf   = (uint8_t*)malloc(s_hbytes);
  lucent::info("fmv", "opened {} ({} hunks, {} frames/hunk)", path, s_hcount, s_fph);
  return s_fph > 0;
}

static bool disc_read_raw(uint32_t lba, uint8_t* out, uint32_t n) {
  if (n > 2352u) n = 2352u;
  uint32_t hunk = lba / s_fph, off = (lba % s_fph) * RAW_FRAME;
  if (hunk >= s_hcount) return false;
  if (hunk != s_cached) {
    if (chd_read(s_chd, hunk, s_hbuf) != CHDERR_NONE) return false;
    s_cached = hunk;
  }
  memcpy(out, s_hbuf + off, n);
  return true;
}

// Mode-2 sector user data (2048 bytes at raw+24; the CD-XA subheader sits at raw[16..23]).
// Same skip as runtime/recomp/disc.c disc_read_sector.
static bool disc_read_user(uint32_t lba, uint8_t* out) {
  uint8_t raw[2352];
  if (!disc_read_raw(lba, raw, 2352)) return false;
  memcpy(out, raw + 24, SECTOR_USER);
  return true;
}

// ====================================================================================
// ISO9660 — resolve an absolute path (PSX backslash style, case/";version"-insensitive)
// to its data LBA and byte size (mirrors runtime/recomp/disc.c disc_find_file)
// ====================================================================================
static uint32_t rd_le32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool iso_name_eq(const uint8_t* name, int nlen, const char* want) {
  int i = 0;
  for (; i < nlen; i++) {
    char nc = (char)name[i];
    char wc = want[i];
    if (nc == ';') break;
    if (wc == ';' || wc == 0) return false;
    char a = (nc >= 'a' && nc <= 'z') ? (char)(nc - 32) : nc;
    char b = (wc >= 'a' && wc <= 'z') ? (char)(wc - 32) : wc;
    if (a != b) return false;
  }
  return want[i] == 0 || want[i] == ';';
}

static bool iso_find_file(const char* path, uint32_t* out_lba, uint32_t* out_size) {
  uint8_t sec[SECTOR_USER];
  if (!disc_read_user(16, sec)) return false;
  if (sec[0] != 1) return false;                       // Primary Volume Descriptor
  uint32_t dir_lba  = rd_le32(sec + 156 + 2);
  uint32_t dir_size = rd_le32(sec + 156 + 10);

  const char* p = path;
  while (*p == '\\' || *p == '/') p++;
  while (*p) {
    char comp[64]; int cn = 0;
    while (*p && *p != '\\' && *p != '/' && cn < (int)sizeof(comp) - 1) comp[cn++] = *p++;
    comp[cn] = 0;
    while (*p == '\\' || *p == '/') p++;
    int is_last = (*p == 0);

    uint32_t nsec = (dir_size + 2047u) / 2048u, found = 0;
    for (uint32_t s = 0; s < nsec && !found; s++) {
      if (!disc_read_user(dir_lba + s, sec)) return false;
      uint32_t o = 0;
      while (o < SECTOR_USER) {
        uint32_t rlen = sec[o];
        if (rlen == 0) break;
        if (o + rlen > SECTOR_USER) break;
        int nlen = sec[o + 32];
        if (iso_name_eq(sec + o + 33, nlen, comp)) {
          uint32_t e_lba  = rd_le32(sec + o + 2);
          uint32_t e_size = rd_le32(sec + o + 10);
          if (is_last) { *out_lba = e_lba; *out_size = e_size; return true; }
          dir_lba = e_lba; dir_size = e_size; found = 1;
          break;
        }
        o += rlen;
      }
    }
    if (!found) return false;
  }
  return false;
}

// ====================================================================================
// PNG writer — RGB24, zlib (the runtime's frame dumps write the same 5-5-5→8-bit per-channel
// expansion, see tools/vram_png.py)
// ====================================================================================
static void png_chunk(FILE* f, const char* type, const uint8_t* data, uint32_t len) {
  uint32_t be = (len >> 24) | ((len & 0x00ff0000u) >> 8) | ((len & 0x0000ff00u) << 8) | (len << 24);
  fwrite(&be, 4, 1, f);
  uint32_t crc0 = (uint32_t)crc32(0, Z_NULL, 0);
  fwrite(type, 4, 1, f);
  crc0 = (uint32_t)crc32(crc0, (const uint8_t*)type, 4);
  if (len) { fwrite(data, 1, len, f); crc0 = (uint32_t)crc32(crc0, data, len); }
  fputc((crc0 >> 24) & 255, f); fputc((crc0 >> 16) & 255, f); fputc((crc0 >> 8) & 255, f); fputc(crc0 & 255, f);
}

static bool png_write(const char* path, int w, int h, const uint8_t* rgb24) {
  FILE* f = fopen(path, "wb");
  if (!f) { lucent::error("fmv", "cannot write {}", path); return false; }
  fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
  uint8_t ihdr[13] = { 0 };
  ihdr[0] = (uint8_t)(w >> 24); ihdr[1] = (uint8_t)(w >> 16); ihdr[2] = (uint8_t)(w >> 8); ihdr[3] = (uint8_t)w;
  ihdr[4] = (uint8_t)(h >> 24); ihdr[5] = (uint8_t)(h >> 16); ihdr[6] = (uint8_t)(h >> 8); ihdr[7] = (uint8_t)h;
  ihdr[8] = 8;  ihdr[9] = 2;  // 8-bit, truecolor
  png_chunk(f, "IHDR", ihdr, 13);

  size_t stride = (size_t)w * 3;
  size_t rawlen = (size_t)h * (stride + 1);
  uint8_t* raw = (uint8_t*)malloc(rawlen);
  for (int y = 0; y < h; y++) {
    raw[(size_t)y * (stride + 1)] = 0;                     // filter: none
    memcpy(raw + (size_t)y * (stride + 1) + 1, rgb24 + (size_t)y * stride, stride);
  }
  uLongf zlen = (uLongf)rawlen * 2 + 64;
  uint8_t* zbuf = (uint8_t*)malloc(zlen);
  if (compress2(zbuf, &zlen, raw, rawlen, 9) != Z_OK) {
    lucent::error("fmv", "zlib compress failed for {}", path);
    free(raw); free(zbuf); fclose(f); return false;
  }
  png_chunk(f, "IDAT", zbuf, (uint32_t)zlen);
  png_chunk(f, "IEND", nullptr, 0);
  free(raw); free(zbuf);
  fclose(f);
  return true;
}

// ====================================================================================
// WAV writer — S16 stereo PCM (same container as tools/xa_wavdump.c)
// ====================================================================================
static void wav_u32(FILE* f, uint32_t v){ fputc(v & 255, f); fputc((v >> 8) & 255, f); fputc((v >> 16) & 255, f); fputc((v >> 24) & 255, f); }
static void wav_u16(FILE* f, uint16_t v){ fputc(v & 255, f); fputc((v >> 8) & 255, f); }

// ====================================================================================
// .env reader (mirrors runtime/recomp/disc.c env_from_dotenv)
// ====================================================================================
static std::string dotenv(const std::string& key) {
  FILE* f = fopen(".env", "rb");
  if (!f) return {};
  char line[1024];
  std::string found;
  size_t klen = key.size();
  while (fgets(line, sizeof line, f)) {
    char* p = line; while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, key.c_str(), klen) == 0) {
      char* eq = strchr(p, '=');
      if (eq && (size_t)(eq - p) == klen) {
        char* v = eq + 1;
        size_t n = strlen(v);
        while (n && (v[n-1] == '\n' || v[n-1] == '\r' || v[n-1] == ' ' || v[n-1] == '\t')) n--;
        found.assign(v, n);
        break;
      }
    }
  }
  fclose(f);
  return found;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <out-dir> [movie-path] [disc.chd]\n"
                    "       (movie-path default: MOVIE/LOGO.STR; disc: arg > PSXPORT_TOMBA2_DISC > PSXPORT_DISC, env or ./.env)\n",
            argv[0]);
    return 2;
  }
  const std::string outdir   = argv[1];
  const char* movie          = argc > 2 ? argv[2] : "MOVIE/LOGO.STR";
  const char* disc_arg       = argc > 3 ? argv[3] : nullptr;

  // --- disc path: explicit arg > env > ./.env (never call set_prefix — cfg.cpp reads these full names) ---
  std::string disc = disc_arg ? disc_arg : lucent::config::text("PSXPORT_TOMBA2_DISC");
  if (disc.empty()) disc = lucent::config::text("PSXPORT_DISC");
  if (disc.empty()) disc = dotenv("PSXPORT_TOMBA2_DISC");
  if (disc.empty()) disc = dotenv("PSXPORT_DISC");
  if (disc.empty()) {
    lucent::error("fmv", "no disc image: pass one as argv[3], or set PSXPORT_TOMBA2_DISC / PSXPORT_DISC (env or ./.env)");
    return 2;
  }
  if (!disc_open(disc)) return 1;

  // --- resolve the movie on the ISO ---
  uint32_t lba = 0, size = 0;
  if (!iso_find_file(movie, &lba, &size)) {
    lucent::error("fmv", "{} not found on disc", movie);
    return 1;
  }
  lucent::info("fmv", "{} -> LBA {}, {} bytes", movie, lba, size);

  // --- output dirs ---
  std::filesystem::create_directories(outdir + "/frames");
  FILE* wav = fopen((outdir + "/audio.wav").c_str(), "wb");
  if (!wav) { lucent::error("fmv", "cannot write {}/audio.wav", outdir); return 1; }
  for (int i = 0; i < 44; i++) fputc(0, wav);
  FILE* idx = fopen((outdir + "/index.txt").c_str(), "wb");
  if (!idx) { lucent::error("fmv", "cannot write {}/index.txt", outdir); return 1; }
  fprintf(idx, "# fmv_export manifest: %s @ LBA %u, %u bytes\n", movie, lba, size);
  fprintf(idx, "# str_frame  w  h  chunks  payload_bytes  codes  samples_played  time_ms\n");

  // --- scratch (same caps as runtime/recomp/native_fmv.cpp playLba) ---
  uint8_t*  payload = (uint8_t*)malloc(FMV_PAYLOAD_BYTES);
  uint16_t* codes   = (uint16_t*)malloc(FMV_CODES_MAX * 2);
  uint16_t* pixels  = (uint16_t*)malloc(FMV_MAX_PIXELS * 2);
  int16_t*  xa_pcm  = (int16_t*)malloc(4032 * 2 * 2);
  uint8_t*  rgb     = (uint8_t*)malloc(1024u * 512u * 3);   // RGB24 scratch, VRAM-sized

  int max_frames = (int)lucent::config::number("PSXPORT_FMV_MAXFRAMES", 0);
  mdec_init();

  uint32_t nsectors = (size + SECTOR_USER - 1) / SECTOR_USER;
  int frames = 0, cur_frame = -1;
  uint32_t paylen = 0;
  int fwidth = 320, fheight = 240;
  int expected_chunks = 0, got_chunks = 0;
  int xa_freq = 37800;
  int16_t xa_hist[2][2] = {{0, 0}, {0, 0}};
  long media_frames = 0;                       // cumulative audio sample-pairs = media clock
  uint8_t raw[2352];

  for (uint32_t sec = 0; sec < nsectors; sec++) {
    if (!disc_read_raw(lba + sec, raw, 2352)) break;
    int submode = raw[18];

    if (submode & 0x04) {                       // XA-ADPCM audio sector
      int fq = xa_freq;
      int n = xa_decode_sector(raw, xa_pcm, xa_hist, &fq);
      if (media_frames == 0) xa_freq = fq;
      else if (fq != xa_freq) lucent::warn("fmv", "audio rate changed {}Hz -> {}Hz mid-movie (LBA {})", xa_freq, fq, lba + sec);
      for (int i = 0; i < n; i++) { wav_u16(wav, (uint16_t)xa_pcm[2 * i]); wav_u16(wav, (uint16_t)xa_pcm[2 * i + 1]); }
      media_frames += n;
      continue;
    }

    const uint8_t* sbuf = raw + 24;             // Form1 video user data
    uint16_t magic = (uint16_t)(sbuf[0] | (sbuf[1] << 8));
    if (magic != 0x0160) continue;              // not a video data sector (padding)

    int chunk_idx = sbuf[4]  | (sbuf[5]  << 8);
    int nchunks   = sbuf[6]  | (sbuf[7]  << 8);
    int framenum  = sbuf[8]  | (sbuf[9]  << 8) | (sbuf[10] << 16) | (sbuf[11] << 24);
    int w         = sbuf[16] | (sbuf[17] << 8);
    int h         = sbuf[18] | (sbuf[19] << 8);

    if (chunk_idx == 0) {
      cur_frame = framenum; paylen = 0;
      expected_chunks = nchunks; got_chunks = 0;
      fwidth = w ? w : 320; fheight = h ? h : 240;
    }
    if (cur_frame != framenum) continue;        // out of sync; wait for next chunk-0

    uint32_t plen = SECTOR_USER - SUBHDR_LEN;
    if (paylen + plen <= FMV_PAYLOAD_BYTES) {
      memcpy(payload + paylen, sbuf + SUBHDR_LEN, plen);
      paylen += plen;
    }
    got_chunks++;

    if (expected_chunks > 0 && got_chunks >= expected_chunks) {
      int ncodes = bs_decode_frame(payload, paylen, fwidth, fheight, codes, (int)FMV_CODES_MAX);
      lucent::debug("fmv", "frame {}: {}x{}, {} payload bytes, {} codes, {} samples elapsed",
                    framenum, fwidth, fheight, paylen, ncodes, media_frames);
      if (cfg_on("PSXPORT_FMV_DUMPCODES")) {
        fprintf(stderr, "[fmv] frame %d first codes:", framenum);
        for (int i = 0; i < ncodes && i < 24; i++) fprintf(stderr, " %04x", codes[i]);
        fprintf(stderr, "\n");
        fprintf(stderr, "[fmv] frame %d payload first 32 bytes:", framenum);
        for (int i = 0; i < 32 && i < (int)paylen; i++) fprintf(stderr, " %02x", payload[i]);
        fprintf(stderr, "\n");
      }
      if (ncodes > 0) {
        int np = mdec_decode_to_rgb555(codes, ncodes, fwidth, fheight, pixels);
        if (np > 0) {
          if (cfg_on("PSXPORT_FMV_DUMPCODES")) {
            fprintf(stderr, "[fmv] frame %d first 555 px:", framenum);
            for (int i = 0; i < 32; i++) fprintf(stderr, " %04x", pixels[i]);
            fprintf(stderr, "\n");
          }
          for (int i = 0; i < fwidth * fheight; i++) {
            uint16_t p = pixels[i];
            rgb[3 * i + 0] = (uint8_t)(((p >> 0) & 31) << 3);    // PSX 16bpp: R = bits 0-4
            rgb[3 * i + 1] = (uint8_t)(((p >> 5)  & 31) << 3);   // G = bits 5-9
            rgb[3 * i + 2] = (uint8_t)(((p >> 10) & 31) << 3);   // B = bits 10-14
          }
          char pngpath[512];
          snprintf(pngpath, sizeof pngpath, "%s/frames/frame_%05d.png", outdir.c_str(), frames + 1);
          if (!png_write(pngpath, fwidth, fheight, rgb)) { fclose(wav); fclose(idx); return 1; }
          frames++;
          fprintf(idx, "%d  %d  %d  %d  %u  %d  %ld  %lld\n",
                  frames, framenum, fwidth, fheight, expected_chunks, paylen, ncodes, media_frames,
                  (long long)(media_frames * 1000 / (xa_freq ? xa_freq : 37800)));
        }
      }
      cur_frame = -1; expected_chunks = 0; got_chunks = 0; paylen = 0;
      if (max_frames && frames >= max_frames) break;
      if (frames % 25 == 0)
        lucent::info("fmv", "{} frames, {} audio samples ({:.2f}s @ {}Hz)", frames, media_frames,
                     (double)media_frames / (xa_freq ? xa_freq : 37800), xa_freq);
    }
  }

  long datab = media_frames * 4;
  fseek(wav, 0, SEEK_SET);
  fputs("RIFF", wav); wav_u32(wav, 36 + (uint32_t)datab); fputs("WAVE", wav);
  fputs("fmt ", wav); wav_u32(wav, 16); wav_u16(wav, 1); wav_u16(wav, 2);
  wav_u32(wav, (uint32_t)xa_freq); wav_u32(wav, (uint32_t)xa_freq * 4); wav_u16(wav, 4); wav_u16(wav, 16);
  fputs("data", wav); wav_u32(wav, (uint32_t)datab);
  fclose(wav);
  fclose(idx);

  lucent::info("fmv", "done: {} video frames -> {}/frames, {} audio sample-pairs -> {}/audio.wav "
                      "({:.2f}s @ {}Hz); manifest -> {}/index.txt",
               frames, outdir, media_frames, outdir, (double)media_frames / (xa_freq ? xa_freq : 37800),
               xa_freq, outdir);
  return 0;
}
