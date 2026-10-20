// The .STR decode machinery shared by the runtime player (native_fmv.cpp) and tools/fmv_export.
#pragma once
#include "pixel_size.h"
#include "xa_adpcm.h"

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <stdint.h>
#include <vector>

class MdecDevice;

namespace psx::movie {

inline constexpr std::size_t kPayloadBytes = std::size_t{512} * 1024; // one frame's concatenated BS payload
inline constexpr std::size_t kMaxCodes = std::size_t{512} * 1024;     // MDEC run-level codes per frame
inline constexpr uint32_t kStrChunkHeaderBytes = 32u;
inline constexpr std::size_t kMaxFramePixels = std::size_t{1024} * 512; // VRAM-sized frame

// One STR video chunk's 32-byte header: its index and count within the frame, the frame number and size.
struct VideoChunk {
  int chunk = 0;
  int chunks = 0;
  int frame = 0;
  int width = 0;
  int height = 0;
};
// Nullopt for an audio sector or padding.
std::optional<VideoChunk> videoChunk(const uint8_t *raw);

// Concatenates one frame's chunk payloads; a chunk of another frame waits for the next chunk 0.
class FrameAssembler {
public:
  struct Frame {
    std::span<const uint8_t> payload; // valid until the next add()
    int number = 0;
    present::PixelSize size;
    int chunks = 0;
  };
  std::optional<Frame> add(const VideoChunk &chunk, const uint8_t *raw);
  // Drop a partly assembled frame.
  void reset() {
    frame_ = -1;
    expected_ = 0;
    got_ = 0;
    length_ = 0;
  }

private:
  std::vector<uint8_t> payload_;
  uint32_t length_ = 0;
  int frame_ = -1;
  int expected_ = 0;
  int got_ = 0;
  present::PixelSize size_{320, 240};
};

// A BS frame (8-byte header + VLC bitstream) to the MDEC run-level code stream; returns codes written, negative
// on error.
int decodeBsFrame(std::span<const uint8_t> payload, present::PixelSize frame, std::span<uint16_t> codes);

enum class FramePixels : uint8_t { Rgb24 = 3, Rgba32 = 4 };

// Runs a code stream through the bound MDEC at 24bpp and tiles it straight into the caller's pixel layout.
class FrameDecoder {
public:
  FrameDecoder();
  // Uploads the quant and IDCT tables once per movie; a control reset keeps them (Beetle mdec.c).
  void start(MdecDevice &mdec) const;
  // Returns width*height, or negative on error.
  int decode(
      MdecDevice &mdec, std::span<const uint16_t> codes, present::PixelSize frame, FramePixels layout, uint8_t *pixels);

private:
  int decodeMacroblocks(MdecDevice &mdec, std::span<const uint16_t> codes, present::PixelSize frame);

  std::array<uint32_t, 32> quantWords_{};
  std::array<uint32_t, 32> idctWords_{};
  std::vector<uint32_t> in_;
  std::vector<uint32_t> out_;
};

} // namespace psx::movie
