// native_fmv.h — class Fmv — the native .STR movie player, owned by Game (`c->game->fmv`,
// back-pointer wired in Game()). Implemented in native_fmv.cpp: STR demux -> BS VLC decode ->
// MDEC IDCT/YCbCr (Beetle mdec.c) -> VRAM upload + present, plus the interleaved XA-ADPCM audio
// sectors through a dedicated host audio stream at the XA rate (video paced to the media clock).
#pragma once
#include <cstdint>
class Game;

class Fmv {
public:
  Game* game = nullptr;
  int start_prev = 0;   // Start was down on the previously polled frame (skip edge-detect) (was s_fmv_start_prev)

  // play(path): resolve `path` on the disc (ISO9660) and play it. Returns frames played, -1 if
  //   the path can't be resolved. playLba is the by-extent entry the sequencers use.
  int play(const char* path);
  int playLba(uint32_t lba, uint32_t size_bytes);

  ~Fmv();

private:
  // host audio sink: SDL3 push-model stream bound to the default playback device, opened at the
  // movie's XA rate. Held opaquely so this header stays SDL-free.
  void* stream = nullptr;         // SDL_AudioStream* (was s_fmv_stream)
  int   stream_freq = 0;          // rate the stream was opened at (was s_fmv_freq)
  void audioOpen(int freq);
  void audioQueue(const int16_t* pcm, int frames);
  void audioClose();
  // Pace playback to the AUDIO/media clock; polls input, returns 1 if Start was pressed (skip).
  int  pace(long media_frames, int freq, uint32_t t0, int uncapped);
  // Decode an entire BS frame into the MDEC run-level code stream (VLC decode).
  int  bsDecodeFrame(const uint8_t* payload, uint32_t payload_size,
                     int width, int height, uint16_t* codes, int max_codes);
  // Feed the MDEC (16bpp) and extract/tile the RGB555 frame.
  int  mdecDecodeToRgb555(const uint16_t* codes, int ncodes,
                          int width, int height, uint16_t* pixels);

  // decode scratch, heap-allocated on first play (multi-MB; was file-scope static buffers)
  uint8_t*  payload_buf = nullptr;   // concatenated BS payload (512 KB)
  uint16_t* codes_buf   = nullptr;   // MDEC run-level code stream (512K codes)
  uint16_t* pixels_buf  = nullptr;   // decoded RGB555 frame (1024x512)
  uint32_t* inbuf       = nullptr;   // MDEC input words (128K words)
  uint32_t* outbuf      = nullptr;   // MDEC output words (512K words)
  int16_t*  xa_pcm      = nullptr;   // one sector's decoded stereo PCM (4032 frames)
  int bs_hdr_logged = 0;             // [fmv] BS-header debug print latch
  int dconly = -1;                   // PSXPORT_FMV_DCONLY cache (-1 = unread)
};
