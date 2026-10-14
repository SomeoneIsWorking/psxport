// test_field_turn.cpp — psx::FieldTurn: the per-field services a loop owes, and the two ways a loop
// silently failed to owe them.
//
// 1. The mid-run RAM dump (`PSXPORT_RAMDUMP_FRAME`). It used to live inside one framework loop, so
//    every title-owned loop had a bound, audited, unread knob: the audit line said the surface existed
//    and nothing ever called it. Now the turn owns it, and the observable is the file itself — written
//    at exactly the requested frame number, and at no other.
// 2. The client pause. `beginField` is where a pending `step` is consumed, so a turn that forgot the
//    pause would leave a client's single-step request sitting unconsumed while fields kept running.
#include "stub_game_runtime.h"
#include "testutil.h"

#include "config_var.h"
#include "core.h"
#include "dbg_server.h"
#include "field_turn.h"
#include "frame_loop_shell.h"
#include "frame_presenter.h"
#include "game.h"
#include "game_runtime.h"
#include "platform_hle.h"
#include "render_queue.h"
#include "stock_vsync.h"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <string>

namespace {

constexpr std::uint32_t kVSyncAddress = 0x00014000u;
constexpr std::size_t kRamDumpBytes = 0x200000u;

// A host backend that advances the presentation fence and touches no device: the same seam
// `test_frame_loop_shell` uses, so a field step can run without a GPU.
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

class Driver final : public psx::game::FrameDriver {
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

class Runtime final : public psx::test::StubGameRuntime {
public:
  void registerOverrides(Game &game) override {
    vsync_.bind(game);
  }

private:
  const psx::hle::StockVSync vsync_{kVSyncAddress, 0u};
};

struct Fixture {
  Runtime runtime;
  std::unique_ptr<Game> game;
  Driver *driver = nullptr;

  Fixture() {
    game = std::make_unique<Game>(runtime);
    runtime.registerOverrides(*game);
    auto owned = std::make_unique<Driver>();
    owned->game = game.get();
    driver = owned.get();
    game->frameDriver = std::move(owned);
  }
};

// One field the way a product loop runs it: the turn's halves around the title's frame step.
void runField(Game &game, std::uint32_t frame) {
  const psx::frame::FieldTurn turn;
  turn.beginField(game.core);
  psx::frame::FrameLoopShell{}.step(game.core, frame);
  turn.endField(game.core, frame);
}

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
  psx::frame::FrameLoopShell{}.prepareProduct(*fixture.game);
  const std::string path = "test_field_turn_ram.bin";
  remove(path.c_str());
  {
    // No run-end report here: it owes the AFTER-loop dump to this same path, and the test is about the
    // mid-run frame.
    const RamDumpKnobs knobs{path, "2"};
    for (std::uint32_t frame = 0; frame < 4u; ++frame) {
      runField(*fixture.game, frame);
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
  psx::frame::FrameLoopShell{}.prepareProduct(*fixture.game);
  const std::string path = "test_field_turn_unset.bin";
  remove(path.c_str());
  {
    const RamDumpKnobs knobs{path, "7"};
    for (std::uint32_t frame = 0; frame < 3u; ++frame) {
      runField(*fixture.game, frame);
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
  psx::frame::FrameLoopShell{}.prepareProduct(*fixture.game);
  fixture.game->dbg_server.setPaused(true);
  fixture.game->dbg_server.addStep(1);
  CHECK(fixture.game->dbg_server.stepPending());
  // One field only: with the pause still frozen and no step left, a second would idle until a client
  // arrived.
  runField(*fixture.game, 0u);
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