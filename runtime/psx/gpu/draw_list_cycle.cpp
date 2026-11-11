#include "draw_list_cycle.h"

#include "core.h"
#include "game.h"
#include "owner_ledger.h"
#include "render_device.h"

#include <lucent/log.h>

namespace psx::gpu {

void refuseDrawListCycle(Core &core, std::uint32_t node, std::string_view refuser, std::string_view detail) {
  core.game->renderDevice.latchFault(
      render::FaultCause::DrawListCycle, lucent::format("0x{:08X}, refused by {}", node, refuser), detail);
}

std::string guestWriterOf(const Core &core, std::uint32_t address) {
  const present::GuestWriter *writer = core.owners.writer(core.owners.ownerOf(address));
  if (writer == nullptr) {
    return lucent::format("no recorded writer of 0x{:08X}", address);
  }
  if (writer->function == 0) {
    return lucent::format("{} writer block 0x{:08X} (its function is not on the shadow call stack) wrote 0x{:08X}",
                          present::writerKindName(writer->kind),
                          writer->blockPc,
                          address);
  }
  return lucent::format("{} writer fn 0x{:08X} block 0x{:08X} wrote 0x{:08X}",
                        present::writerKindName(writer->kind),
                        writer->function,
                        writer->blockPc,
                        address);
}

} // namespace psx::gpu
