#include "game.h"
#include "config_vars.h" // cv_producers — PSXPORT_PRODUCERS, read once per Game below
#include "gte_state.h"   // GTE_BindState — released below
#include "mdec_state.h"  // MDEC_BindState — released below
#include "ot_attr.h"     // g_producer_census_armed — armed by every Game's constructor, below
#include "spu_state.h"   // SPU_BindState, spu_bind_log, spu_bind_irq_core — released below
#include "xa_state.h"    // xa_bind_state — released below

HostIdentity Game::hostIdentity() const {
  if (core.cfg) {
    return {core.cfg->windowTitle, core.cfg->cardEnvVar, core.cfg->cardDefaultPath};
  }
  const HostIdentity *declared = runtime ? runtime->hostIdentity() : nullptr;
  return declared ? *declared : HostIdentity{};
}

Game::Game() : ownedGpuDevice(std::make_unique<GpuDevice>()), gpu_dev(*ownedGpuDevice) {
  wireRuntimeMembers();
}

// A Game that presents through a device the caller owns. Nothing here may create or release one: the
// device outlives this Game by contract, and the process claim below points at it exactly as it would
// for a self-owning Game.
Game::Game(GpuDevice &presentation) : gpu_dev(presentation) {
  wireRuntimeMembers();
}

void Game::wireRuntimeMembers() {
  // THE PRODUCER-CENSUS ARM, from PSXPORT_PRODUCERS. This used to be assigned only inside
  // native_boot_run, which direct-boot runtimes (Tekken 3's bootInit dispatch) never execute, so the
  // knob silently did nothing there. Game construction is on EVERY runtime's route — native_boot_run
  // itself dereferences c->game, which only this constructor sets — so arming here covers them all
  // and runs BEFORE any guest store, preserving the old site's before-guest-execution guarantee
  // (it is in fact strictly earlier). Idempotent: SBS constructs two Games and each re-arms
  // identically. Read via .get(), whose lazy env binding needs no init call (config_var.h); the cost
  // of the gate this arms is measured in ot_attr.h.
  g_producer_census_armed = psx::config::cv_producers.get();
  runtime = core.runtime;
  core.game = this;
  gpu.game = this;
  gpu_vk.game = this;
  timing.game = this;
  pad.game = this;
  hle.game = this;
  sio.game = this;
  rq.game = this;
  pcSched.game = this;
  cd.game = this;
  fmv.game = this;
  stub.game = this;
  spu_audio.game = this;
  rml_overlay.game = this;
  platform_hle.game = this;
  memcard.game = this;
  dbg_server.game = this;
  if (!GpuDevice::sInstance) {
    GpuDevice::sInstance = &gpu_dev;
  }
  mods.init(runtime ? runtime->renderCapabilities() : RenderCapabilities::direct());
  disc_state_init(&disc);
  cdc_state_init(&cdc);
  timing.bindCdcClock(&cdc);
  xa_state_init(&xa);
  gte.dbg.sxhist_on = gte.dbg.gteprobe = gte.dbg.projprobe = gte.dbg.rtpcaller_on = -1;
  disc.env_key = core.cfg ? core.cfg->discEnvVar : (runtime ? runtime->discEnvVar() : nullptr);
  cdc.disc = &disc;
  cdc.xa = &xa;
  xa.disc = &disc;

  // Factories receive a fully wired Game. Direct runtimes create no temporal decorator by default;
  // compatibility runtimes opt in explicitly through their factory override.
  if (runtime) {
    temporalPresentation = runtime->createTemporalFramePresentation(*this);
    frameDriver = runtime->createFrameDriver(*this);
    taskScheduler = runtime->createTaskScheduler(*this);
  }

  // Every port constructs a Game, including ports that bypass native_boot_run. Keep the shipping GPU
  // self-test reachable from that common boundary; leaving tritest compiled but uncalled made
  // PSXPORT_GPU_SELFTEST audit as an unknown no-op and falsely implied its shader checks were running.
  gpu_vk.tritest();
}

// The Beetle peripherals reach their per-instance state through process-wide bind points that the title's
// frame step re-establishes from its explicit Core. A bind point that outlives the Game it names is a
// dangling write for whatever runs next in this process (measured: a second Game's gte_init wrote through
// the first Game's freed GTE registers). Every one is returned to its shared default here, while the
// storage it names is still alive.
void Game::releaseHardwareBindings() {
  GTE_BindState(nullptr);
  SPU_BindState(nullptr);
  spu_bind_log(nullptr);
  spu_bind_irq_core(nullptr);
  MDEC_BindState(nullptr);
  xa_bind_state(nullptr);
  core.rsub.projParams.release();
  core.rsub.projprim.release();
}

Game::~Game() {
  releaseHardwareBindings();
  // Unconditional, because the interesting answer is often "nothing was ever read". A run that
  // stalled three seconds in one chd_read and a run that never touched the disc are different
  // facts, and only the denominators tell them apart (Spyro issue 0115).
  disc_read_report(&disc, "disc hunk cache at shutdown");
  // Same reason, and the case that prompted it: a Spyro run that reached gameplay through the
  // save menu and one that stalled in the title for 12,000 fields produced identical card
  // output, because nothing counted the syscalls either made (issue 0123).
  memcard.syscallLog().report("at shutdown");
  // GpuDevice is declared after gpu_vk and therefore dies first during member teardown. Release this
  // per-Game retained texture while its owning SDL device is still alive; GpuVkState's destructor then
  // only clears the already-empty policy state.
  gpu_vk.release_native_composite_capture();
  // Only a Game that OWNS the device releases the process claim. A Game presenting through a
  // host-owned device must leave it: the host outlives this Game and every later session's Game will
  // claim the very same device.
  if (ownedGpuDevice && GpuDevice::sInstance == &gpu_dev) {
    GpuDevice::sInstance = nullptr;
  }
}
