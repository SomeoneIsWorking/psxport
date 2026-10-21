#include "hunk_reader.h"

#include <lucent/log.h>

#include <utility>

namespace psx::cd {

HunkReader::HunkReader(chd_file *chd, std::string path)
    : chd_(chd), path_(std::move(path)), hunkBytes_(chd_get_header(chd)->hunkbytes),
      hunkCount_(chd_get_header(chd)->totalhunks),
      storage_(std::make_unique<std::uint8_t[]>(static_cast<std::size_t>(hunkBytes_) * kSlots)) {
  for (std::size_t i = 0; i < kSlots; ++i) {
    slots_[i].data = storage_.get() + i * hunkBytes_;
  }
  workers_.reserve(kWorkers);
  for (unsigned i = 0; i < kWorkers; ++i) {
    workers_.emplace_back([this, i] {
      work(i);
    });
  }
}

HunkReader::~HunkReader() {
  {
    const std::lock_guard lock(mutex_);
    stopping_ = true;
  }
  queued_.notify_all();
  for (std::thread &worker : workers_) {
    worker.join();
  }
}

HunkReader::Slot *HunkReader::find(std::uint32_t index) {
  for (Slot &slot : slots_) {
    if (slot.state != SlotState::Empty && slot.index == index) {
      return &slot;
    }
  }
  return nullptr;
}

HunkReader::Slot *HunkReader::victim() {
  Slot *oldest = nullptr;
  for (Slot &slot : slots_) {
    if (slot.state == SlotState::Decoding || slot.used == request_) {
      continue;
    }
    if (oldest == nullptr || slot.used < oldest->used) {
      oldest = &slot;
    }
  }
  return oldest;
}

void HunkReader::queueAhead(std::uint32_t index) {
  bool queuedAny = false;
  for (std::uint32_t next = index + 1; next <= index + kAhead && next < hunkCount_; ++next) {
    if (Slot *held = find(next)) {
      held->used = request_;
      continue;
    }
    Slot *slot = victim();
    if (slot == nullptr) {
      break;
    }
    slot->index = next;
    slot->used = request_;
    slot->state = SlotState::Queued;
    queuedAny = true;
  }
  if (queuedAny) {
    queued_.notify_all();
  }
}

HunkReader::Fetch HunkReader::hunk(std::uint32_t index) {
  std::unique_lock lock(mutex_);
  ++request_;
  Slot *slot = find(index);
  const bool resident = slot != nullptr && slot->state == SlotState::Ready;
  if (slot != nullptr && slot->state == SlotState::Decoding) {
    decoded_.wait(lock, [slot] {
      return slot->state != SlotState::Decoding;
    });
    slot = slot->state == SlotState::Ready && slot->index == index ? slot : nullptr;
  }
  if (slot == nullptr || slot->state != SlotState::Ready) {
    if (slot == nullptr) {
      decoded_.wait(lock, [this, &slot] {
        slot = victim();
        return slot != nullptr;
      });
    }
    slot->index = index;
    slot->used = request_;
    slot->state = SlotState::Decoding;
    lock.unlock();
    const bool read = chd_read(chd_, index, slot->data) == CHDERR_NONE;
    lock.lock();
    slot->state = read ? SlotState::Ready : SlotState::Empty;
    decoded_.notify_all();
    if (!read) {
      return {};
    }
  }
  slot->used = request_;
  queueAhead(index);
  return {slot->data, resident};
}

void HunkReader::work(unsigned worker) {
  chd_file *own = nullptr;
  if (chd_open(path_.c_str(), CHD_OPEN_READ, nullptr, &own) != CHDERR_NONE) {
    lucent::warn("disc", "hunk worker {} cannot open {}; it decodes nothing ahead", worker, path_);
    return;
  }
  std::unique_lock lock(mutex_);
  while (true) {
    Slot *next = nullptr;
    queued_.wait(lock, [this, &next] {
      next = nullptr;
      for (Slot &slot : slots_) {
        if (slot.state == SlotState::Queued &&
            (next == nullptr || slot.used > next->used || (slot.used == next->used && slot.index < next->index))) {
          next = &slot;
        }
      }
      return stopping_ || next != nullptr;
    });
    if (stopping_) {
      break;
    }
    next->state = SlotState::Decoding;
    const std::uint32_t index = next->index;
    lock.unlock();
    const bool read = chd_read(own, index, next->data) == CHDERR_NONE;
    lock.lock();
    next->state = read ? SlotState::Ready : SlotState::Empty;
    decoded_.notify_all();
  }
  lock.unlock();
  chd_close(own);
}

} // namespace psx::cd
