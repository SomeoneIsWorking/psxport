// fmv_decode.h — the pure .STR decode machinery, shared by the runtime FMV player (native_fmv.cpp) and the
// offline tools (tools/fmv_export), so a tool dump runs the same code the runtime runs.
#pragma once
#include "pixel_size.h"

#include <cstddef>
#include <span>
#include <stdint.h>

namespace psx::movie {

inline constexpr std::size_t kPayloadBytes = std::size_t{512} * 1024; // one frame's concatenated BS payload
inline constexpr std::size_t kMaxCodes = std::size_t{512} * 1024;     // MDEC run-level codes per frame

// Decode an entire BS frame (8-byte BS header + VLC bitstream) into the MDEC run-level code
// stream for a `frame`-sized picture. Returns number of 16-bit codes written, or negative on error.
int decodeBsFrame(std::span<const uint8_t> payload, present::PixelSize frame, std::span<uint16_t> codes);

// Feed the MDEC (16bpp) with the code stream and extract/tile the RGB555 frame into `pixels`
// (width*height entries). Uploads the quant + IDCT tables first. Returns width*height, or
// negative on error.
int decodeMdecToRgb555(std::span<const uint16_t> codes, present::PixelSize frame, uint16_t *pixels);

// Same at MDEC depth 2 (24bpp): `pixels` receives width*height RGB888 triples, three bytes per
// pixel in R,G,B order — the layout mdec.c's EncodeRow24 writes and the layout the PSX 24-bit
// display samples. Returns width*height, or negative on error.
int decodeMdecToRgb888(std::span<const uint16_t> codes, present::PixelSize frame, uint8_t *pixels);

// An XA-ADPCM sector's format, from its subheader coding byte.
struct XaFormat {
  int rate = 0;   // Hz
  int frames = 0; // PCM frames one sector decodes to
};
XaFormat xaSectorFormat(const uint8_t *raw);

// One raw 2352-byte XA-ADPCM sector to interleaved S16 stereo; returns frames written.
int decodeXaSector(const uint8_t *raw, int16_t *out, int16_t hist[2][2], int *freq);

} // namespace psx::movie
