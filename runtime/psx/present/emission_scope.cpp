#include "emission_scope.h"

#include "guest_memory_map.h"

#include <lucent/log.h>

#include <algorithm>
#include <cstdlib>

namespace psx::present {
namespace {

constexpr std::size_t kMainRamWords = kMainRamBytes / 4u;

// Only main RAM holds a DMA-able packet.
std::optional<std::uint32_t> mainRamWord(std::uint32_t address) {
  const std::optional<std::uint32_t> offset = mainRamOffset(address);
  if (!offset) {
    return std::nullopt;
  }
  return *offset >> 2u;
}

} // namespace

EmissionScope::Guard::Guard(EmissionScope &scope, std::uint32_t producer, std::uint32_t object, std::uint32_t element)
    : scope_(scope) {
  scope_.push(RecordKey{producer, object, element, 0, scope_.nextSerial_++});
}

EmissionScope::Guard::Guard(EmissionScope &scope, const RecordKey &key) : scope_(scope) {
  scope_.push(key);
}

EmissionScope::Guard::~Guard() {
  scope_.pop();
}

EmissionScope::Guard EmissionScope::instance(std::uint32_t object) {
  if (stack_.empty()) {
    lucent::error("emission", "instance(0x{:08X}) with no producer scope open", object);
    std::abort();
  }
  return Guard(*this, RecordKey{stack_.back().producer, object, 0, 0, nextSerial_++});
}

EmissionScope::Guard EmissionScope::element(std::uint32_t index) {
  if (stack_.empty()) {
    lucent::error("emission", "element({}) with no producer scope open", index);
    std::abort();
  }
  return Guard(*this, RecordKey{stack_.back().producer, stack_.back().object, index, 0, stack_.back().serial});
}

EmissionScope::Guard EmissionScope::amend(const RecordKey &key) {
  return Guard(*this, key);
}

EmissionScope::EmissionScope() : words_(kMainRamWords) {}

void EmissionScope::push(const RecordKey &key) {
  if (key.producer == 0) {
    lucent::error("emission", "a producer scope needs a nonzero producer address");
    std::abort();
  }
  stack_.push_back(key);
}

const RecordKey &EmissionScope::current() const {
  if (stack_.empty()) {
    lucent::error("emission", "a producer state saved with no producer scope open");
    std::abort();
  }
  return stack_.back();
}

void EmissionScope::pop() {
  stack_.pop_back();
}

void EmissionScope::noteStore(GuestSpan store) {
  const std::optional<std::uint32_t> first = mainRamWord(store.address);
  const std::optional<std::uint32_t> last = mainRamWord(store.address + store.bytes - 1u);
  if (!first || !last || *last < *first) {
    return;
  }
  const auto begin = words_.begin() + *first;
  const auto end = words_.begin() + *last + 1;
  if (stack_.empty()) {
    std::for_each(begin, end, [](RecordKey &key) {
      key.producer = 0;
    });
  } else {
    std::fill(begin, end, stack_.back());
  }
}

void EmissionScope::bindPacket(std::uint32_t packetAddress) {
  if (stack_.empty()) {
    lucent::error("emission", "bindPacket(0x{:08X}) with no producer scope open", packetAddress);
    std::abort();
  }
  noteStore({packetAddress + 4u, 4u});
}

std::optional<RecordKey> EmissionScope::keyFor(std::uint32_t packetAddress) const {
  const std::optional<std::uint32_t> word = mainRamWord(packetAddress + 4u);
  if (!word || words_[*word].producer == 0) {
    return std::nullopt;
  }
  return words_[*word];
}

void EmissionScope::clear() {
  std::fill(words_.begin(), words_.end(), RecordKey{});
}

} // namespace psx::present
