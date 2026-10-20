// The .STR decode the runtime player and tools/fmv_export share: STR demux, BS VLC decode, MDEC frame decode.
#include "fmv_decode.h"
#include "cd_position.h"
#include "diagnostic_config.h"
#include "mdec_device.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <lucent/log.h>

namespace psx::movie {
namespace {

constexpr uint16_t kVideoChunkMagic = 0x0160;
constexpr int kDefaultWidth = 320;
constexpr int kDefaultHeight = 240;

uint32_t le16(const uint8_t *p) {
  return p[0] | (p[1] << 8);
}

// BS stores little-endian 16-bit units whose bits are consumed MSB-first.
class BitReader {
public:
  BitReader(const uint8_t *data, uint32_t size) : data_(data), size_(size) {}

  uint32_t peek(int n) {
    refill();
    if (n == 0) {
      return 0;
    }
    return (bits_ >> (32 - n)) & static_cast<uint32_t>((1ull << n) - 1);
  }
  void skip(int n) {
    bits_ <<= n;
    count_ -= n;
  }
  uint32_t get(int n) {
    const uint32_t v = peek(n);
    skip(n);
    return v;
  }
  bool atEnd() const {
    return count_ <= 0 && pos_ + 1 >= size_;
  }

private:
  void refill() {
    while (count_ <= 16 && pos_ + 1 < size_) {
      const uint32_t w = le16(data_ + pos_);
      pos_ += 2;
      bits_ |= w << (16 - count_);
      count_ += 16;
    }
  }

