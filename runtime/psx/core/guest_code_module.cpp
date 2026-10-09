#include "guest_code_module.h"

#include "core.h"
#include "execution_control.h"
#include "game.h"
#include "game_runtime.h"
#include "image_identity.h"

#include <lucent/content.h>
#include <lucent/log.h>

#include <cstddef>
#include <span>
#include <string>

namespace psx::code_module {

Digest digest(std::span<const std::uint8_t> bytes) {
  const lucent::content::Sha256 sum = lucent::content::sha256(std::as_bytes(bytes));
  Digest result;
  result.hex = lucent::content::sha256_hex(sum);
  for (std::size_t i = 0; i < 8u; ++i) {
    result.identity = result.identity << 8u | sum[i];
  }
  return result;
}

psx::cpu::ImageIdentity activate(Core &core, std::string_view kind, GuestAddressRange physical, const Digest &content) {
  return core.imageCatalog().activate(std::string(kind) + " SHA-256 " + content.hex, physical, content.identity);
}

namespace {

void refuse(Core &core, const psx::cd::StockReadLanding &landing, const char *detail) {
  psx::cpu::requestExecutionExit(core,
                                 {.reason = psx::cpu::ExecutionExitReason::Fault,
                                  .guestPc = core.pc,
                                  .detail = std::string("stock CdRead landing refused: ") + detail});
  lucent::error("cd",
                "stock CdRead landing of {} byte(s) at 0x{:08X} from LBA {} was NOT published as an image: {}",
                landing.bytes,
                landing.destination,
                landing.firstLba,
                detail);
}

} // namespace

void publishStockReadLanding(Core &core, const psx::cd::StockReadLanding &landing, GuestAddressRange arena) {
  const std::uint32_t begin = landing.destination & 0x1fffffffu;
  if (landing.bytes == 0u || begin >= sizeof core.ram || landing.bytes > sizeof core.ram - begin) {
    refuse(core, landing, "the landed range is empty or does not fit guest main RAM");
    return;
  }
  if (arena.valid() && (begin < arena.begin || begin + landing.bytes > arena.end)) {
    return;
  }
  const Digest content = digest(std::span<const std::uint8_t>(core.ram + begin, landing.bytes));
  const auto identity = activate(core, "CD read", {begin, begin + landing.bytes}, content);
  lucent::debug("cd",
                "stock CdRead of {} sector(s) from LBA {} published as image {} generation {}: 0x{:08X}..0x{:08X}, "
                "SHA-256 {}",
                landing.sectors,
                landing.firstLba,
                identity.id,
                identity.generation,
                landing.destination,
                landing.destination + landing.bytes,
                content.hex);
}

} // namespace psx::code_module

GuestAddressRange guestCodeModuleWindow(const Core &core) {
  const GameRuntime *runtime = core.game ? core.game->runtime : nullptr;
  return runtime ? runtime->guestCodeModuleWindow() : GuestAddressRange{};
}

void publishGuestCodeModuleLanding(Core &core, GuestAddressRange landed) {
  const GuestAddressRange window = guestCodeModuleWindow(core);
  if (!window.valid() || !landed.valid() || landed.begin < window.begin || landed.end > window.end) {
    return;
  }
  if (core.currentImageIdentity(window).has_value()) {
    return; // already a residency; this sector is part of the same load
  }
  const auto content =
      psx::code_module::digest(std::span<const std::uint8_t>(core.ram + window.begin, window.end - window.begin));
  const auto identity = psx::code_module::activate(core, "guest-code-module", window, content);
  lucent::info("code-module",
               "activated guest code module 0x{:08X}..0x{:08X} (image {} generation {}) after a CD "
               "transfer landed at 0x{:08X}",
               0x80000000u | window.begin,
               0x80000000u | window.end,
               identity.id,
               identity.generation,
               0x80000000u | landed.begin);
}