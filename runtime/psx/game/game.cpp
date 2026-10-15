#include "game.h"
#include "bios_load_exec.h" // the disc reader A0:0x51 LoadAndExecute is bound with, below
#include "config_vars.h"    // cv_producers — PSXPORT_PRODUCERS, read once per Game below
#include "ot_attr.h"        // g_producer_census_armed — armed by every Game's constructor, below
#include "xa_state.h"       // xa_state_init, psx::audio::releaseXaState

psx::game::HostIdentity Game::hostIdentity() const {
  const psx::game::HostIdentity *declared = runtime.hostIdentity();
  return declared ? *declared : psx::game::HostIdentity{};
}

Game::Game(psx::game::GameRuntime &runtime, const psx::config::Config &config)
    : config(config), core(runtime), ownedGpuDevice(std::make_unique<GpuDevice>()), gpu_dev(*ownedGpuDevice),
      runtime(runtime) {
  wireRuntimeMembers();
}

// A Game that presents through a device the caller owns. Nothing here may create or release one: the
// device outlives this Game by contract, and the process claim below points at it exactly as it would
// for a self-owning Game.
Game::Game(psx::game::GameRuntime &runtime, GpuDevice &presentation, const psx::config::Config &config)
    : config(config), core(runtime), gpu_dev(presentation), runtime(runtime) {
  wireRuntimeMembers();
}

void Game::wireRuntimeMembers() {
  // PSXPORT_PRODUCERS is armed per Game, before any guest store.
  g_producer_census_armed = psx::config::cv_producers.get();
  core.game = this;
  gpu.game = this;
  gpu_vk.game = this;
  timing.game = this;
  hostInput.attachOverlay(&rml_overlay); // one drain feeds the pad's key state AND the overlay
  pad.game = this;
  hle.game = this;
  sio.game = this;
  rq.game = this;
  cd.game = this;
  fmv.game = this;
  spu_audio.game = this;
  rml_overlay.game = this;
  platform_hle.game = this;
  memcard.game = this;
  dbg_server.game = this;
  if (!GpuDevice::sInstance) {
    GpuDevice::sInstance = &gpu_dev;
  }
  mods.declareTitleSettings(runtime.titleIntSettings());
  mods.init(runtime.interpolates(), runtime.renderCapabilities().defaultFaceOrder, config.fps60);
  // The declared render path is installed here so no title can run on a path it did not declare.
  render_path_install(&core);
  disc_state_init(&disc);
  cdc_state_init(&cdc);
  timing.bindCdcClock(&cdc);
  xa_state_init(&xa);
  // BIOS A0:0x51 LoadAndExecute reads the executable the guest named through this run's disc, bound
  // here with every other per-Core owner rather than at the dispatch site.
  hle.loadExecReader = psx::hle::discFileReader(disc);
  cdc.disc = &disc;
  cdc.xa = &xa;
  xa.disc = &disc;

  // Factories receive a fully wired Game; the session first, so the products can reach it.
  sessionState = runtime.createSession(*this);
  temporalPresentation = runtime.createTemporalFramePresentation(*this);
  frameDriver = runtime.createFrameDriver(*this);

  // Every product constructs a Game, so PSXPORT_GPU_SELFTEST runs here.
  gpu_vk.tritest();
}

// A process-wide bind point that still names this Game would dangle once it is freed; one that names
// another live Game is left alone.
void Game::releaseHardwareBindings() {
  core.gte().release();
  spu.release();
  mdec.release();
  psx::audio::releaseXaState(xa);
  core.rsub.projprim.release();
}

Game::~Game() {
  core.rsub.census.report("producer census at shutdown");
  releaseHardwareBindings();
  // Unconditional, because the interesting answer is often "nothing was ever read". A run that
  // stalled three seconds in one chd_read and a run that never touched the disc are different
  // facts, and only the denominators tell them apart (Spyro issue 0115).
  disc_read_report(&disc, "disc hunk cache at shutdown");
  // Same reason: without a syscall count a run that reached gameplay through the save menu and one
  // that stalled in the title produce identical card output.
  memcard.syscallLog().report("at shutdown");
  // GpuDevice is declared after gpu_vk and therefore dies first during member teardown. Release this
  // per-Game retained texture while its owning SDL device is still alive; GpuVkState's destructor then
  // only clears the already-empty policy state.
  gpu_vk.release_native_composite_capture();
  // Every other SDL device object this Game created — VRAM/CLUT textures, the depth and semi-blend
  // intermediates, the per-batch vertex buffers and their upload/download transfer buffers — dies
  // with the device unless it is released HERE, while the device is alive. Left to member teardown
  // they outlive it: the Vulkan validation layer reports VUID-vkDestroyDevice-device-05137 naming
  // the live VkBuffer, and the process dies on the way out.
  //
  // Those handles are the RENDERER'S PROCESS-GLOBAL pool, shared by every Core in the process, so only
  // the Game that owns the device may release them. A Game presenting through a host-owned device — a
  // picker panel session, a session handed a title host — is torn down while that owner is still
  // drawing with the pool; releasing it there takes the owner's renderer down with it. A process that
  // runs one Game at a time is unaffected: it owns the device, so it releases as before.
  if (ownedGpuDevice && GpuDevice::sInstance == &gpu_dev) {
    gpu_vk.release_device_resources();
    GpuDevice::sInstance = nullptr;
  }
}
