// The little-endian byte codec: save-state sections, pad recordings and oracle snapshots.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace psx::platform {

class BlobWriter {
public:
  void u8(std::uint8_t v) {
    mBytes.push_back(v);
  }
  void u16(std::uint16_t v) {
    u8(static_cast<std::uint8_t>(v & 0xFFu));
    u8(static_cast<std::uint8_t>(v >> 8));
  }
  void u32(std::uint32_t v) {
    u16(static_cast<std::uint16_t>(v & 0xFFFFu));
    u16(static_cast<std::uint16_t>(v >> 16));
  }
  void u64(std::uint64_t v) {
    u32(static_cast<std::uint32_t>(v & 0xFFFFFFFFu));
    u32(static_cast<std::uint32_t>(v >> 32));
  }
  void i8(std::int8_t v) {
    u8(static_cast<std::uint8_t>(v));
  }
  void i32(std::int32_t v) {
    u32(static_cast<std::uint32_t>(v));
  }
  void i64(std::int64_t v) {
    u64(static_cast<std::uint64_t>(v));
  }
  void boolean(bool v) {
    u8(v ? 1u : 0u);
  }
  void bytes(std::span<const std::uint8_t> v) {
    mBytes.insert(mBytes.end(), v.begin(), v.end());
  }
  // Length-prefixed, so an empty run and a missing run differ.
  void blob(std::span<const std::uint8_t> v) {
    u32(static_cast<std::uint32_t>(v.size()));
    bytes(v);
  }
  template <class T> void array(const T *values, std::size_t count) {
    blob(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(values), count * sizeof(T)));
  }
  template <class T> void value(const T &v) {
    array(&v, 1);
  }
  const std::vector<std::uint8_t> &bytesOut() const {
    return mBytes;
  }
  std::vector<std::uint8_t> take() {
    return std::move(mBytes);
  }

private:
  std::vector<std::uint8_t> mBytes;
};

// Bounded reads: the first short read latches !ok() and every later read returns zero.
class BlobReader {
public:
  explicit BlobReader(std::span<const std::uint8_t> bytes) : mBytes(bytes) {}

  [[nodiscard]] bool ok() const {
    return mOk;
  }
  std::size_t offset() const {
    return mOffset;
  }
  [[nodiscard]] std::size_t size() const {
    return mBytes.size();
  }

  std::uint8_t u8() {
    if (!mOk || mOffset >= mBytes.size()) {
      mOk = false;
      return 0;
    }
    return mBytes[mOffset++];
  }
  std::uint16_t u16() {
    const std::uint16_t low = u8();
    const std::uint16_t high = u8();
    return static_cast<std::uint16_t>(low | (high << 8));
  }
  std::uint32_t u32() {
    const std::uint32_t low = u16();
    const std::uint32_t high = u16();
    return low | (high << 16);
  }
  std::uint64_t u64() {
    const std::uint64_t low = u32();
    const std::uint64_t high = u32();
    return low | (high << 32);
  }
  std::int8_t i8() {
    return static_cast<std::int8_t>(u8());
  }
  std::int32_t i32() {
    return static_cast<std::int32_t>(u32());
  }
  std::int64_t i64() {
    return static_cast<std::int64_t>(u64());
  }
  bool boolean() {
    return u8() != 0u;
  }
  bool bytes(std::span<std::uint8_t> out) {
    if (!mOk || out.size() > mBytes.size() - mOffset) {
      mOk = false;
      return false;
    }
    copyOut(out);
    return true;
  }
  bool blob(std::vector<std::uint8_t> &out) {
    const std::uint32_t length = u32();
    if (!mOk || length > mBytes.size() - mOffset) {
      mOk = false;
      return false;
    }
    out.assign(mBytes.begin() + static_cast<std::ptrdiff_t>(mOffset),
               mBytes.begin() + static_cast<std::ptrdiff_t>(mOffset + length));
    mOffset += length;
    return true;
  }
  template <class T> bool array(T *values, std::size_t count) {
    return blobRef(std::span<std::uint8_t>(reinterpret_cast<std::uint8_t *>(values), count * sizeof(T)));
  }
  template <class T> T value() {
    T v{};
    array(&v, 1);
    return v;
  }

  // Reads the section's layout byte; false with `error` set when it is not `version`.
  bool expectLayout(std::string_view section, std::uint8_t version, std::string &error) {
    const std::uint8_t layout = u8();
    if (mOk && layout == version) {
      return true;
    }
    error = std::string(section) + " section layout " + std::to_string(layout) + ", this build writes " +
            std::to_string(version);
    return false;
  }
  // False with `error` set when a read ran short.
  bool decoded(std::string_view section, std::string &error) const {
    if (mOk) {
      return true;
    }
    error = "the " + std::string(section) + " section is " + std::to_string(mBytes.size()) +
            " bytes and did not decode (" + std::to_string(mOffset) + " consumed)";
    return false;
  }

private:
  bool blobRef(std::span<std::uint8_t> out) {
    const std::uint32_t length = u32();
    // Subtract before comparing: `length + mOffset` can wrap.
    if (!mOk || length > mBytes.size() - mOffset || out.size() != length) {
      mOk = false;
      return false;
    }
    copyOut(out);
    return true;
  }
  void copyOut(std::span<std::uint8_t> out) {
    if (!out.empty()) {
      std::memcpy(out.data(), mBytes.data() + mOffset, out.size());
    }
    mOffset += out.size();
  }

  std::span<const std::uint8_t> mBytes;
  std::size_t mOffset = 0;
  bool mOk = true;
};

} // namespace psx::platform
