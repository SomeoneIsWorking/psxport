// state_bytes.h — a producer state as a flat byte string: trivially copyable records and arrays.
#pragma once

#include <lucent/log.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>
#include <type_traits>
#include <vector>

namespace psx::present {

class StateWriter {
public:
  template <class T> void put(const T &value) {
    static_assert(std::is_trivially_copyable_v<T>);
    const auto *bytes = reinterpret_cast<const std::byte *>(&value);
    bytes_.insert(bytes_.end(), bytes, bytes + sizeof(T));
  }
  template <class T> void putAll(std::span<const T> values) {
    static_assert(std::is_trivially_copyable_v<T>);
    const auto *bytes = reinterpret_cast<const std::byte *>(values.data());
    bytes_.insert(bytes_.end(), bytes, bytes + values.size_bytes());
  }
  std::span<const std::byte> bytes() const {
    return bytes_;
  }

private:
  std::vector<std::byte> bytes_;
};

// Reads what a StateWriter wrote, in the same order; a state shorter than what it names aborts.
class StateReader {
public:
  explicit StateReader(std::span<const std::byte> bytes) : bytes_(bytes) {}

  template <class T> T get() {
    static_assert(std::is_trivially_copyable_v<T>);
    T value;
    take(&value, sizeof(T));
    return value;
  }
  template <class T> std::vector<T> getAll(std::size_t count) {
    static_assert(std::is_trivially_copyable_v<T>);
    std::vector<T> values(count);
    take(values.data(), count * sizeof(T));
    return values;
  }

private:
  void take(void *out, std::size_t size) {
    if (size > bytes_.size() - at_) {
      lucent::error("state-bytes", "a saved state of {} bytes is read past its end", bytes_.size());
      std::abort();
    }
    if (size != 0) {
      std::memcpy(out, bytes_.data() + at_, size);
    }
    at_ += size;
  }

  std::span<const std::byte> bytes_;
  std::size_t at_ = 0;
};

} // namespace psx::present
