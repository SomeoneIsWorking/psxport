// frame_state.h — producer object states, by the scope that saved them and by the record that drew them.
#pragma once

#include "frame_record.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace psx::present {

// A producer's object: the producer's guest address and the object's, with the title's generation
// folded into `object` where the guest reuses an address.
struct ObjectId {
  std::uint32_t producer = 0;
  std::uint32_t object = 0;

  bool operator==(const ObjectId &) const = default;
};

// The states of the objects one record draws. An object whose packets came from two scopes, or whose
// scope saved twice, names no single state and is ambiguous.
class FrameState {
public:
  // The object's state, nullopt when it has none or is ambiguous.
  std::optional<std::span<const std::byte>> find(ObjectId id) const;
  bool empty() const {
    return saved_.empty();
  }

private:
  friend class FrameStates;
  struct Saved {
    std::uint32_t serial = 0;
    std::size_t offset = 0;
    std::size_t size = 0;
    bool ambiguous = false;
  };
  static std::uint64_t indexKey(ObjectId id) {
    return (std::uint64_t{id.producer} << 32) | id.object;
  }
  void add(ObjectId id, std::uint32_t serial, std::span<const std::byte> state, bool ambiguous);

  std::unordered_map<std::uint64_t, Saved> saved_;
  std::vector<std::byte> bytes_;
};

// The states producers saved, each under the scope whose packets it describes. A packet is often built in
// one logic frame and walked in the next, so a state is kept for a few frames and collected by the record
// that draws its packets.
class FrameStates {
public:
  static constexpr std::uint32_t kRetainedFrames = 4;

  // Saves `state` for the scope `owner` (EmissionScope::current()).
  void save(const RecordKey &owner, std::span<const std::byte> state);
  template <class T> void save(const RecordKey &owner, const T &state) {
    static_assert(std::is_trivially_copyable_v<T>);
    save(owner, std::span<const std::byte>(std::as_bytes(std::span<const T, 1>(&state, 1))));
  }
  // The states of the objects `record` draws, by the serial each keyed primitive carries.
  FrameState collect(const FrameRecord &record) const;
  // A logic frame ended; states older than kRetainedFrames are dropped.
  void endFrame();
  // A savestate load: nothing saved so far describes the machine.
  void clear();

private:
  struct Saved {
    ObjectId id;
    std::uint32_t frame = 0;
    bool ambiguous = false;
    std::vector<std::byte> bytes;
  };
  std::unordered_map<std::uint32_t, Saved> bySerial_;
  std::uint32_t frame_ = 0;
};

// Aborts: a state was read as a type of another size than it was saved as.
[[noreturn]] void stateSizeMismatch(std::size_t saved, std::size_t read);

// The state as `T`; `bytes` must be a state saved as `T`.
template <class T> T stateAs(std::span<const std::byte> bytes) {
  static_assert(std::is_trivially_copyable_v<T>);
  if (bytes.size() != sizeof(T)) {
    stateSizeMismatch(bytes.size(), sizeof(T));
  }
  T value;
  std::memcpy(&value, bytes.data(), sizeof(T));
  return value;
}

} // namespace psx::present
