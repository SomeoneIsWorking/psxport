#include "game.h"
#include "bios_load_exec.h"
#include "config_vars.h"
#include "ot_attr.h"
#include "render_device.h"
#include "render_state.h"
#include "xa_state.h"

#include <cstdlib>
#include <lucent/log.h>

psx::game::HostIdentity Game::hostIdentity() const {
  const psx::game::HostIdentity *declared = runtime.hostIdentity();
  return declared ? *declared : psx::game::HostIdentity{};
}

psx::game::FrameDriver &Game::requireFrameDriver() const {
  if (frameDriver == nullptr) {
    lucent::error("frame-loop", "the title runtime installed no FrameDriver");
    std::abort();
  }
  return *frameDriver;
}

Game::Game(psx::game::GameRuntime &runtime, const psx::config::Config &config)
    : config(config), ownedRenderDevice(std::make_unique<psx::render::RenderDevice>()),
      renderDevice(*ownedRenderDevice), ownedHostInput(std::make_unique<psx::input::HostInput>()), core(runtime),
      hostInput(*ownedHostInput), render_(std::make_unique<psx::render::RenderState>(*this, renderDevice)),
      runtime(runtime) {
  wireRuntimeMembers();
}

Game::Game(psx::game::GameRuntime &runtime,
           psx::render::RenderDevice &presentation,
           psx::input::HostInput &input,
           const psx::config::Config &config)
    : config(config), renderDevice(presentation), core(runtime), hostInput(input),
      render_(std::make_unique<psx::render::RenderState>(*this, renderDevice)), runtime(runtime) {
  wireRuntimeMembers();
}

void Game::wireRuntimeMembers() {
  // PSXPORT_PRODUCERS is armed per Game, before any guest store.
  g_producer_census_armed = psx::config::producer_census();
  core.game = this;
  timing.game = this;
  hostInput.attachOverlay(&rml_overlay);
  pad.game = this;
  hle.game = this;
  sio.game = this;
  cd.game = this;
  fmv.game = this;
  spu_audio.game = this;
  rml_overlay.game = this;
  platform_hle.game = this;
  memcard.game = this;
  mods.declareTitleSettings(runtime.titleIntSettings());
  mods.init(runtime.interpolates());
  core.rsub.mode.setPath(psx::config::render_path());
  lucent::info("render", "render path: {}", psx::present::renderPathName(core.rsub.mode.path()));
  cdc.reset();
  timing.bindCdcClock(&cdc);
  xa.reset();
  // BIOS A0:0x51 LoadAndExecute reads the executable the guest named through this run's disc, bound
  // here with every other per-Core owner rather than at the dispatch site.
  hle.loadExecReader = psx::hle::discFileReader(disc);
  cdc.disc = &disc;
  cdc.xa = &xa;
  xa.drive = &cdc;
  xa.disc = &disc;

  // Factories receive a fully wired Game; the session first, so the products can reach it.
  sessionState = runtime.createSession(*this);
  frameDriver = runtime.createFrameDriver(*this);
}

// A process-wide bind point that still names this Game would dangle once it is freed; one that names
// another live Game is left alone.
void Game::releaseHardwareBindings() {
  core.gte().release();
  spu.release();
  mdec.release();
  xa.releaseSamplePull();
}

Game::~Game() {
  hostInput.detachOverlay(&rml_overlay);
  core.rsub.census.report("producer census at shutdown");
  releaseHardwareBindings();
  // Unconditional, because the interesting answer is often "nothing was ever read". A run that
  // stalled three seconds in one chd_read and a run that never touched the disc are different
  // facts, and only the denominators tell them apart (Spyro issue 0115).
  disc.readReport("disc hunk cache at shutdown");
  // Same reason: without a syscall count a run that reached gameplay through the save menu and one
  // that stalled in the title produce identical card output.
  memcard.syscallLog().report("at shutdown");
}

void Game::disablePresentation() {
  renderDevice.disable();
}
