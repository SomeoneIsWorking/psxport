// game_runtime.h — the title interface: one GameRuntime subclass per title, called through virtuals.
#pragma once

#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include "execution_exit.h"
#include "guest_address_range.h"
#include "guest_packet_pool_windows.h"
#include "input_phase.h"
#include "logo_image.h"
#include "pad_delivery.h"
#include "psx_exe_image.h"
#include "render_capabilities.h"
#include "title_settings.h"

class Core;
class Game;
class GuestWidescreenProjection;
namespace psx::cd {
struct StockReadLanding;
}
namespace psx::frame {
class TemporalFramePresentation;
} // namespace psx::frame
namespace psx::present {
class StateProducer;
}
struct GameConfig;
struct GameHooks;
namespace psx::state {
class NativeStatePort;
} // namespace psx::state

namespace psx::game {

// One control-channel line: its first word and the whole trimmed line.
struct TitleCommandLine {
  const char *command;
  const char *line;
};

// Where a title's memory card and user data live. The framework reads the card variable through its
// configuration owner; the title only names the key.
struct HostIdentity {
  const char *cardEnvVar = nullptr;      // checked before the generic PSXPORT_CARD
  const char *cardDefaultPath = nullptr; // used when neither variable names a card
  const char *userDataName = nullptr;    // one path component under the OS user-data root (bug reports)
};

class FrameDriver {
public:
  virtual ~FrameDriver() = default;

  // One finite native frame: one guest turn and exactly one presentation commit. Never runs a
  // guest-owned frame loop or libetc VSync. `frame` counts host fields from 1.
  virtual void stepFrame(Core &core, uint32_t frame) = 0;

  // Past the title's boot prefix (logos, loading and publisher cards). The picker asks before it shows
  // a panel; a driver with no prefix is past it from its first frame.
  virtual bool pastBootPrefix() const {
    return true;
  }
};

// A title's per-Core state, created with each Game and destroyed with it.
class TitleSessionState {
public:
  virtual ~TitleSessionState() = default;
  TitleSessionState(const TitleSessionState &) = delete;
  TitleSessionState &operator=(const TitleSessionState &) = delete;
  TitleSessionState(TitleSessionState &&) = delete;
  TitleSessionState &operator=(TitleSessionState &&) = delete;

protected:
  TitleSessionState() = default;
};

// A title's own music engine and sound test, mixed over the SPU's output.
class TitleAudio {
public:
  virtual ~TitleAudio() = default;

  // Adds the title's music to `frames` stereo frames the SPU already rendered into `pcm`.
  virtual void mixFrame(std::int16_t *pcm, int frames) = 0;
  // The sound-test track playing now, or null.
  virtual const char *nowPlayingName() const {
    return nullptr;
  }
  // Plays sound-test track `track`, or stops on a negative value.
  virtual void soundTestPlay(int track) {
    (void)track;
  }
};

// The areas the Debug tab's warp offers; the warp itself is the title's `warp` control command.
class TitleDevWarp {
public:
  virtual ~TitleDevWarp() = default;

  virtual int areaCount() const = 0;
  // The area's sourced name, or null when the title has none.
  virtual const char *areaName(int area) const {
    (void)area;
    return nullptr;
  }
  virtual bool warpAllowed() const {
    return true;
  }
};

class GameRuntime {
public:
  virtual ~GameRuntime() = default;
  GameRuntime(const GameRuntime &) = delete;
  GameRuntime &operator=(const GameRuntime &) = delete;
  GameRuntime(GameRuntime &&) = delete;
  GameRuntime &operator=(GameRuntime &&) = delete;

  // ---- Boot

  // Maps the authenticated executable and sets its entry registers; a refusal leaves Core unchanged.
  virtual psx::cpu::PsxExeLoadResult admitExecutable(Core &core, std::span<const std::uint8_t> bytes);
  // Runs before the first frame. The default starts at the EXE header entry `admitExecutable` set; a
  // title that replays crt0 calls psx::game::Crt0Boot with its own addresses.
  virtual void startProgram(Core &core) {
    (void)core;
  }
  virtual void registerOverrides(Game &game) = 0;
  virtual std::unique_ptr<TitleSessionState> createSession(Game &game);
  virtual std::unique_ptr<FrameDriver> createFrameDriver(Game &game);

  // ---- Hardware services the title owns

