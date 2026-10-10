// game_runtime.h — polymorphic framework↔game ownership seam.
//
// A game derives one GameRuntime and owns that object for the process lifetime. Game-specific
// behavior belongs in that derived class or in the per-Game products it creates. The legacy
// GameConfig/GameHooks pair remains available only through the bounded adapter declared by
// game_iface.h while existing consumers migrate; it is not the second-generation interface. A real
// consumer derives that adapter until its narrow typed fact groups have replaced every cfg read.
#pragma once

#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <span>

#include "guest_packet_pool_windows.h"
#include "guest_pad_buffer_layout.h"
#include "guest_program_image.h"
#include "input_phase.h"
#include "logo_image.h"
#include "render_capabilities.h"
#include "title_settings.h"

class Core;
class Game;
class GuestWidescreenProjection;
namespace psx::cd {
struct StockReadLanding;
}
struct GuestCdStreamCallbackLayout;
namespace psx::frame {
class TemporalFramePresentation;
} // namespace psx::frame
struct GameConfig;
struct GameHooks;
struct PlatformHlePlan;
namespace psx::state {
class BlobReader;
class BlobWriter;
class NativeStatePort;
} // namespace psx::state

// The host-visible identity of a game: what its window is called and where its memory card lives.
// A DIRECT runtime (core.cfg == nullptr) declares it through GameRuntime::hostIdentity(); the legacy
// adapter's equivalent is GameConfig::windowTitle / cardEnvVar / cardDefaultPath, and Game::hostIdentity()
// is the one place that chooses between them. The framework reads the card variable itself, through
// its configuration owner (the environment, then ./.env), so a title names the key and never reads it.
struct HostIdentity {
  const char *windowTitle = nullptr;     // null shows the framework's deliberately untitled marker
  const char *cardEnvVar = nullptr;      // checked before the generic PSXPORT_CARD
  const char *cardDefaultPath = nullptr; // used when neither variable names a card
  const char *userDataName = nullptr;    // one path component under the OS user-data root (bug reports)
};

class FrameDriver {
public:
  virtual ~FrameDriver() = default;

  // One finite native frame. The title owns its measured input/audio/simulation/render/present order
  // and commits presentation exactly once (or supplies a measured unpresented fence). It must never
  // dispatch a guest-owned frame loop or call libetc VSync.
  virtual void stepFrame(Core &core, uint32_t frame) = 0;

  // Whether this driver is PAST whatever boot prefix its title owns — the logos, the loading screens
  // and the publisher cards that come before a title's own picture. A driver with no prefix is past
  // it from its first frame.
  //
  // This exists for hosts that show a title's picture before handing the session to the player (a
  // multi-title picker shows each title's own picture in a panel). A pixel test cannot tell a publisher's
  // logo from a title screen: both are filled, coherent and stable. The title's OWN phase can, and it
  // is the same signal that decides which renderer owns the frame.
  virtual bool pastBootPrefix() const {
    return true;
  }
};

class TaskScheduler {
public:
  virtual ~TaskScheduler() = default;

  virtual void step() = 0;
  virtual void yield(Core &core) = 0;
  virtual void tickSleepCountdown() {}
};

class GameRuntime {
public:
  virtual ~GameRuntime() = default;
  GameRuntime(const GameRuntime &) = delete;
  GameRuntime &operator=(const GameRuntime &) = delete;
  GameRuntime(GameRuntime &&) = delete;
  GameRuntime &operator=(GameRuntime &&) = delete;

  virtual void *createContext(Core &core) = 0;
  virtual void destroyContext(void *context) = 0;
  virtual void registerOverrides(Game &game) = 0;
  virtual void bootInit(Core &core) = 0;

  // Immutable facts about the guest executable image. A direct runtime owns the returned value for
  // at least the lifetime of every Core. Null is an honest answer for tools/smoke clients that never
  // boot or route guest code; those algorithms refuse by name when invoked without it.
  virtual const GuestProgramImage *guestProgramImage() const {
    return nullptr;
  }

  // Hardware-sync primitives for DIRECT runtimes (core.cfg == nullptr): the measured SCEI library
  // leaves this binary links and the windows that admit them. A native-frame product declares its
  // libetc VSync address here. Adapter runtimes keep the equivalent GameConfig::hle fact slice.
  // Null remains valid
  // for generic bare products and non-product smoke/tool clients that never enter `dc_boot_init`.
  virtual const PlatformHlePlan *platformHlePlan() const {
    return nullptr;
  }

