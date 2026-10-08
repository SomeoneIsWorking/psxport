// state_blob.h — the little-endian scalar codec every state SECTION is written through.
//
// A section is a flat byte blob with no schema of its own: the reader and the writer are the same
// owner (one device), so the layout is a private contract between them, and the FILE only has to
// find a section by name. Making the format self-describing here would mean two descriptions of
// every device's state, and they would drift.
//
// Every read is BOUNDED and every short read is recorded: `BlobReader::ok()` goes false the first
// time a field does not fit, and the caller refuses the whole state rather than continuing with a
// half-populated machine. A truncated state file that leaves "the rest of the fields at their
// power-on values" is indistinguishable from a correct one at every call site that reads a field.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace psx::state {

class BlobWriter {
public:
  void u8(std::uint8_t v);
  void u16(std::uint16_t v);
  void u32(std::uint32_t v);
  void u64(std::uint64_t v);
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
  void bytes(std::span<const std::uint8_t> v);
  // A length-prefixed byte run, so an empty run and a missing run cannot be confused.
  void blob(std::span<const std::uint8_t> v);
  // A length-prefixed POD array of `sizeof(T)`-sized elements.
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

// Reads what a BlobWriter wrote. Every accessor returns a value ONLY while the run is intact; the
// first short read latches the failure and every later accessor returns zero, so a caller that
// forgets to check `ok()` gets zeros rather than uninitialised memory (and `ok()` is checked once,
// by the section owner, before anything is restored).
class BlobReader {
public:
  explicit BlobReader(std::span<const std::uint8_t> bytes) : mBytes(bytes) {}

  [[nodiscard]] bool ok() const {
    return mOk;
  }
  std::size_t offset() const {
    return mOffset;
  }
  // Bytes this reader was given, and how many it has consumed — the two numbers a short-answer
  // diagnostic must publish so "the rest was zero" cannot be read as "the rest was zero in the
  // file".
  [[nodiscard]] std::size_t size() const {
    return mBytes.size();
  }

  std::uint8_t u8();
  std::uint16_t u16();
  std::uint32_t u32();
  std::uint64_t u64();
  std::int8_t i8() {
    return static_cast<std::int8_t>(u8());
  }
  std::int32_t i32() {
    return static_cast<std::int32_t>(u32());
  }
  std::int64_t i64() {
    return static_cast<std::int64_t>(u64());
  }
  bool boolean();
  bool bytes(std::span<std::uint8_t> out);
  bool blob(std::vector<std::uint8_t> &out);
  template <class T> bool array(T *values, std::size_t count) {
    return blobRef(std::span<std::uint8_t>(reinterpret_cast<std::uint8_t *>(values), count * sizeof(T)));
  }
  template <class T> T value() {
    T v{};
    array(&v, 1);
    return v;
  }

private:
  bool blobRef(std::span<std::uint8_t> out);
  void fail() {
    mOk = false;
  }

  std::span<const std::uint8_t> mBytes;
  std::size_t mOffset = 0;
  bool mOk = true;
};

} // namespace psx::state