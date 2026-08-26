// game_iface.cpp — polymorphic game runtime installation plus the bounded legacy adapter.
#include "game_iface.h"

#include "fps60.h"

#include <memory>

namespace {
std::unique_ptr<LegacyGameRuntimeAdapter> g_ownedLegacyRuntime;

} // namespace

void psxport_clear_game_runtime_for_legacy();

LegacyGameRuntimeAdapter::LegacyGameRuntimeAdapter(const GameConfig &config, const GameHooks &hooks)
    : guestProgramImage_{
          .bss = {config.bssZeroLo, config.bssZeroHi},
          .stackTopWordAddress = config.stackTopBase,
          .stackReserveWordAddress = config.stackTopBase2,
          .heapBase = config.heapBase,
          .heapSizeStoreAddress = config.heapSizePtr,
          .heapBaseStoreAddress = config.heapBasePtr,
          .globalPointer = config.gp,
          .libcInitEntry = config.libcInit,
          .gameMainEntry = config.gameMain,
          .crt0Entry = config.crt0,
          .residentText = {config.recMainLo, config.recMainHi},
          .backtraceText = {config.hle.codeScanLo, config.hle.codeScanHi},
          .stackBias = {config.stackBias.declared != 0, config.stackBias.value},
      } {
  bindLegacyInterface(&config, &hooks);
}

void *LegacyGameRuntimeAdapter::createContext(Core &core) {
  const GameHooks *hooks = legacyHooks();
  return hooks->ctxCreate ? hooks->ctxCreate(&core) : nullptr;
}

void LegacyGameRuntimeAdapter::destroyContext(void *context) {
  const GameHooks *hooks = legacyHooks();
  if (hooks->ctxDestroy) {
    hooks->ctxDestroy(context);
  }
}

void LegacyGameRuntimeAdapter::registerOverrides(Game &game) {
  legacyHooks()->registerOverrides(&game);
}

void LegacyGameRuntimeAdapter::bootInit(Core &core) {
  legacyHooks()->bootInit(&core);
}

const GuestProgramImage *LegacyGameRuntimeAdapter::guestProgramImage() const {
  return &guestProgramImage_;
}

RenderCapabilities LegacyGameRuntimeAdapter::renderCapabilities() const {
  return RenderCapabilities::interpolatedNative();
}

bool LegacyGameRuntimeAdapter::guestVramIsPicture(const Game &) const {
  const GameConfig *config = legacyConfigForMigration();
  return config && config->preserveVramBackdrop != 0;
}

std::unique_ptr<TemporalFramePresentation> LegacyGameRuntimeAdapter::createTemporalFramePresentation(Game &game) {
  return std::make_unique<Fps60>(game);
}

void psxport_install_game(const GameConfig *cfg, const GameHooks *hooks) {
  if (!cfg || !hooks) {
    g_ownedLegacyRuntime.reset();
    psxport_clear_game_runtime_for_legacy();
    return;
  }
  g_ownedLegacyRuntime = std::make_unique<LegacyGameRuntimeAdapter>(*cfg, *hooks);
  psxport_install_game(*g_ownedLegacyRuntime);
}
