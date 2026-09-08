// Exercise Fps60's shipping merge and frame fence without a window or guest program.
#include "fps60.h"
#include "game.h"
#include "game_iface.h"
#include "testutil.h"

#include <array>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
constexpr uint32_t sourceTag = 0x1234;

RqItem item(int layer, uint32_t sequence, uint32_t producer, bool projected = false) {
  RqItem result{};
  result.layer = layer;
  result.seq = sequence;
  result.draw_seq = sequence;
  result.sort_key = -1;
  result.dbg_node = producer;
  result.has_xyf = projected;
  result.nv = 3;
  return result;
}

std::array<RqItem, 6> capturedItems() {
  return {item(RQ_BACKGROUND, 1, kBackdropDbgNode),
          item(RQ_BACKGROUND, 2, 0),
          item(RQ_WORLD, 3, sourceTag, true),
          item(RQ_WORLD, 4, 0x5678, true),
          item(RQ_WORLD, 5, 0x6789),
          item(RQ_HUD, 6, 0)};
}

class Source final : public TemporalSceneSource {
public:
  bool eligible(const Core &) const override {
    return ready;
  }
  bool owns(const RqItem &primitive) const override {
    return primitive.layer == RQ_WORLD && primitive.has_xyf && primitive.dbg_node == sourceTag;
  }
  void reconstruct(Core &core, float t) override {
    parameters.push_back(t);
    CHECK(core.rsub.mode.displayPassArmed());
    CHECK(core.game->rqRedirect != nullptr);
    CHECK(core.game->rqRedirect != &core.game->rq);
    core.rsub.projParams.setProjH(999);
    if (throwOnReconstruct) {
      throw std::runtime_error("synthetic producer refusal");
    }
    RqItem *output = core.game->rqRedirect->push();
    CHECK(output != nullptr);
    *output = item(RQ_WORLD, 3, sourceTag, true);
    output->xsf[0] = previous + (current - previous) * t;
  }
  void rotate(Core &core) override {
    ++rotations;
    rotatedCore = &core;
    previous = current;
    current = 0;
  }

  bool ready = true;
  bool throwOnReconstruct = false;
  float previous = 10;
  float current = 30;
  int rotations = 0;
  Core *rotatedCore = nullptr;
  std::vector<float> parameters;
};

class Backend final : public FramePresentationBackend {
public:
  void emit(std::span<const RqItem>) override {}
  void presentReal() override {
    ++realPresents;
  }
  void captureDiagnostic(uint64_t, bool interpolated) override {
    CHECK(!interpolated);
  }
  void pace(int guestFields, int parts) override {
    CHECK_EQ(guestFields, 2);
    CHECK_EQ(parts, 1);
  }
  void reconcile(uint64_t) override {}
  void beginLedgerFrame() override {}
  int realPresents = 0;
};

void checkVerbatim(const Fps60 &presentation, std::span<const RqItem> captured) {
  CHECK_EQ(presentation.mPresentStream.size(), captured.size());
  if (presentation.mPresentStream.size() != captured.size()) {
    return;
  }
  for (size_t i = 0; i < captured.size(); ++i) {
    // Pointer identity proves all captured fields survive, including unexamined material data.
    CHECK(presentation.mPresentStream[i] == &captured[i]);
  }
}

