// game_iface.cpp — the legacy adapter.
#include "game_iface.h"

#include "crt0_boot.h"
#include "fps60.h"
#include "game.h"
#include "host_world_pass_strategy.h"
#include "stock_dma_callbacks.h"
#include "stock_libcd.h"
#include "stock_libgpu.h"
#include "stock_libgte.h"
#include "stock_pad_buffers.h"
#include "stock_vsync.h"

#include <cstdio>
#include <cstring>
#include <memory>

namespace {

class LegacyAudio final : public psx::game::TitleAudio {
public:
  LegacyAudio(Core &core, const GameHooks &hooks) : core_(core), hooks_(hooks) {}

  bool present() const {
    return hooks_.audioMixFrame || hooks_.audioNowPlayingName || hooks_.audioSoundTestPlay;
  }
  void mixFrame(std::int16_t *pcm, int frames) override {
    if (hooks_.audioMixFrame) {
      hooks_.audioMixFrame(&core_, pcm, frames);
    }
  }
  const char *nowPlayingName() const override {
    return hooks_.audioNowPlayingName ? hooks_.audioNowPlayingName(&core_) : nullptr;
  }
  void soundTestPlay(int track) override {
    if (hooks_.audioSoundTestPlay) {
      hooks_.audioSoundTestPlay(&core_, track);
    }
  }

private:
  Core &core_;
  const GameHooks &hooks_;
};

class LegacyDevWarp final : public psx::game::TitleDevWarp {
public:
  LegacyDevWarp(Core &core, const GameHooks &hooks) : core_(core), hooks_(hooks) {}

  bool present() const {
    return hooks_.devAreaCount != nullptr;
  }
  int areaCount() const override {
    return hooks_.devAreaCount(&core_);
  }
  const char *areaName(int area) const override {
    return hooks_.devAreaName ? hooks_.devAreaName(&core_, area) : nullptr;
  }
  bool warpAllowed() const override {
    return !hooks_.devWarpAllowed || hooks_.devWarpAllowed(&core_);
  }

private:
  Core &core_;
  const GameHooks &hooks_;
};

psx::cd::StockLibcd::Entries legacyLibcdEntries(const GameConfig &config) {
  return {.read = config.cdReadStock,
          .readSync = config.cdReadSync,
          .command = config.cdCommand,
          .sync = config.cdSync,
          .searchFile = config.cdSearchFile,
          .getSector = config.cdGetSector};
}

psx::cd::StockCommandWorkArea legacyLibcdWorkArea(const GameConfig &config) {
  const std::uint32_t base = config.cdLastPosBuf;
  return base ? psx::cd::StockCommandWorkArea{base, base + 4u} : psx::cd::StockCommandWorkArea{};
}

// The hooks' context, published as Core::gameCtx for the legacy title code that reads it, and the
// stock libraries the GameConfig names.
class LegacySession final : public psx::game::TitleSessionState {
public:
  LegacySession(Game &game, const GameConfig &config, const GameHooks &hooks)
      : core_(game.core), hooks_(hooks), audio(game.core, hooks), warp(game.core, hooks),
        libcd_(legacyLibcdEntries(config), legacyLibcdWorkArea(config), config.cdReadyCbPtr),
        libgpu_({.drawSync = config.hle.drawSync,
                 .timeoutArm = config.hle.gpuTimeoutArm,
                 .timeoutCheck = config.hle.gpuTimeoutCheck,
                 .timeoutDeadlineWord = config.hle.gpuTimeoutDeadlineVar,
                 .timeoutFlagWord = config.hle.gpuTimeoutFlagVar}),
        libgte_({.setGeomOffset = config.hle.setGeomOffset, .setGeomScreen = config.hle.setGeomScreen}),
        vsync_(config.hle.vsyncTrap, 0), dmaCallbacks_(config.dmaCallbackTable) {
    core_.gameCtx = hooks_.ctxCreate ? hooks_.ctxCreate(&core_) : nullptr;
    libcd_.bind(game);
    libgpu_.bind(game);
    libgte_.bind(game);
    vsync_.bind(game);
    dmaCallbacks_.bind(game);
  }
  ~LegacySession() override {
    if (hooks_.ctxDestroy) {
      hooks_.ctxDestroy(core_.gameCtx);
    }
    core_.gameCtx = nullptr;
  }
  LegacySession(const LegacySession &) = delete;
  LegacySession &operator=(const LegacySession &) = delete;
  LegacySession(LegacySession &&) = delete;
  LegacySession &operator=(LegacySession &&) = delete;

private:
  Core &core_;
  const GameHooks &hooks_;

public:
  LegacyAudio audio;
  LegacyDevWarp warp;

private:
  psx::cd::StockLibcd libcd_;
  psx::hle::StockLibgpu libgpu_;
  psx::hle::StockLibgte libgte_;
  psx::hle::StockVSync vsync_;
  psx::hle::StockDmaCallbacks dmaCallbacks_;
};

// Live base/end globals win over a fixed base and stride, as GameConfig has always meant.
std::optional<psx::gpu::GuestPacketPoolWindows> legacyPacketPool(const GameConfig &config) {
  using Representation = psx::gpu::GuestPacketPoolWindows::Representation;
  for (std::size_t pool = 0; pool < 2; ++pool) {
    if (config.packetPoolBasePtrs[pool] || config.packetPoolEndPtrs[pool]) {
      return psx::gpu::GuestPacketPoolWindows{
          .representation = Representation::LiveBaseEndPointers,
          .basePointer = {config.packetPoolBasePtrs[0], config.packetPoolBasePtrs[1]},
          .endPointer = {config.packetPoolEndPtrs[0], config.packetPoolEndPtrs[1]}};
    }
  }
  if (!config.packetPoolBase || !config.packetPoolStride) {
    return std::nullopt;
  }
  return psx::gpu::GuestPacketPoolWindows{.representation = Representation::FixedBaseStride,
                                          .base = config.packetPoolBase,
                                          .stride = config.packetPoolStride};
}

psx::game::Crt0Image legacyCrt0(const GameConfig &config) {
  return {
      .bss = {config.bssZeroLo, config.bssZeroHi},
      .stackTopWordAddress = config.stackTopBase,
      .stackReserveWordAddress = config.stackTopBase2,
      .heapBase = config.heapBase,
      .heapSizeStoreAddress = config.heapSizePtr,
      .heapBaseStoreAddress = config.heapBasePtr,
      .globalPointer = config.gp,
      .libcInitEntry = config.libcInit,
      .crt0Entry = config.crt0,
      .stackBias = {config.stackBias.declared != 0, config.stackBias.value},
  };
}

LegacySession &sessionOf(const Core &core) {
  return static_cast<LegacySession &>(*core.game->sessionState);
}

} // namespace

