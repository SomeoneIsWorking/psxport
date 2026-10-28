#include "record_fault.h"

#include "core.h"
#include "owner_ledger.h"

#include <lucent/log.h>

#include <cstdlib>
#include <optional>

namespace psx::gpu {

const char *recordFaultName(RecordFaultKind kind) {
  switch (kind) {
  case RecordFaultKind::DrawListCycle:
    return "draw-list-cycle";
  case RecordFaultKind::PolyLineUnterminated:
    return "poly-line-unterminated";
  case RecordFaultKind::RecordEntries:
    return "record-entries";
  case RecordFaultKind::RecordPool:
    return "record-pool";
  }
  return "?";
}

void raiseRecordFault(RecordFaultKind kind,
                      std::uint32_t guestAddress,
                      std::string_view owner,
                      std::string_view detail) {
  lucent::error("record-fault",
                "{} at 0x{:08X}, refused by {}: {}; the process ends before this record reaches the GPU",
                recordFaultName(kind),
                guestAddress,
                owner,
                detail);
  std::abort();
}

std::string guestWriterOf(const Core &core, std::uint32_t address) {
  const present::OwnerHandle owner = core.owners.ownerOf(address);
  if (const std::optional<present::RecordKey> key = core.owners.scopeKey(owner)) {
    return lucent::format("producer {} object 0x{:08X} wrote 0x{:08X}", key->producer, key->object, address);
  }
  if (const present::GuestWriter *writer = core.owners.writer(owner)) {
    return lucent::format("{} writer fn 0x{:08X} block 0x{:08X} wrote 0x{:08X}",
                          present::writerKindName(writer->kind),
                          writer->function,
                          writer->blockPc,
                          address);
  }
  return lucent::format("no recorded writer of 0x{:08X}", address);
}

} // namespace psx::gpu
