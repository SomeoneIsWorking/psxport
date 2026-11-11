// work_clock.h — the user-space work of the product's threads, which hand control to each other and never run at
// once: the emulation thread and the Coro fibers it resumes. Each thread reads only its own counter.
#pragma once

#include "thread_work_counter.h"

#include <cstdint>
#include <optional>
#include <thread>

namespace psx::platform {

class WorkClock {
public:
  // One thread's place on the clock: the emulation thread's, or a fiber's.
  class Thread {
    friend class WorkClock;
    [[nodiscard]] std::uint64_t read() const {
      return counter_ ? counter_->read() : 0;
    }
    std::optional<ThreadWorkCounter> counter_;
    std::uint64_t since_ = 0;
  };

  WorkClock() = default;
  WorkClock(const WorkClock &) = delete;
  WorkClock &operator=(const WorkClock &) = delete;
  WorkClock(WorkClock &&) = delete;
  WorkClock &operator=(WorkClock &&) = delete;

  // Counts from here; the calling thread is the emulation thread.
  void start();
  void stop();
  [[nodiscard]] bool on() const {
    return on_;
  }
  // Fixed by the first counter opened.
  [[nodiscard]] WorkUnit unit() const {
    return unit_.value_or(WorkUnit::Instructions);
  }
  // Work counted so far, read on the running thread.
  [[nodiscard]] std::uint64_t now() const;

  // The running thread is about to block in a handoff: its work so far is banked, and the returned place is
  // what it enters again when it wakes (null while the clock is off).
  Thread *leave();
  // The thread `place` belongs to runs again, on that thread.
  void enter(Thread *place);

  // Threads whose counter could not open in the clock's unit; their work is missing from now().
  [[nodiscard]] std::uint64_t uncountedThreads() const {
    return uncounted_;
  }

private:
  void open(Thread &place);

  bool on_ = false;
  std::optional<WorkUnit> unit_;
  std::thread::id emulationThread_;
  Thread emulation_;
  Thread *running_ = nullptr;
  std::uint64_t banked_ = 0;
  std::uint64_t uncounted_ = 0;
};

} // namespace psx::platform