  const uint8_t *data_;
  uint32_t size_;
  uint32_t pos_ = 0;
  uint32_t bits_ = 0; // left-aligned
  int count_ = 0;
};

struct Vlc {
  uint8_t len;
  uint16_t code;
  uint8_t run;
  uint8_t level;
};

// MPEG-1 Table B-14, which BS v2 uses verbatim (from jpsxdec AC_VARIABLE_LENGTH_CODES_MPEG1); a sign bit follows.
constexpr Vlc kAcCodes[] = {
    {2, 0x00003, 0, 1},   // 11
    {3, 0x00003, 1, 1},   // 011
    {4, 0x00004, 0, 2},   // 0100
    {4, 0x00005, 2, 1},   // 0101
    {5, 0x00005, 0, 3},   // 00101
    {5, 0x00006, 4, 1},   // 00110
    {5, 0x00007, 3, 1},   // 00111
    {6, 0x00004, 7, 1},   // 000100
    {6, 0x00005, 6, 1},   // 000101
    {6, 0x00006, 1, 2},   // 000110
    {6, 0x00007, 5, 1},   // 000111
    {7, 0x00004, 2, 2},   // 0000100
    {7, 0x00005, 9, 1},   // 0000101
    {7, 0x00006, 0, 4},   // 0000110
    {7, 0x00007, 8, 1},   // 0000111
    {8, 0x00020, 13, 1},  // 00100000
    {8, 0x00021, 0, 6},   // 00100001
    {8, 0x00022, 12, 1},  // 00100010
    {8, 0x00023, 11, 1},  // 00100011
    {8, 0x00024, 3, 2},   // 00100100
    {8, 0x00025, 1, 3},   // 00100101
    {8, 0x00026, 0, 5},   // 00100110
    {8, 0x00027, 10, 1},  // 00100111
    {10, 0x00008, 16, 1}, // 0000001000
    {10, 0x00009, 5, 2},  // 0000001001
    {10, 0x0000a, 0, 7},  // 0000001010
    {10, 0x0000b, 2, 3},  // 0000001011
    {10, 0x0000c, 1, 4},  // 0000001100
    {10, 0x0000d, 15, 1}, // 0000001101
    {10, 0x0000e, 14, 1}, // 0000001110
    {10, 0x0000f, 4, 2},  // 0000001111
    {12, 0x00010, 0, 11}, // 000000010000
    {12, 0x00011, 8, 2},  // 000000010001
    {12, 0x00012, 4, 3},  // 000000010010
    {12, 0x00013, 0, 10}, // 000000010011
    {12, 0x00014, 2, 4},  // 000000010100
    {12, 0x00015, 7, 2},  // 000000010101
    {12, 0x00016, 21, 1}, // 000000010110
    {12, 0x00017, 20, 1}, // 000000010111
    {12, 0x00018, 0, 9},  // 000000011000
    {12, 0x00019, 19, 1}, // 000000011001
    {12, 0x0001a, 18, 1}, // 000000011010
    {12, 0x0001b, 1, 5},  // 000000011011
    {12, 0x0001c, 3, 3},  // 000000011100
    {12, 0x0001d, 0, 8},  // 000000011101
    {12, 0x0001e, 6, 2},  // 000000011110
    {12, 0x0001f, 17, 1}, // 000000011111
    {13, 0x00010, 10, 2}, // 0000000010000
    {13, 0x00011, 9, 2},  // 0000000010001
    {13, 0x00012, 5, 3},  // 0000000010010
    {13, 0x00013, 3, 4},  // 0000000010011
    {13, 0x00014, 2, 5},  // 0000000010100
    {13, 0x00015, 1, 7},  // 0000000010110
    {13, 0x00016, 1, 6},  // 0000000010111
    {13, 0x00017, 0, 15}, // 0000000010111
    {13, 0x00018, 0, 14}, // 0000000011000
    {13, 0x00019, 0, 13}, // 0000000011001
    {13, 0x0001a, 0, 12}, // 0000000011010
    {13, 0x0001b, 26, 1}, // 0000000011011
    {13, 0x0001c, 25, 1}, // 0000000011100
    {13, 0x0001d, 24, 1}, // 0000000011101
    {13, 0x0001e, 23, 1}, // 0000000011110
    {13, 0x0001f, 22, 1}, // 0000000011111
    {14, 0x00010, 0, 31}, // 00000000010000
    {14, 0x00011, 0, 30}, // 00000000010001
    {14, 0x00012, 0, 29}, // 00000000010010
    {14, 0x00013, 0, 28}, // 00000000010011
    {14, 0x00014, 0, 27}, // 00000000010100
    {14, 0x00015, 0, 26}, // 00000000010101
    {14, 0x00016, 0, 25}, // 00000000010110
    {14, 0x00017, 0, 24}, // 00000000010111
    {14, 0x00018, 0, 23}, // 00000000011000
    {14, 0x00019, 0, 22}, // 00000000011001
    {14, 0x0001a, 0, 21}, // 00000000011010
    {14, 0x0001b, 0, 20}, // 00000000011011
    {14, 0x0001c, 0, 19}, // 00000000011100
    {14, 0x0001d, 0, 18}, // 00000000011101
    {14, 0x0001e, 0, 17}, // 00000000011110
    {14, 0x0001f, 0, 16}, // 00000000011111
    {15, 0x00010, 0, 40}, // 000000000010000
    {15, 0x00011, 0, 39}, // 000000000010001
    {15, 0x00012, 0, 38}, // 000000000010010
    {15, 0x00013, 0, 37}, // 000000000010011
    {15, 0x00014, 0, 36}, // 000000000010100
    {15, 0x00015, 0, 35}, // 000000000010101
    {15, 0x00016, 0, 34}, // 000000000010110
    {15, 0x00017, 0, 33}, // 000000000010111
    {15, 0x00018, 0, 32}, // 000000000011000
    {15, 0x00019, 1, 14}, // 000000000011001
    {15, 0x0001a, 1, 13}, // 000000000011010
    {15, 0x0001b, 1, 12}, // 000000000011011
    {15, 0x0001c, 1, 11}, // 000000000011100
    {15, 0x0001d, 1, 10}, // 000000000011101
    {15, 0x0001e, 1, 9},  // 000000000011110
    {15, 0x0001f, 1, 8},  // 000000000011111
    {16, 0x00010, 1, 18}, // 0000000000010000
    {16, 0x00011, 1, 17}, // 0000000000010001
    {16, 0x00012, 1, 16}, // 0000000000010010
    {16, 0x00013, 1, 15}, // 0000000000010011
    {16, 0x00014, 6, 3},  // 0000000000010100
    {16, 0x00015, 16, 2}, // 0000000000010101
    {16, 0x00016, 15, 2}, // 0000000000010110
    {16, 0x00017, 14, 2}, // 0000000000010111
    {16, 0x00018, 13, 2}, // 0000000000011000
    {16, 0x00019, 12, 2}, // 0000000000011001
    {16, 0x0001a, 11, 2}, // 0000000000011010
    {16, 0x0001b, 31, 1}, // 0000000000011011
    {16, 0x0001c, 30, 1}, // 0000000000011100
    {16, 0x0001d, 29, 1}, // 0000000000011101
    {16, 0x0001e, 28, 1}, // 0000000000011110
    {16, 0x0001f, 27, 1}, // 0000000000011111
};

struct AcCode {
  enum class Kind : std::uint8_t { EndOfBlock, Pair, Bad };
  Kind kind = Kind::Bad;
  int run = 0;
  int level = 0;
};

// End of block is "10"; escape "000001" carries a 6-bit run and a 10-bit signed level.
AcCode decodeAc(BitReader &b) {
  if (b.peek(2) == 0x2) {
    b.skip(2);
    return {.kind = AcCode::Kind::EndOfBlock};
  }
  if (b.peek(6) == 0x1) {
    b.skip(6);
    const int r = static_cast<int>(b.get(6));
    int l = static_cast<int>(b.get(10));
    if (l & 0x200) {
      l -= 0x400;
    }
    return {.kind = AcCode::Kind::Pair, .run = r, .level = l};
  }
  for (int n = 1; n <= 16; n++) {
    const uint32_t v = b.peek(n);
    for (const Vlc &vlc : kAcCodes) {
      if (vlc.len == n && vlc.code == v) {
        b.skip(n);
        const int level = vlc.level;
        return {.kind = AcCode::Kind::Pair, .run = vlc.run, .level = b.get(1) != 0 ? -level : level};
      }
    }
  }
  return {};
}

// The MPEG-1 intra matrix DecDCTReset uploads, in raster order.
constexpr uint8_t kQuantRaster[64] = {
    2,  16, 19, 22, 26, 27, 29, 34, 16, 16, 22, 24, 27, 29, 34, 37, 19, 22, 26, 27, 29, 34,
    34, 38, 22, 22, 26, 27, 29, 34, 37, 40, 22, 26, 27, 29, 32, 35, 40, 48, 26, 27, 29, 32,
    35, 40, 48, 58, 26, 27, 29, 34, 38, 46, 56, 69, 27, 29, 35, 38, 46, 56, 69, 83,
};

// mdec.c dequantises in scan order, so the matrix is uploaded zigzagged.
constexpr uint8_t kZigZag[64] = {
    0x00, 0x08, 0x01, 0x02, 0x09, 0x10, 0x18, 0x11, 0x0a, 0x03, 0x04, 0x0b, 0x12, 0x19, 0x20, 0x28,
    0x21, 0x1a, 0x13, 0x0c, 0x05, 0x06, 0x0d, 0x14, 0x1b, 0x22, 0x29, 0x30, 0x38, 0x31, 0x2a, 0x23,
    0x1c, 0x15, 0x0e, 0x07, 0x0f, 0x16, 0x1d, 0x24, 0x2b, 0x32, 0x39, 0x3a, 0x33, 0x2c, 0x25, 0x1e,
    0x17, 0x1f, 0x26, 0x2d, 0x34, 0x3b, 0x3c, 0x35, 0x2e, 0x27, 0x2f, 0x36, 0x3d, 0x3e, 0x37, 0x3f,
};

// Decode-macroblock depth bits [28:27]: 2 = 24bpp, 3 bytes per pixel.
constexpr uint32_t kDepth24bpp = 2;
constexpr int kBytesPerPixel = 3;
constexpr int kMacroblockPixels = 16;
constexpr std::size_t kMacroblockBytes = std::size_t{kMacroblockPixels} * kMacroblockPixels * kBytesPerPixel;
constexpr std::size_t kMaxInputWords = std::size_t{128} * 1024;
constexpr std::size_t kMaxOutputWords = std::size_t{512} * 1024;

void resetForDma(MdecDevice &mdec) {
  mdec.write(MdecDevice::kControlPort, 0x80000000u);
  mdec.write(MdecDevice::kControlPort, (1u << 30) | (1u << 29)); // DMA in and out enabled
}

} // namespace

std::optional<VideoChunk> videoChunk(const uint8_t *raw) {
  const uint8_t *header = raw + 24;
  if (psx::cd::sectorSubheader(raw).audio() || le16(header) != kVideoChunkMagic) {
    return std::nullopt;
  }
  return VideoChunk{static_cast<int>(le16(header + 4)),
                    static_cast<int>(le16(header + 6)),
                    static_cast<int>(le16(header + 8) | (le16(header + 10) << 16)),
                    static_cast<int>(le16(header + 16)),
                    static_cast<int>(le16(header + 18))};
}

std::optional<FrameAssembler::Frame> FrameAssembler::add(const VideoChunk &chunk, const uint8_t *raw) {
  if (payload_.empty()) {
    payload_.resize(kPayloadBytes);
  }
  if (chunk.chunk == 0) {
    frame_ = chunk.frame;
    length_ = 0;
    expected_ = chunk.chunks;
    got_ = 0;
    size_ = {chunk.width != 0 ? chunk.width : kDefaultWidth, chunk.height != 0 ? chunk.height : kDefaultHeight};
  }
  if (frame_ != chunk.frame) {
    return std::nullopt;
  }
  constexpr uint32_t kChunkPayload = psx::cd::kDataSectorBytes - kStrChunkHeaderBytes;
  if (length_ + kChunkPayload <= kPayloadBytes) {
    std::memcpy(payload_.data() + length_, raw + 24 + kStrChunkHeaderBytes, kChunkPayload);
    length_ += kChunkPayload;
  }
  got_++;
  if (expected_ <= 0 || got_ < expected_) {
    return std::nullopt;
  }
  const Frame frame{{payload_.data(), length_}, frame_, size_, expected_};
  reset();
  return frame;
}

int decodeBsFrame(std::span<const uint8_t> payloadBytes, present::PixelSize frame, std::span<uint16_t> codes) {
  const uint8_t *payload = payloadBytes.data();
  const auto payloadSize = static_cast<uint32_t>(payloadBytes.size());
  const auto maxCodes = static_cast<int>(codes.size());
  const bool dcOnly = psx::config::movie_dc_only();
  if (payloadSize < 8) {
    return -1;
  }
  int qscale = static_cast<int>(le16(payload + 4) & 0x3F);
  if (qscale == 0) {
    qscale = 1;
  }
  lucent::debug("fmv",
                "BS hdr: nwords={} magic={:04x} qscale={} version={}",
                le16(payload),
                le16(payload + 2),
                qscale,
                le16(payload + 6));

  BitReader br(payload + 8, payloadSize - 8);
  const int blocks = ((frame.width + 15) / 16) * ((frame.height + 15) / 16) * 6;
  int out = 0;
  for (int blk = 0; blk < blocks; blk++) {
    if (br.atEnd()) {
      break;
    }
    // Each block opens with a signed 10-bit DC; the MDEC word carries QScale in [15:10].
    int dc = static_cast<int>(br.get(10));
    if (dc & 0x200) {
      dc -= 0x400;
    }
    if (out >= maxCodes) {
      return out;
    }
    codes[out++] = static_cast<uint16_t>(((qscale & 0x3F) << 10) | (dc & 0x3FF));
    for (;;) {
      const AcCode ac = decodeAc(br);
      if (ac.kind == AcCode::Kind::EndOfBlock) {
        if (out >= maxCodes) {
          return out;
        }
        codes[out++] = 0xFE00;
        break;
      }
      if (ac.kind == AcCode::Kind::Bad) {
        return out > 0 ? out : -2;
      }
      if (out >= maxCodes) {
        return out;
      }
      // DC-only consumes the AC bits to stay in sync but drops the code.
      if (!dcOnly) {
        codes[out++] = static_cast<uint16_t>(((ac.run & 0x3F) << 10) | (ac.level & 0x3FF));
      }
    }
  }
  return out;
}

// mdec.c keeps (int16)uploaded >> 3, so the BIOS cosine table is round(basis * 2^15) (u = 0 entries 0x5A82).
FrameDecoder::FrameDecoder() {
  for (int i = 0; i < 64; i++) {
    quantWords_[i / 4 % 16] |= static_cast<uint32_t>(kQuantRaster[kZigZag[i]]) << (8 * (i % 4));
  }
  for (int i = 0; i < 16; i++) {
    quantWords_[16 + i] = quantWords_[i];
  }
  int16_t idct[64];
  for (int u = 0; u < 8; u++) {
    for (int x = 0; x < 8; x++) {
      const double cu = u == 0 ? 1.0 / std::sqrt(2.0) : 1.0;
      const double v = cu * std::cos((2.0 * x + 1.0) * u * M_PI / 16.0);
      const long iv = std::lround(v * 32768.0);
      idct[u * 8 + x] = static_cast<int16_t>(iv > 32767 ? 32767 : (iv < -32768 ? -32768 : iv));
    }
  }
  for (std::size_t i = 0; i < idctWords_.size(); i++) {
    idctWords_[i] =
        static_cast<uint16_t>(idct[2 * i]) | (static_cast<uint32_t>(static_cast<uint16_t>(idct[2 * i + 1])) << 16);
  }
}

// After power-on both matrices are zero and every block decodes black (libpress DecDCTReset uploads them).
void FrameDecoder::start(MdecDevice &mdec) const {
  resetForDma(mdec);
  mdec.write(MdecDevice::kDataPort, (2u << 29) | 1u); // SetIqTab, luma and chroma
  mdec.dmaIn(quantWords_.data(), static_cast<int>(quantWords_.size()));
  mdec.step();
  mdec.write(MdecDevice::kDataPort, 3u << 29); // SetIdctTab
  mdec.dmaIn(idctWords_.data(), static_cast<int>(idctWords_.size()));
  mdec.step();
}

// The InFIFO holds 0x20 words and drops the rest, so input and output ping-pong as DMA0/DMA1 do. A decoder
// that parks with input outstanding ends the frame with the blocks it produced.
int FrameDecoder::decodeMacroblocks(MdecDevice &mdec, std::span<const uint16_t> codes, present::PixelSize frame) {
  const auto ncodes = static_cast<int>(codes.size());
  resetForDma(mdec);
  const int nwords = (ncodes + 1) / 2;
  mdec.write(MdecDevice::kDataPort, 0x30000000u | (kDepth24bpp << 27) | static_cast<uint32_t>(nwords & 0xFFFF));
  if (nwords > static_cast<int>(kMaxInputWords)) {
    return -1;
  }
  in_.resize(static_cast<std::size_t>(nwords));
  for (int i = 0; i < nwords; i++) {
    const uint16_t lo = codes[std::size_t{2} * i];
    const uint16_t hi = (i * 2 + 1 < ncodes) ? codes[std::size_t{2} * i + 1] : 0xFE00;
    in_[i] = static_cast<uint32_t>(lo) | (static_cast<uint32_t>(hi) << 16);
  }
  const int totalWords = frame.width * frame.height * kBytesPerPixel / 4;
  if (totalWords > static_cast<int>(kMaxOutputWords)) {
    return -2;
  }
  out_.resize(static_cast<std::size_t>(totalWords));
  int fed = 0;
  int got = 0;
  int stall = 0;
  while (true) {
    const int before = fed + got;
    if (fed < nwords && mdec.canWrite()) {
      fed += mdec.dmaIn(&in_[fed], std::min(nwords - fed, 0x10));
    }
    mdec.step();
    got += got < totalWords ? mdec.dmaOut(out_.data() + got, totalWords - got) : 0;
    if (fed + got == before) {
      if (++stall >= 4) {
        break;
      }
    } else {
      stall = 0;
    }
    if (got >= totalWords && fed >= nwords) {
      break;
    }
  }
  const int scattered = got;
  got += mdec.dmaOutRest(out_.data() + got, totalWords - got);
  lucent::debug("fmv", "  drain: {} scattered + {} tail = {}/{} words", scattered, got - scattered, got, totalWords);
  return got;
}

// Macroblocks leave the MDEC column-major: emit index k is row k % rows, column k / rows. A block the decode
// did not produce is black.
int FrameDecoder::decode(
    MdecDevice &mdec, std::span<const uint16_t> codes, present::PixelSize frame, FramePixels layout, uint8_t *pixels) {
  const int got = decodeMacroblocks(mdec, codes, frame);
  if (got < 0) {
    return got;
  }
  const int width = frame.width;
  const int height = frame.height;
  const auto outBytes = static_cast<std::size_t>(layout);
  const auto *mb = reinterpret_cast<const uint8_t *>(out_.data());
  const int columns = (width + 15) / 16;
  const int rows = (height + 15) / 16;
  const int produced = static_cast<int>(static_cast<std::size_t>(got) * 4 / kMacroblockBytes);
  for (int blk = 0; blk < columns * rows; blk++) {
    const int bx = blk / rows;
    const int by = blk % rows;
    const int fx = bx * kMacroblockPixels;
    const int span = std::min(kMacroblockPixels, width - fx);
    for (int yy = 0; yy < kMacroblockPixels && by * kMacroblockPixels + yy < height; yy++) {
      uint8_t *dst = pixels + (static_cast<std::size_t>(by * kMacroblockPixels + yy) * width + fx) * outBytes;
      if (blk >= produced) {
        std::memset(dst, 0, static_cast<std::size_t>(span) * outBytes);
        if (layout == FramePixels::Rgba32) {
          for (int x = 0; x < span; x++) {
            dst[std::size_t{4} * x + 3] = 255;
          }
        }
        continue;
      }
      const uint8_t *row = mb + static_cast<std::size_t>(blk) * kMacroblockBytes +
                           static_cast<std::size_t>(yy) * kMacroblockPixels * kBytesPerPixel;
      if (layout == FramePixels::Rgb24) {
        std::memcpy(dst, row, static_cast<std::size_t>(span) * kBytesPerPixel);
      } else {
        for (int x = 0; x < span; x++) {
          dst[std::size_t{4} * x + 0] = row[std::size_t{3} * x + 0];
          dst[std::size_t{4} * x + 1] = row[std::size_t{3} * x + 1];
          dst[std::size_t{4} * x + 2] = row[std::size_t{3} * x + 2];
          dst[std::size_t{4} * x + 3] = 255;
        }
      }
    }
  }
  return width * height;
}

} // namespace psx::movie
