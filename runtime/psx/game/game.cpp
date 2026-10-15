#include "game.h"
#include "bios_load_exec.h" // the disc reader A0:0x51 LoadAndExecute is bound with, below
#include "config_vars.h"    // producer_census, render_path
#include "gpu_vk_device.h"
#include "gpu_vk_internal.h"
#include "ot_attr.h" // g_producer_census_armed — armed by every Game's constructor, below
#include "xa_state.h"

#include <lucent/log.h>

psx::game::HostIdentity Game::hostIdentity() const {
  const psx::game::HostIdentity *declared = runtime.hostIdentity();
  return declared ? *declared : psx::game::HostIdentity{};
}

Game::Game(psx::game::GameRuntime &runtime, const psx::config::Config &config)
    : config(config), core(runtime), gpuVk_(std::make_unique<GpuVkState>()),
      ownedGpuDevice(std::make_unique<GpuDevice>()), gpu_dev(*ownedGpuDevice), runtime(runtime) {
  wireRuntimeMembers();
}

// A Game that presents through a device the caller owns and outlives this Game.
Game::Game(psx::game::GameRuntime &runtime, GpuDevice &presentation, const psx::config::Config &config)
    : config(config), core(runtime), gpuVk_(std::make_unique<GpuVkState>()), gpu_dev(presentation), runtime(runtime) {
  wireRuntimeMembers();
}

void Game::wireRuntimeMembers() {
  // PSXPORT_PRODUCERS is armed per Game, before any guest store.
  g_producer_census_armed = psx::config::producer_census();
  core.game = this;
  gpuVk_->game = this;
  gpuVk_->device = &gpu_dev;
  timing.game = this;
  hostInput.attachOverlay(&rml_overlay); // one drain feeds the pad's key state AND the overlay
  pad.game = this;
  hle.game = this;
  sio.game = this;
  cd.game = this;
  fmv.game = this;
  spu_audio.game = this;
  rml_overlay.game = this;
  platform_hle.game = this;
  memcard.game = this;
  dbg_server.game = this;
  mods.declareTitleSettings(runtime.titleIntSettings());
  mods.init(runtime.interpolates(), config.fps60);
  core.rsub.mode.setPath(psx::config::render_path());
  lucent::info("render", "render path: {}", psx::gpu::renderPathName(core.rsub.mode.path()));
  cdc.reset();
  timing.bindCdcClock(&cdc);
  xa.reset();
  // BIOS A0:0x51 LoadAndExecute reads the executable the guest named through this run's disc, bound
  // here with every other per-Core owner rather than at the dispatch site.
  hle.loadExecReader = psx::hle::discFileReader(disc);
  cdc.disc = &disc;
  cdc.xa = &xa;
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
  core.rsub.census.report("producer census at shutdown");
  releaseHardwareBindings();
  // Unconditional, because the interesting answer is often "nothing was ever read". A run that
  // stalled three seconds in one chd_read and a run that never touched the disc are different
  // facts, and only the denominators tell them apart (Spyro issue 0115).
  disc.readReport("disc hunk cache at shutdown");
  // Same reason: without a syscall count a run that reached gameplay through the save menu and one
  // that stalled in the title produce identical card output.
  memcard.syscallLog().report("at shutdown");
  // This Game's device objects die here, while the device they were created on is alive.
  gpuVk_->release_device_resources();
}

void Game::disablePresentation() {
  gpu_dev.disable();
}
