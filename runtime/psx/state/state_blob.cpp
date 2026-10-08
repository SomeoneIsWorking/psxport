#include "state_blob.h"

#include <cstring>

namespace psx::state {

void BlobWriter::u8(std::uint8_t v) {
  mBytes.push_back(v);
}

void BlobWriter::u16(std::uint16_t v) {
  u8(static_cast<std::uint8_t>(v & 0xFFu));
  u8(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
}

void BlobWriter::u32(std::uint32_t v) {
  u16(static_cast<std::uint16_t>(v & 0xFFFFu));
  u16(static_cast<std::uint16_t>((v >> 16) & 0xFFFFu));
}

void BlobWriter::u64(std::uint64_t v) {
  u32(static_cast<std::uint32_t>(v & 0xFFFFFFFFu));
  u32(static_cast<std::uint32_t>((v >> 32) & 0xFFFFFFFFu));
}

void BlobWriter::bytes(std::span<const std::uint8_t> v) {
  mBytes.insert(mBytes.end(), v.begin(), v.end());
}

void BlobWriter::blob(std::span<const std::uint8_t> v) {
  u32(static_cast<std::uint32_t>(v.size()));
  bytes(v);
}

std::uint8_t BlobReader::u8() {
  if (!mOk) {
    return 0;
  }
  if (mOffset >= mBytes.size()) {
    fail();
    return 0;
  }
  return mBytes[mOffset++];
}

std::uint16_t BlobReader::u16() {
  const std::uint16_t low = u8();
  const std::uint16_t high = u8();
  return static_cast<std::uint16_t>(low | (high << 8));
}

std::uint32_t BlobReader::u32() {
  const std::uint32_t low = u16();
  const std::uint32_t high = u16();
  return low | (high << 16);
}

std::uint64_t BlobReader::u64() {
  const std::uint64_t low = u32();
  const std::uint64_t high = u32();
  return low | (high << 32);
}

bool BlobReader::boolean() {
  return u8() != 0u;
}

bool BlobReader::blobRef(std::span<std::uint8_t> out) {
  const std::uint32_t length = u32();
  if (!mOk) {
    return false;
  }
  // Subtract before adding: `length + mOffset` can wrap and then pass the bound.
  if (length > mBytes.size() - mOffset || out.size() != length) {
    fail();
    return false;
  }
  if (!out.empty()) {
    std::memcpy(out.data(), mBytes.data() + mOffset, out.size());
  }
  mOffset += out.size();
  return true;
}

bool BlobReader::bytes(std::span<std::uint8_t> out) {
  if (!mOk || out.size() > mBytes.size() - mOffset) {
    fail();
    return false;
  }
  if (!out.empty()) {
    std::memcpy(out.data(), mBytes.data() + mOffset, out.size());
  }
  mOffset += out.size();
  return true;
}

bool BlobReader::blob(std::vector<std::uint8_t> &out) {
  const std::uint32_t length = u32();
  if (!mOk) {
    return false;
  }
  if (length > mBytes.size() - mOffset) {
    fail();
    return false;
  }
  out.assign(mBytes.begin() + static_cast<std::ptrdiff_t>(mOffset),
             mBytes.begin() + static_cast<std::ptrdiff_t>(mOffset + length));
  mOffset += length;
  return true;
}

} // namespace psx::state