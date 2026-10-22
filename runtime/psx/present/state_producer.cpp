// state_producer.cpp — a title's table of renders, and the list a render emits into.
#include "state_producer.h"

#include <lucent/log.h>

#include <cstdlib>
#include <utility>

namespace psx::present {

void PrimitiveList::emit(OtSlot slot, const DrawPrimitive &primitive) {
  primitives.emplace_back(slot, primitive);
}

void PrimitiveList::clear() {
  primitives.clear();
}

void StateProducerTable::install(std::uint32_t producer, std::unique_ptr<const StateProducer> render) {
  if (!render || !renders_.try_emplace(producer, std::move(render)).second) {
    lucent::error("present", "producer 0x{:08X} installed twice or without a render", producer);
    std::abort();
  }
}

const StateProducer *StateProducerTable::find(std::uint32_t producer) const {
  const auto found = renders_.find(producer);
  return found == renders_.end() ? nullptr : found->second.get();
}

} // namespace psx::present