LegacyGameRuntimeAdapter::LegacyGameRuntimeAdapter(const GameConfig &config, const GameHooks &hooks)
    : config_(config),
      hostIdentity_{
          .cardEnvVar = config.cardEnvVar,
          .cardDefaultPath = config.cardDefaultPath,
          .userDataName = config.userDataName,
      },
      packetPool_(legacyPacketPool(config)) {
  bindLegacyInterface(&config, &hooks);
}

std::unique_ptr<psx::game::TitleSessionState> LegacyGameRuntimeAdapter::createSession(Game &game) {
  return std::make_unique<LegacySession>(game, config_, *legacyHooks());
}

void LegacyGameRuntimeAdapter::registerOverrides(Game &game) {
  legacyHooks()->registerOverrides(&game);
}

void LegacyGameRuntimeAdapter::startProgram(Core &core) {
  psx::game::Crt0Boot{legacyCrt0(config_)}.run(core);
  legacyHooks()->bootInit(&core);
}

void LegacyGameRuntimeAdapter::deliverPad(Core &core, const psx::input::PadDelivery &delivery) {
  psx::input::StockPadBuffers{{
                                  .slot0Buffer = config_.padSlot0Buf,
                                  .slot1Buffer = config_.padSlot1Buf,
                                  .slotPointerTable = config_.padSlotPtrTable,
                                  .slotPointerStride = config_.padSlotPtrStride,
                              }}
      .deliver(core, delivery);
}

std::optional<psx::gpu::PacketPoolExtent> LegacyGameRuntimeAdapter::packetPool(Core &core) const {
  if (!packetPool_) {
    return std::nullopt;
  }
  return packetPool_->extent(core);
}

bool LegacyGameRuntimeAdapter::interpolates() const {
  return true;
}

int LegacyGameRuntimeAdapter::displayHeight() const {
  return config_.guestDisplayHeight;
}

RenderCapabilities LegacyGameRuntimeAdapter::renderCapabilities() const {
  return RenderCapabilities::interpolatedNative();
}

bool LegacyGameRuntimeAdapter::guestVramIsPicture(const Game &) const {
  return config_.preserveVramBackdrop != 0;
}

std::unique_ptr<psx::frame::TemporalFramePresentation>
LegacyGameRuntimeAdapter::createTemporalFramePresentation(Game &game) {
  return std::make_unique<Fps60>(game, makeHostWorldPassStrategy(game));
}

bool LegacyGameRuntimeAdapter::titleCommand(Core &core, const char *command, const char *line, std::FILE *reply) {
  const GameHooks &hooks = *legacyHooks();
  if (std::strcmp(command, "tp") == 0 && hooks.replCamTeleport && hooks.replCamTeleportOff) {
    int x = 0;
    int y = 0;
    int z = 0;
    if (std::sscanf(line, "%*s %d %d %d", &x, &y, &z) == 3) {
      hooks.replCamTeleport(&core, x, y, z);
      std::fprintf(reply, "tp camera -> (%d,%d,%d)\n", x, y, z);
    } else {
      hooks.replCamTeleportOff(&core);
      std::fprintf(reply, "tp off (camera follows player)\n");
    }
    return true;
  }
  if (!hooks.replCommand || !hooks.replCommand(&core, command, line)) {
    return false;
  }
  // A GameHooks command answers in the log only; the control channel still gets one line.
  std::fprintf(reply, "%s: done (reply in the log)\n", command);
  return true;
}

psx::game::TitleAudio *LegacyGameRuntimeAdapter::audio(Core &core) const {
  LegacyAudio &audio = sessionOf(core).audio;
  return audio.present() ? &audio : nullptr;
}

psx::game::TitleDevWarp *LegacyGameRuntimeAdapter::devWarp(Core &core) const {
  LegacyDevWarp &warp = sessionOf(core).warp;
  return warp.present() ? &warp : nullptr;
}

const psx::game::HostIdentity *LegacyGameRuntimeAdapter::hostIdentity() const {
  return &hostIdentity_;
}
