#include "guest_code_module.h"

#include "core.h"
#include "game.h"
#include "game_runtime.h"
#include "image_identity.h"

#include <lucent/log.h>

namespace {

// FNV-1a over the window's current bytes. It names the content this residency was established
// from; it is an identity, not a check, and no decision in the framework compares it against an
// expected value.
std::uint64_t windowContentIdentity(Core &core, GuestAddressRange window) {
  std::uint64_t hash = 1469598103934665603ull;
  for (std::uint32_t address = window.begin; address < window.end; ++address) {
    hash ^= core.mem_r8(address);
    hash *= 1099511628211ull;
  }
  return hash;
}

} // namespace

GuestAddressRange guestCodeModuleWindow(const Core &core) {
  const GameRuntime *runtime = core.game ? core.game->runtime : nullptr;
  return runtime ? runtime->guestCodeModuleWindow() : GuestAddressRange{};
}

void publishGuestCodeModuleLanding(Core &core, GuestAddressRange landed) {
  const GuestAddressRange window = guestCodeModuleWindow(core);
  if (!window.valid() || !landed.valid() || window.begin >= landed.begin || landed.end > window.end) {
    return;
  }
  if (core.currentImageIdentity(window).has_value()) {
    return; // already a residency; this sector is part of the same load
  }
  const auto identity = core.imageCatalog().activate("guest-code-module", window, windowContentIdentity(core, window));
  lucent::info("code-module",
               "activated guest code module 0x{:08X}..0x{:08X} (image {} generation {}) after a CD "
               "transfer landed at 0x{:08X}",
               0x80000000u | window.begin,
               0x80000000u | window.end,
               identity.id,
               identity.generation,
               0x80000000u | landed.begin);
}