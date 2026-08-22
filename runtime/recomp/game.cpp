#include "game.h"

Game::Game() {
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
  mods.init();
  disc_state_init(&disc);
  cdc_state_init(&cdc);
  timing.bindCdcClock(&cdc);
  xa_state_init(&xa);
  gte.dbg.sxhist_on = gte.dbg.gteprobe = gte.dbg.projprobe = gte.dbg.rtpcaller_on = -1;
  disc.env_key = core.cfg ? core.cfg->discEnvVar : 0;
  cdc.disc = &disc;
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