void test_mixed_producers_survive_every_reconstructed_slot() {
  auto game = std::make_unique<Game>();
  game->mods.fps60 = true;
  auto ownedSource = std::make_unique<Source>();
  Source &source = *ownedSource;
  Fps60 presentation(*game, std::move(ownedSource));
  const auto captured = capturedItems();
  game->core.rsub.projParams.setProjH(123);
  // An existing redirect must be restored, not overwritten with nullptr.
  game->rqRedirect = &game->rq;
  for (const float t : {0.0f, 0.5f, 1.0f}) {
    presentation.presentPass(&game->core, t, {captured, 1});
    CHECK_EQ(presentation.mPresentStream.size(), captured.size());
    if (presentation.mPresentStream.size() != captured.size()) {
      continue;
    }
    for (size_t i = 0; i < captured.size(); ++i) {
      if (i == 2) {
        CHECK(presentation.mPresentStream[i] != &captured[i]);
        CHECK_EQ(presentation.mPresentStream[i]->xsf[0], 10.0f + 20.0f * t);
      } else {
        CHECK(presentation.mPresentStream[i] == &captured[i]);
      }
    }
    CHECK(game->rqRedirect == &game->rq);
    CHECK_EQ(game->core.rsub.projParams.projH(), 123);
    CHECK(!game->core.rsub.mode.displayPassArmed());
    CHECK_EQ(game->rq.n, 0);
  }
  CHECK(source.parameters == std::vector<float>({0.0f, 0.5f, 1.0f}));
  CHECK_EQ(source.rotations, 0);
}

void test_disabled_ineligible_and_reference_frames_replay_capture() {
  auto game = std::make_unique<Game>();
  auto ownedSource = std::make_unique<Source>();
  Source &source = *ownedSource;
  Fps60 presentation(*game, std::move(ownedSource));
  const auto captured = capturedItems();
  game->mods.fps60 = true;
  presentation.presentPass(&game->core, 1, {captured, 0});
  CHECK_EQ(source.parameters.size(), 1u);
  CHECK(presentation.mSink != nullptr);
  CHECK_EQ(presentation.mSink->n, 1);
  source.parameters.clear();
  // The prior reconstruction remains allocated; none of the following paths may merge it.
  game->mods.fps60 = false;
  for (const float t : {0.5f, 1.0f}) {
    presentation.presentPass(&game->core, t, {captured, 1});
    checkVerbatim(presentation, captured);
  }
  game->mods.fps60 = true;
  source.ready = false;
  presentation.presentPass(&game->core, 1, {captured, 2});
  checkVerbatim(presentation, captured);
  source.ready = true;
  for (const auto path : {RenderPath::Gte, RenderPath::Psx}) {
    game->core.rsub.mode.setPath(path);
    presentation.presentPass(&game->core, 1, {captured, 3});
    checkVerbatim(presentation, captured);
  }
  CHECK(source.parameters.empty());
}

void test_rotation_is_per_instance_and_once_per_real_fence_even_disabled() {
  auto game = std::make_unique<Game>();
  auto ownedSource = std::make_unique<Source>();
  Source &source = *ownedSource;
  Fps60 presentation(*game, std::move(ownedSource));
  auto otherGame = std::make_unique<Game>();
  auto otherSource = std::make_unique<Source>();
  Source &other = *otherSource;
  Fps60 otherPresentation(*otherGame, std::move(otherSource));
  Backend backend;
  game->mods.fps60 = true;
  presentation.present(backend, game->core, {}, 2); // first frame has no intermediate slot
  CHECK_EQ(source.parameters.size(), 1u);
  CHECK_EQ(source.parameters.front(), 1.0f);
  CHECK_EQ(source.rotations, 1);
  CHECK_EQ(source.previous, 30.0f);
  CHECK(source.rotatedCore == &game->core);
  CHECK_EQ(other.rotations, 0);
  game->mods.fps60 = false;
  presentation.present(backend, game->core, {}, 2);
  CHECK_EQ(source.parameters.size(), 1u);
  CHECK_EQ(source.rotations, 2);
  CHECK_EQ(source.previous, 0.0f); // a missing current endpoint cannot survive a frame fence
  CHECK_EQ(backend.realPresents, 2);
  CHECK_EQ(other.rotations, 0);
}