  // Guest receive buffers for DIRECT runtimes (core.cfg == nullptr). The returned immutable facts
  // live for the runtime's process lifetime. Null is an honest answer for smoke/tool clients that
  // never ask Pad to publish input; serviceFrame then advances host/replay state without writing an
  // invented guest address. Adapter runtimes keep the equivalent legacy GameConfig fields.
  virtual const GuestPadBufferLayout *guestPadBufferLayout() const {
    return nullptr;
  }

  // Guest CD-ready callback slot and delivery owner for DIRECT runtimes (core.cfg == nullptr).
  // HostPump reads the current function value on each field; GuestInterrupt leaves delivery to the
  // guest's libcd ISR after the controller raises INT1. Adapter runtimes retain the legacy direct
  // GameConfig::cdReadyCbPtr behavior.
  virtual const GuestCdStreamCallbackLayout *guestCdStreamCallbackLayout() const {
    return nullptr;
  }

  // The guest RAM the title's 2D packet pool occupies, for DIRECT runtimes (core.cfg == nullptr).
  // `OtAttr` records packet-producer spans only inside this window, so a runtime that declares none
  // has a `GuestPacketFilter` that can never match anything — which reads as "the guest submitted
  // nothing here" rather than as "nobody said where the pool is". Adapter runtimes keep the legacy
  // GameConfig::packetPool* fields, which are read where they always were and are not re-read here.
  // The default declares no pool, which is the honest answer for a title whose pool is not located,
  // for a bare product, and for a smoke/tool client that never submits packets.
  virtual const GuestPacketPoolWindows *guestPacketPoolWindows() const {
    return nullptr;
  }

  // The guest RAM this title loads RUNTIME code modules into, for DIRECT runtimes. A title that
  // loads relocatable modules from the disc and calls their entries (C-12 reads `RELOCS/GT.LVB`
  // into its heap) needs those bytes to be an executable residency; the framework establishes one
  // when a CD transfer lands inside the declared window (`guest_code_module.h`). The default
  // declares none, which is the honest answer for a title whose code all lives in the resident
  // image and for a bare product.
  virtual GuestAddressRange guestCodeModuleWindow() const {
    return {};
  }

  // A synchronous stock `CdRead` just landed its whole payload in guest RAM (psx::cd::StockReadLanding).
  // The bytes are already written and already reported to the executable-write invalidation owner, so a
  // title that treats a streamed region as a code module or authored image publishes it here, after
  // the write, never before (an earlier publication would be subtracted by the write's own
  // invalidation). The default does nothing: most titles stream data.
  virtual void stockCdReadLanded(Core &, const psx::cd::StockReadLanding &) {}

  // Optional title-owned guest projection. The returned object declares the aspect only; the title
  // must publish a matching guest projection plan before the host exposes a wider presentation span.
  virtual const GuestWidescreenProjection *guestWidescreenProjection() const {
    return nullptr;
  }

  // Integer settings the title offers in the settings menu, one stepped row each, persisted in the
  // settings file. The returned span must outlive the runtime's Games; empty declares none.
  virtual std::span<const TitleIntSetting> titleIntSettings() const {
    return {};
  }

  // Disc environment key for DIRECT runtimes (core.cfg == nullptr). The disc resolver checks this
  // variable in the environment and ./.env before falling back to generic PSXPORT_DISC.
  virtual const char *discEnvVar() const {
    return nullptr;
  }

  // The title's INPUT PHASE for pad record/replay (input_phase.h): which screen or game state is
  // taking input this pad frame, read from guest state the title has identified. Recordings store
  // each frame as an offset from its phase's entry, so boot and load timing absorb at phase
  // boundaries instead of shifting every later press. The key is opaque to the framework and must be
  // stable for as long as the screen is — never a tick or counter. The default declares no phase:
  // recordings are then absolute from boot, and a phase-keyed recording is refused by name.
  virtual std::uint64_t inputPhase(Core &) const {
    return psx::input::kUnkeyedPhase;
  }

  // Window title and memory-card policy for DIRECT runtimes (core.cfg == nullptr). Null is honest for
  // smoke/tool clients and yields an untitled window and the generic card path.
  virtual const HostIdentity *hostIdentity() const {
    return nullptr;
  }

  // Which presentation products this title actually owns. Direct runtimes default to a native path
  // without temporal interpolation; a capable direct title opts in explicitly. The legacy adapter
  // preserves existing native+temporal consumers while they migrate.
  virtual RenderCapabilities renderCapabilities() const = 0;

