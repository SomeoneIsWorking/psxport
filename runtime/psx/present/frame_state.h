// frame_state.h — producer object states, by the scope that saved them and by the record that drew them.
#pragma once

#include "frame_record.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <type_traits>
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
// scope saved twice, names no single state and is ambiguous. Collected into kept storage, so a steady
// stream of records allocates nothing.
class FrameState {
public:
  // The object's state, nullopt when it has none or is ambiguous.
  std::optional<std::span<const std::byte>> find(ObjectId id) const;
  // Whether the record drew the object, its state single or ambiguous.
  bool holds(ObjectId id) const;
  // Calls `visit(ObjectId, std::span<const std::byte>)` for each object with a single state.
  template <class Visit> void forEachObject(Visit &&visit) const {
    for (const Saved &saved : saved_) {
      if (!saved.ambiguous) {
        visit(ObjectId{static_cast<std::uint32_t>(saved.key >> 32), static_cast<std::uint32_t>(saved.key)},
              std::span<const std::byte>(bytes_).subspan(saved.offset, saved.size));
      }
    }
  }
  bool empty() const {
    return saved_.empty();
  }
  void clear() {
    saved_.clear();
    bytes_.clear();
  }

private:
  friend class FrameStates;
  struct Saved {
    std::uint64_t key = 0; // producer << 32 | object
    std::uint32_t serial = 0;
    std::size_t offset = 0;
    std::size_t size = 0;
    bool ambiguous = false;
  };
  static std::uint64_t indexKey(ObjectId id) {
    return (std::uint64_t{id.producer} << 32) | id.object;
  }

  std::vector<Saved> saved_; // by key
  std::vector<std::byte> bytes_;
};

// The states producers saved, each under the scope whose packets it describes. A packet is often built in
// one logic frame and walked in the next, so a state is kept for a few frames and collected by the record
// that draws its packets. Saved into kept storage, so a steady stream of saves allocates nothing.
class FrameStates {
public:
  static constexpr std::uint32_t kRetainedFrames = 4;

  // Saves the concatenation of `parts` for the scope `owner` (EmissionScope::current()).
  void save(const RecordKey &owner, std::span<const std::span<const std::byte>> parts);
  void save(const RecordKey &owner, std::span<const std::byte> state) {
    save(owner, std::span<const std::span<const std::byte>>(&state, 1));
  }
  // One trivially copyable value; a range of parts or bytes takes the overloads above.
  template <class T>
    requires(std::is_trivially_copyable_v<T> && !std::is_convertible_v<const T &, std::span<const std::byte>> &&
             !std::is_convertible_v<const T &, std::span<const std::span<const std::byte>>>)
  void save(const RecordKey &owner, const T &state) {
    save(owner, std::span<const std::byte>(std::as_bytes(std::span<const T, 1>(&state, 1))));
  }
  // The states of the objects `record` draws, by the serial each keyed primitive carries, into `into`.
  void collect(const FrameRecord &record, FrameState &into) const;
  // A logic frame ended; states older than kRetainedFrames are dropped.
  void endFrame();
  // A savestate load: nothing saved so far describes the machine.
  void clear();

private:
  struct Saved {
    std::uint32_t serial = 0;
    ObjectId id;
    std::uint32_t frame = 0;
    bool ambiguous = false;
    std::size_t offset = 0;
    std::size_t size = 0;
  };
  const Saved *find(std::uint32_t serial) const;

  std::vector<Saved> bySerial_; // by serial
  std::vector<std::byte> bytes_;
  std::vector<std::byte> spareBytes_; // the arena endFrame compacts into
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
