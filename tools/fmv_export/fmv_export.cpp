// fmv_export: offline .STR export, no game boot, through the runtime player's own decode.
//
// Output (all under <out-dir>):
//   frames/frame_NNNNN.png   one RGB24 PNG per video frame
//   audio.wav                the movie's XA-ADPCM soundtrack, S16 stereo
//   index.txt                per-video-frame manifest (STR frame, samples, media time)
//
// run: build/<dir>/tools/fmv_export/fmv_export <out-dir> [movie-path] [disc.chd]
//   disc: the argument, else PSXPORT_DISC (env or ./.env)
//   PSXPORT_FMV_MAXFRAMES  bounds the number of video frames dumped (0/unset = all)
//   PSXPORT_FMV_DUMPCODES  prints each frame's first codes and payload bytes
//   PSXPORT_DEBUG=fmv      per-frame decode detail
#include <stdint.h>
#include <stdio.h>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <lucent/log.h>

#include "cd_position.h"
#include "diagnostic_config.h"
#include "disc.h"
#include "fmv_decode.h"
#include "image_writer.h"
#include "mdec_device.h"
#include "wav_writer.h"
#include "xa_adpcm.h"

namespace {

constexpr std::size_t kMaxPixels = std::size_t{1024} * 512; // VRAM-sized frame

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

  std::filesystem::create_directories(outdir + "/frames");
  FILE *idx = fopen((outdir + "/index.txt").c_str(), "wb");
  if (!idx) {
    lucent::error("fmv", "cannot write {}/index.txt", outdir);
    return 1;
  }
  fprintf(idx, "# fmv_export manifest: %s @ LBA %u, %u bytes\n", movie, lba, size);
  fprintf(idx, "# output_frame  str_frame  w  h  chunks  payload_bytes  codes  samples_played  time_ms\n");

  MdecDevice mdec;
  mdec.bind();
  psx::movie::FrameAssembler assembler;
  psx::movie::FrameDecoder decoder;
  std::vector<uint16_t> codes(psx::movie::kMaxCodes);
  std::vector<uint8_t> rgb(kMaxPixels * 3);
  std::vector<int16_t> xa_pcm(std::size_t{psx::audio::kXaMaxSectorFrames} * 2);
  // The WAV opens at the first audio sector's rate.
  std::optional<psx::audio::WavWriter> wav;
  const long max_frames = psx::config::movie_max_frames();
  const bool dump_codes = psx::config::movie_dump_codes();

  const uint32_t nsectors = (size + psx::movie::kSectorUserBytes - 1) / psx::movie::kSectorUserBytes;
  int frames = 0;
  int xa_freq = psx::audio::kXaDefaultRateHz;
  int16_t xa_hist[2][2] = {{0, 0}, {0, 0}};
  long media_frames = 0; // audio sample-pairs: the media clock
  uint8_t raw[psx::cd::kRawSectorBytes];

  for (uint32_t sec = 0; sec < nsectors; sec++) {
    if (!disc.readRaw(lba + sec, raw, sizeof raw)) {
      break;
    }
    if (psx::audio::sectorSubheader(raw).audio()) {
      const psx::audio::XaFormat format = psx::audio::decodeXaSector(raw, xa_pcm.data(), xa_hist);
      if (!wav) {
        xa_freq = format.rate;
        wav.emplace(outdir + "/audio.wav", static_cast<uint32_t>(xa_freq));
        if (!wav->isOpen()) {
          fclose(idx);
          return 1;
        }
      } else if (format.rate != xa_freq) {
        lucent::warn("fmv", "audio rate changed {}Hz -> {}Hz mid-movie (LBA {})", xa_freq, format.rate, lba + sec);
      }
      wav->append(xa_pcm.data(), static_cast<uint32_t>(format.frames));
      media_frames += format.frames;
      continue;
    }
    const std::optional<psx::movie::VideoChunk> chunk = psx::movie::videoChunk(raw);
    if (!chunk) {
      continue;
    }
    const std::optional<psx::movie::FrameAssembler::Frame> frame = assembler.add(*chunk, raw);
    if (!frame) {
      continue;
    }
    const int width = frame->size.width;
    const int height = frame->size.height;
    const int ncodes = psx::movie::decodeBsFrame(frame->payload, frame->size, codes);
    lucent::debug("fmv",
                  "frame {}: {}x{}, {} payload bytes, {} codes, {} samples elapsed",
                  frame->number,
                  width,
                  height,
                  frame->payload.size(),
                  ncodes,
                  media_frames);
    if (dump_codes) {
      fprintf(stderr, "[fmv] frame %d first codes:", frame->number);
      for (int i = 0; i < ncodes && i < 24; i++) {
        fprintf(stderr, " %04x", codes[i]);
      }
      fprintf(stderr, "\n[fmv] frame %d payload first 32 bytes:", frame->number);
      for (std::size_t i = 0; i < 32 && i < frame->payload.size(); i++) {
        fprintf(stderr, " %02x", frame->payload[i]);
      }
      fprintf(stderr, "\n");
    }
    if (ncodes <= 0 ||
        decoder.decodeRgb888(mdec, {codes.data(), static_cast<std::size_t>(ncodes)}, frame->size, rgb.data()) <= 0) {
      continue;
    }
    char pngpath[512];
    snprintf(pngpath, sizeof pngpath, "%s/frames/frame_%05d.png", outdir.c_str(), frames + 1);
    if (!psx::debug::writeImageRgb24(pngpath, rgb.data(), width, height)) {
      fclose(idx);
      return 1;
    }
    frames++;
    fprintf(idx,
            "%d  %d  %d  %d  %d  %zu  %d  %ld  %lld\n",
            frames,
            frame->number,
            width,
            height,
            frame->chunks,
            frame->payload.size(),
            ncodes,
            media_frames,
            static_cast<long long>(media_frames * 1000 / xa_freq));
    if (max_frames && frames >= max_frames) {
      break;
    }
    if (frames % 25 == 0) {
      lucent::info("fmv",
                   "{} frames, {} audio samples ({:.2f}s @ {}Hz)",
                   frames,
                   media_frames,
                   static_cast<double>(media_frames) / xa_freq,
                   xa_freq);
    }
  }
  if (!wav) {
    wav.emplace(outdir + "/audio.wav", static_cast<uint32_t>(xa_freq));
  }
  fclose(idx);
  lucent::info("fmv",
               "done: {} video frames -> {}/frames, {} audio sample-pairs -> {}/audio.wav "
               "({:.2f}s @ {}Hz); manifest -> {}/index.txt",
               frames,
               outdir,
               media_frames,
               outdir,
               static_cast<double>(media_frames) / xa_freq,
               xa_freq,
               outdir);
  return 0;
}
