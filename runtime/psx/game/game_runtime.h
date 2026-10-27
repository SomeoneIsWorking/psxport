// game_runtime.h — the title interface: one GameRuntime subclass per title, called through virtuals.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "execution_exit.h"
#include "field_cadence.h"
#include "guest_address_range.h"
#include "guest_packet_pool_windows.h"
#include "input_phase.h"
#include "logo_image.h"
#include "pad_delivery.h"
#include "psx_exe_image.h"
#include "title_settings.h"

class Core;
class Game;
namespace psx::cd {
struct StockReadLanding;
}
namespace psx::present {
class GuestWidescreenProjection;
class StateProducer;
} // namespace psx::present
namespace psx::state {
class NativeStatePort;
} // namespace psx::state

namespace psx::game {

class TitleDebugMenu;

// Where a title's memory card and user data live; PSXPORT_CARD overrides the card for every title.
struct HostIdentity {
  const char *cardDefaultPath = nullptr; // used when PSXPORT_CARD names no card
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
  // The sound-test track playing now; empty when stopped.
  virtual std::string_view nowPlayingName() const {
    return {};
  }
  // The sound test's tracks; an empty name shows as its number.
  virtual int trackCount() const {
    return 0;
  }
  virtual std::string_view trackName(int track) const {
    (void)track;
    return {};
  }
  // Plays sound-test track `track`, or stops on a negative value.
  virtual void soundTestPlay(int track) {
    (void)track;
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
  // The host deadline a present representing `cadence` is shown at, delivering its simulated fields; a title
  // whose field scheduler already advances the devices only plans it. nullopt when unpaced.
  virtual std::optional<double> presentDeadline(Core &core, frame::FieldCadence cadence);
  // Lines the guest scans out, or 0 when the decoded display height is right.
  virtual int displayHeight() const {
    return 0;
  }
  // Optional title-owned guest projection.
  virtual const psx::present::GuestWidescreenProjection *guestWidescreenProjection() const {
    return nullptr;
  }
  // The title's logo for its picker panel, read from its live machine.
  virtual std::optional<psx::host::LogoImage> panelLogo(Core &core) const {
    (void)core;
    return std::nullopt;
  }

  // The guest picture covers only the native width, so the present samples those columns alone.
  virtual bool guestPictureIsNativeWidth(const Game &game) const {
    (void)game;
    return false;
  }

  // ---- Debug and run

  // The title's debug verbs, readouts, sound test and About lines; outlives every Game.
  virtual const TitleDebugMenu &debugMenu() const = 0;
  virtual TitleAudio *audio(Core &core) const {
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
  // ---- Identity data
  virtual const HostIdentity *hostIdentity() const {
    return nullptr;
  }
  // Stepped settings-menu rows; the span outlives every Game.
  virtual std::span<const TitleIntSetting> titleIntSettings() const {
    return {};
  }

protected:
  GameRuntime() = default;
};

} // namespace psx::game
