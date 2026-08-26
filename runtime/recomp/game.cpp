#include "game.h"
#include "config_vars.h" // cv_producers — PSXPORT_PRODUCERS, read once per Game below
#include "ot_attr.h"     // g_producer_census_armed — armed by every Game's constructor, below

Game::Game() {
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
  verify.core = &core;
  if (!GpuDevice::sInstance) {
    GpuDevice::sInstance = &gpu_dev;
  }
  mods.init(runtime ? runtime->renderCapabilities() : RenderCapabilities::direct());
  disc_state_init(&disc);
  cdc_state_init(&cdc);
  timing.bindCdcClock(&cdc);
  xa_state_init(&xa);
  gte.dbg.sxhist_on = gte.dbg.gteprobe = gte.dbg.projprobe = gte.dbg.rtpcaller_on = -1;
  disc.env_key = core.cfg ? core.cfg->discEnvVar : 0;
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
}

Game::~Game() = default;
