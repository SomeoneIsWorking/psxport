// The title interface's per-title answers reach their framework consumers: each case overrides one
// GameRuntime virtual on the stub runtime and reads it back through the shipping owner.
#include "crt0_boot.h"
#include "dev_command_control.h"
#include "display_presenter.h"
#include "frame_composer.h"
#include "frame_presenter.h"
#include "game.h"
#include "gte.h"
#include "image_identity.h"
#include "marker_render.h"
#include "state_producer.h"
#include "stub_game_runtime.h"
#include "testutil.h"
#include "warp_control.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

class DisplayHeightRuntime final : public psx::test::StubGameRuntime {
public:
  int displayHeight() const override {
    return 224;
  }
};

void test_display_height_bounds_the_presented_guest_lines() {
  DisplayHeightRuntime runtime;
  auto game = std::make_unique<Game>(runtime, psx::config::Config{});
  CHECK_EQ(psx::present::DisplayPresenter::presentedHeight(game->core, 240), 224);

  psx::test::StubGameRuntime decoded;
  auto decodedGame = std::make_unique<Game>(decoded, psx::config::Config{});
  CHECK_EQ(psx::present::DisplayPresenter::presentedHeight(decodedGame->core, 240), 240);
}

class InterpolatingRuntime final : public psx::test::StubGameRuntime {
public:
  bool interpolates() const override {
    return true;
  }
};

void test_interpolates_decides_whether_the_fps60_mod_exists() {
  InterpolatingRuntime interpolating;
  auto game = std::make_unique<Game>(interpolating, psx::config::Config{});
  CHECK(game->mods.temporalInterpolationSupported());

  psx::test::StubGameRuntime still;
  auto stillGame = std::make_unique<Game>(still, psx::config::Config{});
  CHECK(!stillGame->mods.temporalInterpolationSupported());
}

class CommandRuntime final : public psx::test::StubGameRuntime {
public:
  bool titleCommand(Core &core, const psx::game::TitleCommandLine &input, std::FILE *reply) override {
    const auto [command, line] = input;
    seenCore = &core;
    if (std::strcmp(command, "warp") != 0) {
      return false;
    }
    std::fprintf(reply, "armed %s\n", line);
    return true;
  }

  Core *seenCore = nullptr;
};

void test_title_command_answers_the_dev_command_control() {
  CommandRuntime runtime;
  auto game = std::make_unique<Game>(runtime, psx::config::Config{});
  const psx::ui::DevCommandControl control(game.get());

  CHECK(control.send("warp 3 2") == "armed warp 3 2");
  CHECK(runtime.seenCore == &game->core);
  CHECK(control.send("items") == "not supported by this game");

  psx::test::StubGameRuntime silent;
  auto silentGame = std::make_unique<Game>(silent, psx::config::Config{});
  CHECK(psx::ui::DevCommandControl(silentGame.get()).send("warp 1 0") == "not supported by this game");
}

class Areas final : public psx::game::TitleDevWarp {
public:
  int areaCount() const override {
    return 2;
  }
  const char *areaName(int area) const override {
    return area == 1 ? "Village" : nullptr;
  }
  bool warpAllowed() const override {
    return allowed;
  }

  bool allowed = false;
};

class WarpRuntime final : public psx::test::StubGameRuntime {
public:
  psx::game::TitleDevWarp *devWarp(Core &) const override {
    return &areas;
  }

  mutable Areas areas;
};

void test_dev_warp_feeds_the_debug_tab_warp() {
  WarpRuntime runtime;
  auto game = std::make_unique<Game>(runtime, psx::config::Config{});
  const psx::ui::DevCommandControl commands(game.get());
  psx::ui::WarpControl warp(game.get(), &commands);

  CHECK_EQ(warp.areaCount(), 2);
  CHECK(warp.areaLabel(1) == "1 - Village");
  CHECK(warp.areaLabel(0) == "Area 0");
  CHECK(warp.arm() == "not in the field - reach gameplay first");

  psx::test::StubGameRuntime none;
  auto noneGame = std::make_unique<Game>(none, psx::config::Config{});
  psx::ui::WarpControl absent(noneGame.get(), &commands);
  CHECK_EQ(absent.areaCount(), 0);
  CHECK(absent.arm() == "warp unavailable");
}

class ProducerSession final : public psx::game::TitleSessionState {
public:
  static constexpr std::uint32_t kProducer = 0x80012340u;
  psx::present::StateProducerTable table = marker::renders(kProducer);
};

class ProducerRuntime final : public psx::test::StubGameRuntime {
public:
  std::unique_ptr<psx::game::TitleSessionState> createSession(Game &) override {
    return std::make_unique<ProducerSession>();
  }
  const psx::present::StateProducer *stateProducer(Core &core, std::uint32_t producer) const override {
    return static_cast<const ProducerSession &>(*core.game->sessionState).table.find(producer);
  }
};

