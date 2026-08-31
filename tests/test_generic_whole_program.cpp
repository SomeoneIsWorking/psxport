// The generic whole-program profile is the bare-port baseline: run generated guest code until its
// measured libetc VSync, let the shell present one frame, then resume the exact suspended call stack.
#include "frame_loop_shell.h"
#include "game.h"
#include "game_runtime.h"
#include "platform_hle.h"
#include "recomp_iface.h"
#include "testutil.h"

#include <cstdint>
#include <memory>

namespace {

constexpr uint32_t kEntry = 0x80010000u;
constexpr uint32_t kVSync = 0x80010100u;

int guestEntries = 0;
int guestResumes = 0;
int guestVSyncReturns = 0;
uint32_t firstVSyncReturn = 0;

void generatedMain(Core *core, uint32_t address) {
  if (address != kEntry) {
    return;
  }
  ++guestEntries;
  core->game->platform_hle.lookup(kVSync)(core);
  ++guestResumes;
  firstVSyncReturn = core->r[2];
  ++guestVSyncReturns;
  core->game->platform_hle.lookup(kVSync)(core);
}

void setGeneratedOverride(uint32_t, RecOverrideFn) {}

const RecWholeProgramMetadata kWholeProgram = {
    .entryAddress = kEntry,
    .vsyncAddress = kVSync,
};

const RecompRegistry kRegistry = {
    generatedMain,
    nullptr,
    nullptr,
    0,
    setGeneratedOverride,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    &kWholeProgram,
};

class WholeProgramRuntime final : public GameRuntime {
public:
  WholeProgramRuntime() {
    image_.residentText = {.begin = 0x00010000u, .end = 0x00010200u};
  }

  void *createContext(Core &) override {
    return nullptr;
  }
  void destroyContext(void *) override {}
  void registerOverrides(Game &) override {}
  void bootInit(Core &) override {}
  RenderCapabilities renderCapabilities() const override {
    return RenderCapabilities::direct();
  }
  bool guestVramIsPicture(const Game &) const override {
    return true;
  }
  const GuestProgramImage *guestProgramImage() const override {
    return &image_;
  }
  const GenericWholeProgramProfile *genericWholeProgramProfile() const override {
    return &program_;
  }

private:
  GenericWholeProgramProfile program_{};
  GuestProgramImage image_{};
};

} // namespace

static void test_generated_program_yields_at_vsync_and_resumes_after_present() {
  guestEntries = 0;
  guestResumes = 0;
  guestVSyncReturns = 0;
  firstVSyncReturn = 0;
  WholeProgramRuntime runtime;
  psxport_install_game(runtime);
  psxport_install_recomp(&kRegistry);
  auto game = std::make_unique<Game>();
  game->diff_mode = true; // presentation still fences without opening a GPU/window.

  FrameLoopShell shell;
  shell.prepareProduct(*game);
  shell.prepareProduct(*game); // The shared boot paths may preflight an already-prepared Game.
  shell.step(game->core, 0u);

  CHECK_EQ(guestEntries, 1);
  CHECK_EQ(guestResumes, 0);
  CHECK_EQ(guestVSyncReturns, 0);
  CHECK_EQ(game->presentation.fence(), 1u);
  CHECK_EQ(game->timing.vblank, 1u);

  shell.step(game->core, 1u);
  CHECK_EQ(guestEntries, 1);
  CHECK_EQ(guestResumes, 1);
  CHECK_EQ(guestVSyncReturns, 1);
  CHECK_EQ(firstVSyncReturn, 1u);
  CHECK_EQ(game->presentation.fence(), 2u);
  CHECK_EQ(game->timing.vblank, 2u);

  psxport_install_recomp(nullptr);
}

int main() {
  RUN(generated_program_yields_at_vsync_and_resumes_after_present);
  return pt_summary();
}