  // Whether guest VRAM is picture content for the current frame. This is deliberately a runtime
  // policy, not an immutable configuration bit: a title may use guest uploads for boot logos and
  // later hand the whole frame to native producers. The renderer asks at each present so the
  // derived title runtime can answer from its actual render mode.
  virtual bool guestVramIsPicture(const Game &game) const = 0;

  // Whether this frame's guest picture covers only the game's native width even when the display is
  // widened (an upload-only boot logo drawn before any widening applies). The presenter then samples
  // only those columns and letterboxes them at 4:3 instead of showing the unwritten ones.
  virtual bool guestPictureIsNativeWidth(const Game &) const {
    return false;
  }

  // Optional temporal decorator. Direct runtimes default to the neutral current-frame presenter and
  // therefore instantiate no interpolation history. Legacy consumers keep their existing behavior via
  // LegacyGameRuntimeAdapter until they declare the narrower contract directly.
  virtual std::unique_ptr<psx::frame::TemporalFramePresentation> createTemporalFramePresentation(Game &game);

  // The default presenter delivers simulated fields and waits for their host deadline. A title
  // whose field scheduler already advances devices overrides this with host waiting only.
  virtual void pacePresentation(Core &core, int guestFields, int parts);

  // Whether the logic frame just sealed is a cut (camera cut, scene change, teleport, respawn at the
  // same address), answered from the guest state the game itself uses for it. A cut's in-between is
  // the frame itself.
  virtual bool sealedFrameIsCut(Core &) const {
    return false;
  }

  // Native ports override this with their measured finite driver. A runtime guest-execution product
  // may instead drive bounded turns explicitly through psx::cpu::dispatchGuest.
  virtual std::unique_ptr<FrameDriver> createFrameDriver(Game &);
  virtual std::unique_ptr<TaskScheduler> createTaskScheduler(Game &) {
    return nullptr;
  }

  // Title-specific developer commands. The framework REPL owns parsing for framework state only;
  // guest addresses, object layouts, and title subsystems stay behind this game-owned boundary.
  virtual bool replCommand(Core &, const char *, const char *) {
    return false;
  }

  // Title-specific commands on the live control channel (the loopback debug endpoint). `out` is the
  // reply stream. Return true when the command was this title's. Unlike replCommand, whose reply goes to
  // the log, this answers the client that asked.
  virtual bool controlCommand(Core &, const char *, const char *, FILE *) {
    return false;
  }

  // TITLE-OWNED NATIVE STATE, for the whole-machine save state (runtime/psx/state/machine_state.h).
  //
  // A title whose native owners — frame driver, field scheduler, boot-sequence state, picker
  // session — hold state outside guest RAM returns a port here; a title with none returns null and
  // no title section is ever written. This is the ONLY way to declare native owners, so "a title has
  // native state" and "a title implements it" cannot be two claims that come apart.
  //
  // A LOAD refuses rather than desyncs: a file whose title section does not match this port's name
  // and version is rejected by name, and so is a title that owns native state the file does not
  // carry. Neither direction is recoverable by trying harder later — the run is already wrong.
  //
  // The port is asked for with the Core whose state is being saved or loaded, because a title's
  // native owners live in that Core's own context (`createContext`), not in this runtime object.
  virtual psx::state::NativeStatePort *nativeState(Core &) const {
    return nullptr;
  }

  // The title's own logo for its picker panel, read from its live machine; nullopt until there is one.
  virtual std::optional<psx::host::LogoImage> panelLogo(Core &) const {
    return std::nullopt;
  }

  // Title-specific end-of-run reporting, after the framework's run-complete line.
  virtual void reportRun(Core &) const {}

  // Non-virtual compatibility views. Direct runtimes return null; only
  // LegacyGameRuntimeAdapter binds them while a consumer migrates.
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

// Installs one game-owned derived runtime before constructing any Game. The caller retains
// ownership; SBS Games share the same immutable process-lifetime runtime object.
void psxport_install_game(GameRuntime &runtime);
GameRuntime *psxport_game_runtime();

// Checked shipping query used by every renderer path. A missing runtime is an installation defect,
// not an implicit answer about the picture, and therefore refuses instead of returning false.
bool game_guest_vram_is_picture(const Game &game);
bool game_guest_picture_is_native_width(const Game &game);