// A Record present composes the title's render of a saved producer state over the sealed record.
void test_state_producers_compose_the_record_present() {
  constexpr psx::present::RecordKey kKey{ProducerSession::kProducer, 0x80150000u, 0, 0};
  const psx::present::FrameRecord shown = marker::keyedRecord(1, kKey, 0);
  const psx::present::FrameState state = marker::savedState(shown, kKey, 20.0f);

  ProducerRuntime runtime;
  auto game = std::make_unique<Game>(runtime, psx::config::Config{});
  const std::optional<psx::present::FrameRecord> composed =
      psx::present::composeFrame(shown, nullptr, state, 1.0f, psx::frame::RuntimeStateProducers(game->core));
  CHECK(composed.has_value());
  if (composed) {
    CHECK_EQ(std::get<psx::present::DrawPrimitive>(composed->entries()[0]).vertices[0].x, 20);
  }

  psx::test::StubGameRuntime none;
  auto noneGame = std::make_unique<Game>(none, psx::config::Config{});
  CHECK(!psx::present::composeFrame(shown, nullptr, state, 1.0f, psx::frame::RuntimeStateProducers(noneGame->core)));
}

class Music final : public psx::game::TitleAudio {
public:
  void mixFrame(std::int16_t *pcm, int frames) override {
    ++calls;
    heard.assign(pcm, pcm + (static_cast<std::size_t>(frames) * 2u));
  }

  int calls = 0;
  std::vector<std::int16_t> heard;
};

class MusicRuntime final : public psx::test::StubGameRuntime {
public:
  psx::game::TitleAudio *audio(Core &) const override {
    return &music;
  }

  mutable Music music;
};

// The title's music mixes into each output field the SPU rendered, and never into a logic-only field.
void test_title_audio_mixes_each_output_field() {
  MusicRuntime runtime;
  auto game = std::make_unique<Game>(runtime, psx::config::Config{});
  game->spu_audio.init();
  game->spu_audio.frameLogic();
  CHECK_EQ(runtime.music.calls, 0);
  game->spu_audio.frame();
  CHECK_EQ(runtime.music.calls, 1);
  CHECK(runtime.music.heard == game->spu_audio.fieldReports().back().pcmSamples);
}

// A title with no music engine still gets its SPU output field.
void test_no_title_audio_still_renders_the_field() {
  psx::test::StubGameRuntime runtime;
  auto game = std::make_unique<Game>(runtime, psx::config::Config{});
  game->spu_audio.init();
  game->spu_audio.frame();
  CHECK(!game->spu_audio.fieldReports().empty());
}

// A crt0 title starts through Crt0Boot: the plan's registers land on the Core and libcInit is called.
class Crt0Runtime final : public psx::test::StubGameRuntime {
public:
  void startProgram(Core &core) override {
    psx::game::Crt0Boot(image).run(core);
  }

  psx::game::Crt0Image image{
      .bss = {0x80030000u, 0x80030040u},
      .stackTopWordAddress = 0x80020000u,
      .stackReserveWordAddress = 0x80020004u,
      .heapBase = 0x80040000u,
      .globalPointer = 0x80038000u,
      .libcInitEntry = 0x80010000u,
      .stackBias = {true, -8},
  };
};

void test_start_program_runs_the_titles_crt0() {
  Crt0Runtime runtime;
  auto game = std::make_unique<Game>(runtime, psx::config::Config{});
  Core &core = game->core;
  core.mem_w32(0x80010000u, 0x03E00008u); // libcInit: jr ra
  core.mem_w32(0x80010004u, 0u);
  (void)core.imageCatalog().activate("crt0-test", {0x00010000u, 0x00010008u}, 1u);
  core.mem_w32(0x80020000u, 0x00200000u);
  core.mem_w32(0x80020004u, 0x00008000u);
  core.mem_w32(0x80030000u, 0xDEADBEEFu);
  core.r[31] = 0xDEAD0000u;

  runtime.startProgram(core);

  CHECK_EQ(core.mem_r32(0x80030000u), 0u);
  CHECK_EQ(core.r[29], 0x801FFFF8u);
  CHECK_EQ(core.r[28], 0x80038000u);
}

// Each Core's GTE is its own: a write to one never shows on the other, whichever was bound last.
void test_each_core_reads_and_runs_its_own_gte() {
  psx::test::StubGameRuntime runtime;
  auto first = std::make_unique<Game>(runtime, psx::config::Config{});
  auto second = std::make_unique<Game>(runtime, psx::config::Config{});
  psx::cpu::Gte &a = first->core.gte();
  psx::cpu::Gte &b = second->core.gte();

  a.writeControl(26, 256u);
  b.writeControl(26, 512u);
  CHECK_EQ(a.readControl(26), 256u);
  CHECK_EQ(b.readControl(26), 512u);

  // NCLIP over (0,0) (10,0) (0,10): MAC0 = 100, on the first Core only.
  a.writeData(12, 0u);
  a.writeData(13, 10u);
  a.writeData(14, 10u << 16);
  b.writeData(24, 7u);
  a.execute(0x4A000006u);
  CHECK_EQ(a.readData(24), 100u);
  CHECK_EQ(b.readData(24), 7u);
}

} // namespace

int main() {
  RUN(display_height_bounds_the_presented_guest_lines);
  RUN(interpolates_decides_whether_the_fps60_mod_exists);
  RUN(title_command_answers_the_dev_command_control);
  RUN(dev_warp_feeds_the_debug_tab_warp);
  RUN(state_producers_compose_the_record_present);
  RUN(title_audio_mixes_each_output_field);
  RUN(no_title_audio_still_renders_the_field);
  RUN(start_program_runs_the_titles_crt0);
  RUN(each_core_reads_and_runs_its_own_gte);
  return psx::test::summary();
}
