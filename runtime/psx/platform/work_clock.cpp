#include "work_clock.h"

#include <lucent/log.h>

#include <cstdlib>

namespace psx::platform {

void WorkClock::start() {
  if (on_) {
    return;
  }
  const std::thread::id self = std::this_thread::get_id();
  if (emulation_.counter_ && self != emulationThread_) {
    lucent::error("perf", "the work clock was started on a thread other than the emulation thread it counts");
    std::abort();
  }
  emulationThread_ = self;
  on_ = true;
  open(emulation_);
  running_ = &emulation_;
  emulation_.since_ = emulation_.read();
}

void WorkClock::stop() {
  on_ = false;
  running_ = nullptr;
}

std::uint64_t WorkClock::now() const {
  if (running_ == nullptr) {
    return banked_;
  }
  return banked_ + running_->read() - running_->since_;
}

WorkClock::Thread *WorkClock::leave() {
  if (!on_ || running_ == nullptr) {
    return nullptr;
  }
  Thread *left = running_;
  banked_ += left->read() - left->since_;
  running_ = nullptr;
  return left;
}

void WorkClock::enter(Thread *place) {
  if (!on_ || place == nullptr) {
    return;
  }
  open(*place);
  running_ = place;
  place->since_ = place->read();
}

void WorkClock::open(Thread &place) {
  if (place.counter_) {
    return;
  }
  if (unit_) {
    place.counter_.emplace(*unit_);
  } else {
    place.counter_.emplace();
    unit_ = place.counter_->unit();
  }
  if (!place.counter_ || !place.counter_->valid()) {
    ++uncounted_;
    lucent::error("perf", "a product thread's work counter did not open; its work is missing from the cost lines");
  }
}

} // namespace psx::platform