void test_no_source_replays_without_synthesizing_a_second_present() {
  auto game = std::make_unique<Game>();
  game->mods.fps60 = true;
  Fps60 presentation(*game);
  presentation.mHavePrev = 1;
  presentation.mTier1EligibleCur = true; // a legacy latch cannot give a direct runtime a producer
  const auto captured = capturedItems();
  Backend backend;
  presentation.present(backend, game->core, {captured, 1}, 2);
  checkVerbatim(presentation, captured);
  CHECK_EQ(backend.realPresents, 1);
}

void test_failed_reconstruction_restores_display_state() {
  auto game = std::make_unique<Game>();
  game->mods.fps60 = true;
  auto source = std::make_unique<Source>();
  source->throwOnReconstruct = true;
  Fps60 presentation(*game, std::move(source));
  game->core.rsub.projParams.setProjH(123);
  bool refused = false;
  try {
    presentation.presentPass(&game->core, 1, {});
  } catch (const std::runtime_error &) {
    refused = true;
  }
  CHECK(refused);
  CHECK(game->rqRedirect == nullptr);
  CHECK_EQ(game->core.rsub.projParams.projH(), 123);
  CHECK(!game->core.rsub.mode.displayPassArmed());
}

int hookPresents = 0;
int hookRotations = 0;
void legacyWorld(Core *core, float t) {
  ++hookPresents;
  CHECK_EQ(t, 1.0f);
  CHECK(fps60(*core->game).mCamOverrideOn);
  CHECK_EQ(fps60(*core->game).mCamOverride.T[0], 42.0f);
  *core->game->rqRedirect->push() = item(RQ_WORLD, 3, sourceTag, true);
}
void legacyRotate(Core *) {
  ++hookRotations;
}

void test_legacy_factory_preserves_uncaptured_endpoint_geometry_and_rotation() {
  GameConfig config{};
  GameHooks hooks{};
  hooks.fps60WorldPass = legacyWorld;
  hooks.fps60TemporalRotate = legacyRotate;
  LegacyGameRuntimeAdapter runtime(config, hooks);
  auto game = std::make_unique<Game>();
  game->core.hooks = &hooks;
  game->temporalPresentation = runtime.createTemporalFramePresentation(*game);
  Fps60 &presentation = fps60(*game);
  presentation.mTier1EligibleCur = true;
  presentation.mCamCur.T[0] = 42;
  game->mods.fps60 = false;
  hookPresents = hookRotations = 0;
  const std::array captured{
      item(RQ_BACKGROUND, 1, 0), item(RQ_WORLD, 2, 0), item(RQ_WORLD, 3, sourceTag, true), item(RQ_HUD, 4, 0)};
  Backend backend;
  presentation.present(backend, game->core, {captured, 1}, 2);
  CHECK_EQ(hookPresents, 1);
  CHECK_EQ(hookRotations, 1);
  CHECK_EQ(presentation.mCamPrev.T[0], 42.0f);
  CHECK(!presentation.mCamOverrideOn);
  CHECK_EQ(presentation.mPresentStream.size(), captured.size());
  CHECK(presentation.mPresentStream[0] == &captured[0]);
  CHECK(presentation.mPresentStream[1] == &captured[1]);
  CHECK(presentation.mPresentStream[2] != &captured[2]);
  CHECK(presentation.mPresentStream[3] == &captured[3]);
  presentation.presentPass(&game->core, 0.5f, {captured, 1});
  checkVerbatim(presentation, captured);
  CHECK_EQ(hookPresents, 1); // disabled interpolation never invokes an intermediate callback
}
} // namespace

int main() {
  RUN(mixed_producers_survive_every_reconstructed_slot);
  RUN(disabled_ineligible_and_reference_frames_replay_capture);
  RUN(rotation_is_per_instance_and_once_per_real_fence_even_disabled);
  RUN(no_source_replays_without_synthesizing_a_second_present);
  RUN(failed_reconstruction_restores_display_state);
  RUN(legacy_factory_preserves_uncaptured_endpoint_geometry_and_rotation);
  return pt_summary();
}
