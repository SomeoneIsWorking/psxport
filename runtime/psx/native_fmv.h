// native_fmv.h — class Fmv — the native .STR movie player, owned by Game (`c->game->fmv`,
// back-pointer wired in Game()). Implemented in native_fmv.cpp: STR demux -> BS VLC decode ->
// MDEC IDCT/YCbCr (Beetle mdec.c) -> present, plus the interleaved XA-ADPCM audio sectors through a
// dedicated host audio stream at the XA rate (video paced to the media clock).
//
// STEPPED, NOT BLOCKING. A movie is played by begin() + one step() per HOST TURN, each presenting at
// most one frame. Playing a whole movie inside one turn looked simpler and was wrong three ways: it
// starved the frame loop, the control channel and window events for the movie's whole duration; a
// Start press could not reach the pad while it ran, so the skip this class implements could not be
// exercised at all; and nothing could observe a movie frame. One frame per turn costs a handful of
// instructions and makes all three true.
#pragma once
#include <cstdint>
#include <vector>
class Core;
class Game;

class Fmv {
public:
  Game *game = nullptr;

  // Open the movie at `lba` for `size_bytes` and arm playback. Returns false when the extent cannot
  // be opened, which leaves the player finished (a caller then answers as the retail player would for
  // a movie that never played).
  bool begin(uint32_t lba, uint32_t size_bytes);
  // Decode and present AT MOST ONE frame, then return. Ends the movie (end of stream, or a Start
  // press) and closes the audio stream; finished() says which happened.
  void step();
  bool finished() const {
    return finished_;
  }
  // True between begin() and the movie's end. A movie presents its own frame as a native image
  // from inside the guest call, so whoever else presents the guest's VRAM for that field would show
  // the guest's layers instead of the movie: this is the query that lets a title's frame driver put
  // the movie back on top once the host has presented.
  bool presenting() const {
    return active_;
  }
  // Present the movie's most recent frame again, over whatever the host presented. A movie frame is
  // delivered from inside the guest call, which is BEFORE the host's own present for the same field,
  // so a title that also presents guest layers each field must re-put the movie last or the guest's
  // layers cover it.
  void replayLastFrame(Core &core);
  bool skipped() const {
    return skipped_;
  }
  int framesPlayed() const {
    return frames_;
  }

  // Resolve `path` on the disc (ISO9660) and OPEN it, without playing: the caller then drives
  // step(). This is the entry for a title whose guest can be interrupted mid-movie; play() below is
  // the blocking convenience built on it.
  bool beginPath(const char *path);
  // play(path): resolve and play to completion. Returns frames played, -1 when the path cannot be
  // resolved. These block the calling turn on purpose: they are for callers with nothing else to do
  // until the movie ends (the boot movie).
  int play(const char *path);
  int playLba(uint32_t lba, uint32_t size_bytes);

  ~Fmv();

private:
  // host audio sink: SDL3 push-model stream bound to the default playback device, opened at the
  // movie's XA rate. Held opaquely so this header stays SDL-free.
  void *stream = nullptr; // SDL_AudioStream* (was s_fmv_stream)
  int stream_freq = 0;    // rate the stream was opened at (was s_fmv_freq)
  void audioOpen(int freq);
  void audioQueue(const int16_t *pcm, int frames);
  void audioClose();
  // The blocking loop behind play()/playLba(): pumps host input and steps until the movie ends.
  int playToEnd();
  // Pace playback to the AUDIO/media clock; returns 1 if Start is held (skip).
  int pace(long media_frames, int freq, uint32_t t0, int uncapped);
  // Decode an entire BS frame into the MDEC run-level code stream (VLC decode).
  int bsDecodeFrame(
      const uint8_t *payload, uint32_t payload_size, int width, int height, uint16_t *codes, int max_codes);
  // Present one decoded 24-bit frame.
  void presentFrame(const uint8_t *pixels, int width, int height);
  // Allocate the multi-megabyte decode scratch on first use.
  void ensureScratch();
  // End the movie: endAtEndOfStream is the ordinary end (stream exhausted, or the dev cap), and
  // endWithSkip is the guest asking out with Start. Both are idempotent.
  void endAtEndOfStream();
  void endWithSkip();

  // decode scratch, heap-allocated on first play (multi-MB; was file-scope static buffers).
  // The MDEC in/out word buffers live inside fmv_decode.cpp (function-static), since the
  // offline tools use the same decode entry points.
  uint8_t *payload_buf = nullptr;  // concatenated BS payload (512 KB)
  uint16_t *codes_buf = nullptr;   // MDEC run-level code stream (512K codes)
  uint8_t *pixels24_buf = nullptr; // decoded RGB888 frame (1024x512 px, 3 bytes each)
  // RGBA expansion scratch for presentation, owned here rather than as a function-local static
  // (tools/cpp_policy.py).
  std::vector<uint8_t> rgba_scratch_;
  int last_frame_width_ = 0; // the frame replayLastFrame() re-presents; 0 until the first frame
  int last_frame_height_ = 0;
  int16_t *xa_pcm = nullptr; // one sector's decoded stereo PCM (4032 frames)

  // --- playback cursors, so step() can stop between frames -------------------------------------
  uint32_t base_lba_ = 0;
  uint32_t sector_count_ = 0;
  uint32_t sector_ = 0;
  int frame_in_progress_ = -1; // STR frame number currently being assembled
  uint32_t payload_len_ = 0;
  int expected_chunks_ = 0;
  int got_chunks_ = 0;
  int frame_width_ = 320;
  int frame_height_ = 240;
  long media_frames_ = 0; // cumulative audio sample-pairs = the media clock
  int xa_freq_ = 37800;
  int16_t xa_hist_[2][2] = {{0, 0}, {0, 0}};
  uint32_t clock_origin_ms_ = 0;
  int max_frames_ = 0; // PSXPORT_FMV_MAXFRAMES dev cap; 0 = play the whole movie
  bool uncapped_ = false;
  bool finished_ = true;
  bool active_ = false;
  bool skipped_ = false;
  int frames_ = 0;
  bool start_held_at_begin_ = false; // Start already down when the movie opened: needs a release first
};
