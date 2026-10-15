// fmv_export — offline .STR (FMV) exporter: PNG frames, an XA WAV and an index manifest, no game boot.
//
// Reads the disc through psx::cd::Disc and decodes with the runtime player's own psx::movie functions.
//
// Output (all under <out-dir>):
//   frames/frame_NNNNN.png   one RGB24 PNG per video frame
//   audio.wav                the movie's XA-ADPCM soundtrack, S16 stereo
//   index.txt                per-video-frame manifest (STR frame, samples, media time)
//
// run: build/<dir>/tools/fmv_export/fmv_export <out-dir> [movie-path] [disc.chd]
//   disc: the argument, else PSXPORT_DISC (env or ./.env)
//   PSXPORT_FMV_MAXFRAMES  bounds the number of video frames dumped (0/unset = all)
//   PSXPORT_DEBUG=fmv      per-frame decode detail
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <filesystem>
#include <optional>
#include <string>

#include <lucent/config.h>
#include <lucent/log.h>

#include "c_subsys.h"          // mdec_init
#include "diagnostic_config.h" // movie_dump_codes (PSXPORT_FMV_DUMPCODES)
#include "disc.h"
#include "fmv_decode.h"
#include "image_writer.h"

namespace {

constexpr std::uint32_t kSectorUser = 2048u;
constexpr std::uint32_t kStrHeaderBytes = 32u;
constexpr std::size_t kMaxPixels = std::size_t{1024} * 512; // RGB555 frame scratch, VRAM-sized

// ====================================================================================
// WAV writer — S16 stereo PCM
// ====================================================================================
void wav_u32(FILE *f, uint32_t v) {
  fputc(static_cast<int>(v & 255), f);
  fputc(static_cast<int>((v >> 8) & 255), f);
  fputc(static_cast<int>((v >> 16) & 255), f);
  fputc(static_cast<int>((v >> 24) & 255), f);
}
void wav_u16(FILE *f, uint16_t v) {
  fputc(static_cast<int>(v & 255), f);
  fputc(static_cast<int>((v >> 8) & 255), f);
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr,
            "usage: %s <out-dir> [movie-path] [disc.chd]\n"
            "       (movie-path default: MOVIE/LOGO.STR; disc: arg > PSXPORT_DISC, env or ./.env)\n",
            argv[0]);
    return 2;
  }
  const std::string outdir = argv[1];
  const char *movie = argc > 2 ? argv[2] : "MOVIE/LOGO.STR";
  const char *disc_arg = argc > 3 ? argv[3] : nullptr;

  psx::cd::Disc disc;
  if (!(disc_arg ? disc.openImage(disc_arg) : disc.open())) {
    return 1;
  }
  const std::optional<psx::cd::DiscFile> file = disc.findFile(movie);
  if (!file) {
    lucent::error("fmv", "{} not found on disc", movie);
    return 1;
  }
  const std::uint32_t lba = file->lba;
  const std::uint32_t size = file->size;
  lucent::info("fmv", "{} -> LBA {}, {} bytes", movie, lba, size);

  // --- output dirs ---
  std::filesystem::create_directories(outdir + "/frames");
  FILE *wav = fopen((outdir + "/audio.wav").c_str(), "wb");
  if (!wav) {
    lucent::error("fmv", "cannot write {}/audio.wav", outdir);
    return 1;
  }
  for (int i = 0; i < 44; i++) {
    fputc(0, wav);
  }
  FILE *idx = fopen((outdir + "/index.txt").c_str(), "wb");
  if (!idx) {
    lucent::error("fmv", "cannot write {}/index.txt", outdir);
    return 1;
  }
  fprintf(idx, "# fmv_export manifest: %s @ LBA %u, %u bytes\n", movie, lba, size);
  fprintf(idx, "# output_frame  str_frame  w  h  chunks  payload_bytes  codes  samples_played  time_ms\n");

  // --- scratch (same caps as runtime/psx/movie/native_fmv.cpp playLba) ---
  uint8_t *payload = (uint8_t *)malloc(psx::movie::kPayloadBytes);
  uint16_t *codes = (uint16_t *)malloc(psx::movie::kMaxCodes * sizeof(uint16_t));
  uint16_t *pixels = (uint16_t *)malloc(kMaxPixels * sizeof(uint16_t));
  int16_t *xa_pcm = (int16_t *)malloc(std::size_t{4032} * 2 * sizeof(int16_t));
  uint8_t *rgb = (uint8_t *)malloc(std::size_t{1024} * 512 * 3); // RGB24 scratch, VRAM-sized

  int max_frames = (int)lucent::config::number("PSXPORT_FMV_MAXFRAMES", 0);
  mdec_init();

  uint32_t nsectors = (size + kSectorUser - 1) / kSectorUser;
  int frames = 0, cur_frame = -1;
  uint32_t paylen = 0;
  int fwidth = 320, fheight = 240;
  int expected_chunks = 0, got_chunks = 0;
  int xa_freq = 37800;
  int16_t xa_hist[2][2] = {{0, 0}, {0, 0}};
  long media_frames = 0; // cumulative audio sample-pairs = media clock
  uint8_t raw[2352];

  for (uint32_t sec = 0; sec < nsectors; sec++) {
    if (!disc.readRaw(lba + sec, raw, 2352)) {
      break;
    }
    int submode = raw[18];

    if (submode & 0x04) { // XA-ADPCM audio sector
      int fq = xa_freq;
      int n = psx::movie::decodeXaSector(raw, xa_pcm, xa_hist, &fq);
      if (media_frames == 0) {
        xa_freq = fq;
      } else if (fq != xa_freq) {
        lucent::warn("fmv", "audio rate changed {}Hz -> {}Hz mid-movie (LBA {})", xa_freq, fq, lba + sec);
      }
      for (int i = 0; i < n; i++) {
        wav_u16(wav, (uint16_t)xa_pcm[std::size_t{2} * i]);
        wav_u16(wav, (uint16_t)xa_pcm[std::size_t{2} * i + 1]);
      }
      media_frames += n;
      continue;
    }

    const uint8_t *sbuf = raw + 24; // Form1 video user data
    uint16_t magic = (uint16_t)(sbuf[0] | (sbuf[1] << 8));
    if (magic != 0x0160) {
      continue; // not a video data sector (padding)
    }

    int chunk_idx = sbuf[4] | (sbuf[5] << 8);
    int nchunks = sbuf[6] | (sbuf[7] << 8);
    int framenum = sbuf[8] | (sbuf[9] << 8) | (sbuf[10] << 16) | (sbuf[11] << 24);
    int w = sbuf[16] | (sbuf[17] << 8);
    int h = sbuf[18] | (sbuf[19] << 8);

    if (chunk_idx == 0) {
      cur_frame = framenum;
      paylen = 0;
      expected_chunks = nchunks;
      got_chunks = 0;
      fwidth = w ? w : 320;
      fheight = h ? h : 240;
    }
    if (cur_frame != framenum) {
      continue; // out of sync; wait for next chunk-0
    }

    uint32_t plen = kSectorUser - kStrHeaderBytes;
    if (paylen + plen <= psx::movie::kPayloadBytes) {
      memcpy(payload + paylen, sbuf + kStrHeaderBytes, plen);
      paylen += plen;
    }
    got_chunks++;

    if (expected_chunks > 0 && got_chunks >= expected_chunks) {
      int ncodes = psx::movie::decodeBsFrame({payload, paylen}, {fwidth, fheight}, {codes, psx::movie::kMaxCodes});
      lucent::debug("fmv",
                    "frame {}: {}x{}, {} payload bytes, {} codes, {} samples elapsed",
                    framenum,
                    fwidth,
                    fheight,
                    paylen,
                    ncodes,
                    media_frames);
      if (psx::config::movie_dump_codes()) {
        fprintf(stderr, "[fmv] frame %d first codes:", framenum);
        for (int i = 0; i < ncodes && i < 24; i++) {
          fprintf(stderr, " %04x", codes[i]);
        }
        fprintf(stderr, "\n");
        fprintf(stderr, "[fmv] frame %d payload first 32 bytes:", framenum);
        for (int i = 0; i < 32 && i < (int)paylen; i++) {
          fprintf(stderr, " %02x", payload[i]);
        }
        fprintf(stderr, "\n");
      }
      if (ncodes > 0) {
        int np = psx::movie::decodeMdecToRgb555({codes, static_cast<std::size_t>(ncodes)}, {fwidth, fheight}, pixels);
        if (np > 0) {
          if (psx::config::movie_dump_codes()) {
            fprintf(stderr, "[fmv] frame %d first 555 px:", framenum);
            for (int i = 0; i < 32; i++) {
              fprintf(stderr, " %04x", pixels[i]);
            }
            fprintf(stderr, "\n");
          }
          for (int i = 0; i < fwidth * fheight; i++) {
            uint16_t p = pixels[i];
            rgb[3 * i + 0] = (uint8_t)(((p >> 0) & 31) << 3);  // PSX 16bpp: R = bits 0-4
            rgb[3 * i + 1] = (uint8_t)(((p >> 5) & 31) << 3);  // G = bits 5-9
            rgb[3 * i + 2] = (uint8_t)(((p >> 10) & 31) << 3); // B = bits 10-14
          }
          char pngpath[512];
          snprintf(pngpath, sizeof pngpath, "%s/frames/frame_%05d.png", outdir.c_str(), frames + 1);
          if (!psx::debug::writeImageRgb24(pngpath, rgb, fwidth, fheight)) {
            fclose(wav);
            fclose(idx);
            return 1;
          }
          frames++;
          fprintf(idx,
                  "%d  %d  %d  %d  %d  %u  %d  %ld  %lld\n",
                  frames,
                  framenum,
                  fwidth,
                  fheight,
                  expected_chunks,
                  paylen,
                  ncodes,
                  media_frames,
                  (long long)(media_frames * 1000 / (xa_freq ? xa_freq : 37800)));
        }
      }
      cur_frame = -1;
      expected_chunks = 0;
      got_chunks = 0;
      paylen = 0;
      if (max_frames && frames >= max_frames) {
        break;
      }
      if (frames % 25 == 0) {
        lucent::info("fmv",
                     "{} frames, {} audio samples ({:.2f}s @ {}Hz)",
                     frames,
                     media_frames,
                     (double)media_frames / (xa_freq ? xa_freq : 37800),
                     xa_freq);
      }
    }
  }

  long datab = media_frames * 4;
  fseek(wav, 0, SEEK_SET);
  fputs("RIFF", wav);
  wav_u32(wav, 36 + (uint32_t)datab);
  fputs("WAVE", wav);
  fputs("fmt ", wav);
  wav_u32(wav, 16);
  wav_u16(wav, 1);
  wav_u16(wav, 2);
  wav_u32(wav, (uint32_t)xa_freq);
  wav_u32(wav, (uint32_t)xa_freq * 4);
  wav_u16(wav, 4);
  wav_u16(wav, 16);
  fputs("data", wav);
  wav_u32(wav, (uint32_t)datab);
  fclose(wav);
  fclose(idx);

  lucent::info("fmv",
               "done: {} video frames -> {}/frames, {} audio sample-pairs -> {}/audio.wav "
               "({:.2f}s @ {}Hz); manifest -> {}/index.txt",
               frames,
               outdir,
               media_frames,
               outdir,
               (double)media_frames / (xa_freq ? xa_freq : 37800),
               xa_freq,
               outdir);
  return 0;
}
