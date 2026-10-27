#include "emission_scope.h"


#include <lucent/log.h>

#include <cstdlib>

namespace psx::present {

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
  return Guard(*this, RecordKey{stack_.back().key.producer, object, 0, 0, nextSerial_++});
}

EmissionScope::Guard EmissionScope::element(std::uint32_t index) {
  if (stack_.empty()) {
    lucent::error("emission", "element({}) with no producer scope open", index);
    std::abort();
  }
  return Guard(*this, RecordKey{stack_.back().key.producer, stack_.back().key.object, index, 0, stack_.back().key.serial});
}

EmissionScope::Guard EmissionScope::amend(const RecordKey &key) {
  return Guard(*this, key);
}

EmissionScope::EmissionScope(OwnerLedger &ledger) : ledger_(ledger) {}

void EmissionScope::push(const RecordKey &key) {
  if (key.producer == 0) {
    lucent::error("emission", "a producer scope needs a nonzero producer address");
    std::abort();
  }
  stack_.push_back({key, ledger_.openScope(key)});
}

const RecordKey &EmissionScope::current() const {
  if (stack_.empty()) {
    lucent::error("emission", "a producer state saved with no producer scope open");
    std::abort();
  }
  return stack_.back().key;
}

void EmissionScope::pop() {
  stack_.pop_back();
}

void EmissionScope::noteStore(GuestSpan store) {
  ledger_.noteStore(store, stack_.empty() ? kNoOwner : stack_.back().handle);
}

void EmissionScope::bindPacket(std::uint32_t packetAddress) {
  if (stack_.empty()) {
    lucent::error("emission", "bindPacket(0x{:08X}) with no producer scope open", packetAddress);
    std::abort();
  }
  noteStore({packetAddress + 4u, 4u});
}

std::optional<RecordKey> EmissionScope::keyFor(std::uint32_t packetAddress) const {
  return ledger_.scopeKey(ledger_.ownerOf(packetAddress + 4u));
}

} // namespace psx::present
