// hunk_reader.h — psx::cd::HunkReader: one CHD image's decoded hunks; the caller's hunk and the ones after it are
// decoded ahead on worker threads, so a burst of sequential reads costs the caller a fraction of its decode time.
#pragma once
#include <libchdr/chd.h>

#include <array>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace psx::cd {

class HunkReader {
public:
  struct Fetch {
    const std::uint8_t *data = nullptr; // valid until the next hunk() call; null on a read error
    bool resident = false;              // decoded before the call asked for it
  };

  // `chd` is the caller's open image, read only on the caller's thread; each worker opens `path` itself.
  HunkReader(chd_file *chd, std::string path);
  ~HunkReader();
  HunkReader(const HunkReader &) = delete;
  HunkReader &operator=(const HunkReader &) = delete;
  HunkReader(HunkReader &&) = delete;
  HunkReader &operator=(HunkReader &&) = delete;

  Fetch hunk(std::uint32_t index);

private:
  enum class SlotState : std::uint8_t { Empty, Queued, Decoding, Ready };
  struct Slot {
    std::uint32_t index = 0;
    std::uint64_t used = 0; // the request that last touched it
    SlotState state = SlotState::Empty;
    std::uint8_t *data = nullptr;
  };

  static constexpr std::size_t kSlots = 32;
  static constexpr std::uint32_t kAhead = 12;
  static constexpr unsigned kWorkers = 4;

  Slot *find(std::uint32_t index);
  // The least recently touched slot no worker is decoding and this request has not touched; null if none.
  Slot *victim();
  void queueAhead(std::uint32_t index);
  void work(unsigned worker);

  chd_file *chd_;
  const std::string path_;
  const std::uint32_t hunkBytes_;
  const std::uint32_t hunkCount_;
  std::unique_ptr<std::uint8_t[]> storage_;
  std::array<Slot, kSlots> slots_{};
  std::mutex mutex_;
  std::condition_variable queued_;
  std::condition_variable decoded_;
  std::uint64_t request_ = 0;
  bool stopping_ = false;
  std::vector<std::thread> workers_;
};

} // namespace psx::cd
