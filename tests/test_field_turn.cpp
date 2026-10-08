// test_field_turn.cpp — psx::FieldTurn: the per-field services a loop owes, and the two ways a loop
// silently failed to owe them.
//
// 1. The mid-run RAM dump (`PSXPORT_RAMDUMP_FRAME`). It used to live inside `native_boot.cpp`'s loop, so
//    every title-owned loop had a bound, audited, unread knob: the audit line said the surface existed
//    and nothing ever called it. Now the turn owns it, and the observable is the file itself — written
//    at exactly the requested frame number, and at no other.
// 2. The client pause. `beginField` is where a pending `step` is consumed, so a turn that forgot the
//    pause would leave a client's single-step request sitting unconsumed while fields kept running.
#include "testutil.h"

#include "config_var.h"
#include "core.h"
#include "dbg_server.h"
#include "field_turn.h"
#include "frame_presenter.h"
#include "game.h"
#include "game_runtime.h"
#include "machine.h"
#include "platform_hle.h"
#include "render_queue.h"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <string>

namespace {

constexpr std::uint32_t kVSyncAddress = 0x00014000u;
constexpr std::size_t kRamDumpBytes = 0x200000u;

// A host backend that advances the presentation fence and touches no device: the same seam
// `test_frame_loop_shell` and `test_machine_composition` use, so a field step can run without a GPU.
class FenceBackend final : public psx::frame::FramePresentationBackend {
public:
  void emit(std::span<const RqItem>) override {}
  void presentReal() override {}
  void captureDiagnostic(std::uint64_t, bool) override {}
  void pace(int, int) override {}
  void reconcile(std::uint64_t) override {}
  void beginLedgerFrame() override {}
  bool interpolatesRecords() const override {
    return false;
  }
  bool sealedFrameIsCut() override {
    return false;
  }
  void presentInBetween() override {}
  psx::gpu::RecordRect displayedBuffer() override {
    return {};
  }
};

class Driver final : public FrameDriver {
public:
  void stepFrame(Core &core, std::uint32_t frame) override {
    ++steps;
    lastFrame = frame;
    // Mark the RAM so a dump is distinguishable from a zero-filled buffer: the dump is the guest's
    // memory, not a constant block this test wrote itself.
    core.ram[0] = 0xA5u;
    core.ram[1] = 0x5Au;
    game->presentation.commitUnpresented(backend_);
  }
  unsigned steps = 0;
  std::uint32_t lastFrame = 0;
  Game *game = nullptr;

private:
  FenceBackend backend_;
};

class Runtime final : public GameRuntime {
public:
  void *createContext(Core &) override {
    return nullptr;
  }
  void destroyContext(void *) override {}
  void bootInit(Core &) override {}
  RenderCapabilities renderCapabilities() const override {
    return RenderCapabilities::direct();
  }
  bool guestVramIsPicture(const Game &) const override {
    return false;
  }
  void registerOverrides(Game &) override {}
  const PlatformHlePlan *platformHlePlan() const override {
    return &plan;
  }
  PlatformHlePlan plan{};
};

struct Fixture {
  Runtime runtime;
  std::unique_ptr<Game> game;
  Driver *driver = nullptr;

  Fixture() {
    runtime.plan.vsyncAddress = kVSyncAddress;
    runtime.plan.windowLo[0] = kVSyncAddress;
    runtime.plan.windowHi[0] = kVSyncAddress + 4u;
    psxport_install_game(runtime);
    game = std::make_unique<Game>();
    auto owned = std::make_unique<Driver>();
    owned->game = game.get();
    driver = owned.get();
    game->frameDriver = std::move(owned);
  }
};

// The RAM-dump knobs are process-wide CVar state, so a test that writes them must put them back or it
// decides the next test's behaviour.
struct RamDumpKnobs {
  RamDumpKnobs(const std::string &path, const std::string &frame) {
    psx::config::cv_ramdump.set(psx::config::Layer::Runtime, path);
    psx::config::cv_ramdump_frame.set(psx::config::Layer::Runtime, frame);
  }
  ~RamDumpKnobs() {
    psx::config::cv_ramdump.clear(psx::config::Layer::Runtime);
    psx::config::cv_ramdump_frame.clear(psx::config::Layer::Runtime);
  }
};

std::size_t fileSize(const std::string &path) {
  FILE *file = fopen(path.c_str(), "rb");
  if (file == nullptr) {
    return 0u;
  }
  fseek(file, 0, SEEK_END);
  const long size = ftell(file);
  fclose(file);
  return size < 0 ? 0u : static_cast<std::size_t>(size);
}

// The dump is the turn's, at the frame the LOOP numbered — not at a counter the turn keeps itself.
static void test_the_mid_run_ram_dump_happens_at_the_requested_frame(void) {
  Fixture fixture;
  psx::Machine machine{*fixture.game};
  machine.prepare();
  const std::string path = "test_field_turn_ram.bin";
  remove(path.c_str());
  {
    // Fields are stepped directly rather than through `run`, because the run ALSO owes the AFTER-loop
    // dump and that one would write this same path — the test is about the mid-run frame, so the run-end
    // dump must not be able to produce what it asserts.
    const RamDumpKnobs knobs{path, "2"};
    for (std::uint32_t frame = 0; frame < 4u; ++frame) {
      machine.stepFrame(frame);
    }
  }
  CHECK_EQ(fixture.driver->steps, 4u);
  CHECK_EQ(fixture.driver->lastFrame, 3u);
  CHECK_EQ(fileSize(path), kRamDumpBytes);
  // The guest's own bytes, not a filled buffer: a dump taken before the field ran would not carry them.
  FILE *dump = fopen(path.c_str(), "rb");
  unsigned char head[2] = {0u, 0u};
  if (dump != nullptr) {
    CHECK_EQ(fread(head, 1, 2u, dump), 2u);
    fclose(dump);
  }
  CHECK_EQ(head[0], 0xA5u);
  CHECK_EQ(head[1], 0x5Au);
  remove(path.c_str());
}

static void test_no_requested_frame_writes_nothing(void) {
  Fixture fixture;
  psx::Machine machine{*fixture.game};
  machine.prepare();
  const std::string path = "test_field_turn_unset.bin";
  remove(path.c_str());
  {
    const RamDumpKnobs knobs{path, "7"};
    for (std::uint32_t frame = 0; frame < 3u; ++frame) {
      machine.stepFrame(frame);
    }
  }
  CHECK_EQ(fixture.driver->steps, 3u);
  CHECK_EQ(fileSize(path), 0u);
  remove(path.c_str());
}

// A client's single step is consumed by the turn, which is where the pause is answered. A turn that
// omitted the pause would let fields run with the request still pending — the exact failure a paused
// product must not have.
static void test_the_client_step_is_consumed_by_the_turn(void) {
  Fixture fixture;
  psx::Machine machine{*fixture.game};
  machine.prepare();
  fixture.game->dbg_server.setPaused(true);
  fixture.game->dbg_server.addStep(1);
  CHECK(fixture.game->dbg_server.stepPending());
  // `quit` is what lets this run end: with the pause still frozen and no step left, the second field
  // would idle until a client arrived, and a test must never wait for one.
  fixture.game->dbg_server.requestQuit();
  machine.run(0u);
  CHECK_EQ(fixture.driver->steps, 1u);
  CHECK(!fixture.game->dbg_server.stepPending());
}

} // namespace

int main() {
  RUN(the_mid_run_ram_dump_happens_at_the_requested_frame);
  RUN(no_requested_frame_writes_nothing);
  RUN(the_client_step_is_consumed_by_the_turn);
  return pt_summary();
}