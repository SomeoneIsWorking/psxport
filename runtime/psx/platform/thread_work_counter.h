// thread_work_counter.h — the calling thread's own user-space work: instructions retired, or its CPU time.
#pragma once

#include <array>
#include <cstdint>

namespace psx::platform {

enum class WorkUnit : std::uint8_t {
  Instructions,   // user-space instructions retired (Linux perf_event)
  CpuNanoseconds, // the thread's CPU time, where no instruction counter opens
};

// A counter of the thread that constructs it. read() is called on that thread only.
class ThreadWorkCounter {
public:
  // The instruction counter when this host opens one, else thread CPU time.
  ThreadWorkCounter();
  // `unit` only; valid() is false when this host cannot count it for this thread.
  explicit ThreadWorkCounter(WorkUnit unit);
  ~ThreadWorkCounter();
  ThreadWorkCounter(const ThreadWorkCounter &) = delete;
  ThreadWorkCounter &operator=(const ThreadWorkCounter &) = delete;
  ThreadWorkCounter(ThreadWorkCounter &&) = delete;
  ThreadWorkCounter &operator=(ThreadWorkCounter &&) = delete;

  [[nodiscard]] bool valid() const {
    return valid_;
  }
  [[nodiscard]] WorkUnit unit() const {
    return unit_;
  }
  [[nodiscard]] std::uint64_t read() const;

private:
  bool openInstructions();
  void closeAll();

  // A hybrid CPU counts each core type on its own PMU; their sum is the thread's.
  static constexpr std::size_t kMaxPmus = 2;
  std::array<int, kMaxPmus> fds_{-1, -1};
  WorkUnit unit_ = WorkUnit::Instructions;
  bool valid_ = false;
};

} // namespace psx::platform
