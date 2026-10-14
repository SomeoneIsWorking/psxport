// game_iface.h — the legacy GameConfig/GameHooks adapter; deleted once no title derives it, add nothing.
#pragma once

#include "game_runtime.h"
#include "guest_packet_pool_windows.h"
#include "legacy_game_config.h"
#include "legacy_game_hooks.h"

class LegacyGameRuntimeAdapter : public psx::game::GameRuntime {
public:
  LegacyGameRuntimeAdapter(const GameConfig &config, const GameHooks &hooks);

  std::unique_ptr<psx::game::TitleSessionState> createSession(Game &game) override;
  void registerOverrides(Game &game) override;
  void startProgram(Core &core) override;
  void deliverPad(Core &core, const psx::input::PadDelivery &delivery) override;
  std::optional<psx::gpu::PacketPoolExtent> packetPool(Core &core) const override;
  bool interpolates() const override;
  int displayHeight() const override;
  RenderCapabilities renderCapabilities() const override;
  bool guestVramIsPicture(const Game &game) const override;
  std::unique_ptr<psx::frame::TemporalFramePresentation> createTemporalFramePresentation(Game &game) override;
  bool titleCommand(Core &core, const char *command, const char *line, std::FILE *reply) override;
  psx::game::TitleAudio *audio(Core &core) const override;
  psx::game::TitleDevWarp *devWarp(Core &core) const override;
  const psx::game::HostIdentity *hostIdentity() const override;

private:
  const GameConfig &config_;
  psx::game::HostIdentity hostIdentity_;
  std::optional<psx::gpu::GuestPacketPoolWindows> packetPool_;
};