  // Hands the host pad packet to the guest. The default leaves it to the guest's own SIO0 driver; a
  // title whose driver polls fixed buffers calls psx::input::StockPadBuffers.
  virtual void deliverPad(Core &core, const psx::input::PadDelivery &delivery) {
    (void)core;
    (void)delivery;
  }
  // A synchronous stock CdRead landed its payload; the bytes are written and already invalidated.
  virtual void stockCdReadLanded(Core &core, const psx::cd::StockReadLanding &landing) {
    (void)core;
    (void)landing;
  }
  // Guest RAM the title loads runtime code modules into; a CD transfer landing there becomes an image.
  virtual GuestAddressRange guestCodeModuleWindow() const {
    return {};
  }
  // The title's packet pool now; the producer census attributes only stores inside it. Read once per
  // logic frame and again after a store to one of its sources. Empty: the pool is not reverse-engineered.
  virtual std::optional<psx::gpu::PacketPoolExtent> packetPool(Core &core) const {
    (void)core;
    return std::nullopt;
  }

  // ---- Presentation

  // Whether Record presents get 60 fps in-betweens.
  virtual bool interpolates() const {
    return false;
  }
  // The render of producer `producer`'s saved states, owned by the title's session; null keeps its
  // recorded entries.
  virtual const psx::present::StateProducer *stateProducer(Core &core, std::uint32_t producer) const {
    (void)core;
    (void)producer;
    return nullptr;
  }
  // The sealed logic frame is a cut (camera cut, scene change, teleport); its in-between is the frame.
  virtual bool sealedFrameIsCut(Core &core) const {
    (void)core;
    return false;
  }
  // Delivers simulated fields and waits for their deadline; a title whose field scheduler already
  // advances the devices only waits.
  virtual void pacePresentation(Core &core, int guestFields, int parts);
  // Lines the guest scans out, or 0 when the decoded display height is right.
  virtual int displayHeight() const {
    return 0;
  }
  // Optional title-owned guest projection.
  virtual const GuestWidescreenProjection *guestWidescreenProjection() const {
    return nullptr;
  }
  // The title's logo for its picker panel, read from its live machine.
  virtual std::optional<psx::host::LogoImage> panelLogo(Core &core) const {
    (void)core;
    return std::nullopt;
  }

  // Renderer selection until Record is the only path.
  virtual RenderCapabilities renderCapabilities() const = 0;
  virtual bool guestVramIsPicture(const Game &game) const = 0;
  virtual bool guestPictureIsNativeWidth(const Game &game) const {
    (void)game;
    return false;
  }
  virtual std::unique_ptr<psx::frame::TemporalFramePresentation> createTemporalFramePresentation(Game &game);

  // ---- Debug and run

  // A control-channel or REPL command the framework did not recognise. Replies go to `reply`; true
  // when the command was the title's.
  virtual bool titleCommand(Core &core, const TitleCommandLine &input, std::FILE *reply) {
    (void)core;
    (void)input;
    (void)reply;
    return false;
  }
  virtual TitleAudio *audio(Core &core) const {
    (void)core;
    return nullptr;
  }
  virtual TitleDevWarp *devWarp(Core &core) const {
    (void)core;
    return nullptr;
  }
  // The screen or game state taking input, keying pad recordings (input_phase.h).
  virtual std::uint64_t inputPhase(Core &core) const {
    (void)core;
    return psx::input::kUnkeyedPhase;
  }
  // Native owners' state for the machine save state; null when the title keeps none outside guest RAM.
  virtual psx::state::NativeStatePort *nativeState(Core &core) const {
    (void)core;
    return nullptr;
  }
  // Title lines after the framework's run-complete report.
  virtual void reportRun(Core &core) const {
    (void)core;
  }
  // The Debug tab's world line (position, stage); empty when the title has none.
  virtual std::string worldReadout(Core &core) const {
    (void)core;
    return {};
  }

  // ---- Identity data
  virtual const HostIdentity *hostIdentity() const {
    return nullptr;
  }
  // Stepped settings-menu rows; the span outlives every Game.
  virtual std::span<const TitleIntSetting> titleIntSettings() const {
    return {};
  }

  // Legacy views, bound only by LegacyGameRuntimeAdapter.
  const GameConfig *legacyConfigForMigration() const {
    return legacyConfig_;
  }
  const GameHooks *legacyHooksForMigration() const {
    return legacyHooks_;
  }

protected:
  GameRuntime() = default;

  void bindLegacyInterface(const GameConfig *config, const GameHooks *hooks) {
    legacyConfig_ = config;
    legacyHooks_ = hooks;
  }

  const GameHooks *legacyHooks() const {
    return legacyHooks_;
  }

private:
  const GameConfig *legacyConfig_ = nullptr;
  const GameHooks *legacyHooks_ = nullptr;
};

} // namespace psx::game

// Shipping queries used by every renderer path.
bool game_guest_vram_is_picture(const Game &game);
bool game_guest_picture_is_native_width(const Game &game);
