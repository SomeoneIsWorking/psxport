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

class Source : public InBetweenStrategy {
public:
  bool eligible(const Core &) const override {
    return ready;
  }
  bool owns(const RqItem &primitive) const override {
    if (primitive.layer != RQ_WORLD || !primitive.has_xyf) {
      return false;
    }
    // With twoOwned the source claims a SECOND producer too, so a test can place an un-owned item
    // between two reconstructed ones and prove it stays there.
    return primitive.dbg_node == sourceTag || (twoOwned && primitive.dbg_node == sourceTag + 1);
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
    // TWO owned items when the caller asks for them, so a test can place an un-owned item
    // BETWEEN two reconstructed ones. `seq` here is 3 as the reconstruction's own queue numbers
    // it from zero, which is the whole point: it must not be compared against the live frame's.
    const int produced = twoOwned ? 2 : 1;
    for (int i = 0; i < produced; ++i) {
      RqItem *output = core.game->rqRedirect->push();
      CHECK(output != nullptr);
      *output = item(RQ_WORLD, 3 + i, sourceTag + i, true);
      output->xsf[0] = previous + (current - previous) * t;
    }
  }
  void rotate(Core &core) override {
    ++rotations;
    rotatedCore = &core;
    previous = current;
    current = 0;
  }

  bool ready = true;
  bool twoOwned = false;
  bool throwOnReconstruct = false;
  float previous = 10;
  float current = 30;
  int rotations = 0;
  Core *rotatedCore = nullptr;
  std::vector<float> parameters;
};

// A source that claims a named basis for presenting on a path that keeps the guest's renderer live,
// and otherwise says nothing. Everything else about it is identical, so the claim is the only thing
// the admission rule below can be reading.
class ClaimingSource : public Source {
public:
  explicit ClaimingSource(InBetweenStrategy::GuestPathClaim claim) : claim_(claim) {}
  InBetweenStrategy::GuestPathClaim guestPathClaim() const override {
    return claim_;
  }

private:
  InBetweenStrategy::GuestPathClaim claim_;
};

class Backend final : public psx::frame::FramePresentationBackend {
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

// THE ORDERING THE SPLICE EXISTS TO KEEP. Two owned items with an UN-OWNED one between them, in
// one layer: the un-owned item must stay between them. A merge on (layer, seq) cannot do this —
// the reconstruction's queue numbers its items from zero while this queue runs to hundreds — so it
// sorted both reconstructed items ahead of the captured one and moved a translucent item across a
// neighbour it is composited against. Measured on Spyro 1 as identical draw calls producing
// different pixels in the effect's own region.
void test_unowned_items_keep_their_captured_position_between_reconstructed_ones() {
  auto game = std::make_unique<Game>();
  game->mods.fps60 = true;
  auto ownedSource = std::make_unique<Source>();
  Source &source = *ownedSource;
  Fps60 presentation(*game, std::move(ownedSource));
  source.twoOwned = true;
  source.ready = true;
  // WORLD: owned(seq 10), un-owned(seq 11), owned(seq 12). The seq values are deliberately far
  // from zero so a reconstruction numbered from zero cannot coincide with them by accident.
  const std::array<RqItem, 3> captured = {
      item(RQ_WORLD, 10, sourceTag, true),
      item(RQ_WORLD, 11, 0xBEEF, true),
      item(RQ_WORLD, 12, sourceTag + 1, true),
  };
  presentation.presentPass(&game->core, 0.5f, {captured, 1});
  CHECK_EQ(presentation.mPresentStream.size(), captured.size());
  if (presentation.mPresentStream.size() != captured.size()) {
    return;
  }
  // The un-owned item is pointer-identical to the captured one: same item, not a copy.
  CHECK(presentation.mPresentStream[1] == &captured[1]);
  // And it is still BETWEEN the two reconstructed ones, which are NOT the captured owned items.
  CHECK(presentation.mPresentStream[0] != &captured[0]);
  CHECK(presentation.mPresentStream[2] != &captured[2]);
}

// THE ORDERING THE SPLICE HAD TO LEARN. A source's reconstruction and the captured queue it replaces
// are the SAME producers' output in a DIFFERENT ORDER, with a DIFFERENT COUNT per producer, and a
// splice that pairs them by stream position puts one producer's geometry in another producer's run.
// Measured on Spyro 1's picker, on one aborting frame: captured owned slots A x196 B x167 C x195
// D x692 E x404 against reconstruction C x197 D x681 A x199 B x167 E x401. That is what aborted the
// product four times - refused=8 UnsortedQueue (the overlay's layer-3 quad landed in a layer-1 world
// slot), refused=12 DuplicateReplayKey, refused=2 NonWorld - and this is the frame shape behind all
// three.
//
// So each captured slot must take ITS OWN producer's next reconstruction primitive, whatever order
// the two streams run in and however many of them there are.
constexpr uint32_t kProducerA = 0x8001F798u;
constexpr uint32_t kProducerB = 0x800258F0u;
constexpr uint32_t kStrangerTag = 0xBEEFu;

class ReorderedSource : public InBetweenStrategy {
public:
  bool eligible(const Core &) const override {
    return true;
  }
  bool owns(const RqItem &primitive) const override {
    return primitive.layer == RQ_WORLD && primitive.has_xyf &&
           (primitive.painter_object == kProducerA || primitive.painter_object == kProducerB);
  }
  void reconstruct(Core &core, float) override {
    // B FIRST and one primitive long, then A: the reverse of the captured queue's order, with the
    // counts the picker measured (one producer short, one producer over).
    for (int i = 0; i < 3; ++i) {
      RqItem *output = core.game->rqRedirect->push();
      CHECK(output != nullptr);
      *output = item(RQ_WORLD, (uint32_t)i, kProducerB, true);
      output->painter_object = kProducerB;
      output->xsf[0] = 100.0f + (float)i;
    }
    for (int i = 0; i < 2; ++i) {
      RqItem *output = core.game->rqRedirect->push();
      CHECK(output != nullptr);
      *output = item(RQ_WORLD, (uint32_t)(3 + i), kProducerA, true);
      output->painter_object = kProducerA;
      output->xsf[0] = 200.0f + (float)i;
    }
  }
  void rotate(Core &) override {}
};

void test_reconstruction_is_paired_per_producer_not_per_stream_position() {
  auto game = std::make_unique<Game>();
  game->mods.fps60 = true;
  auto ownedSource = std::make_unique<ReorderedSource>();
  Fps60 presentation(*game, std::move(ownedSource));
  // Captured order: A, a stranger, B, a stranger, A, B, B. Sorted by (layer, draw_seq) as a real
  // captured queue is, and the draw_seq values are far from the reconstruction's own numbering.
  std::array<RqItem, 7> captured = {
      item(RQ_WORLD, 10, 0, true),
      item(RQ_WORLD, 11, kStrangerTag, true),
      item(RQ_WORLD, 12, 0, true),
      item(RQ_WORLD, 13, kStrangerTag, true),
      item(RQ_WORLD, 14, 0, true),
      item(RQ_WORLD, 15, 0, true),
      item(RQ_WORLD, 16, 0, true),
  };
  captured[0].painter_object = kProducerA;
  captured[2].painter_object = kProducerB;
  captured[4].painter_object = kProducerA;
  captured[5].painter_object = kProducerB;
  captured[6].painter_object = kProducerB;
  presentation.presentPass(&game->core, 0.5f, {captured, 1});

  CHECK_EQ(presentation.mPresentStream.size(), captured.size());
  if (presentation.mPresentStream.size() != captured.size()) {
    return;
  }
  // Each slot received its OWN producer's next primitive, identified by the geometry that source
  // actually produced. Pairing by stream position hands slot 0 B's first primitive instead.
  const std::array<uint32_t, 5> slotProducer{kProducerA, kProducerB, kProducerA, kProducerB, kProducerB};
  const std::array<size_t, 5> slots{0, 2, 4, 5, 6};
  const std::array<float, 5> slotGeometry{200.0f, 100.0f, 201.0f, 101.0f, 102.0f};
  for (size_t k = 0; k < slots.size(); ++k) {
    const RqItem *emitted = presentation.mPresentStream[slots[k]];
    CHECK(emitted != &captured[slots[k]]);
    CHECK_EQ(emitted->painter_object, slotProducer[k]);
    CHECK_EQ(emitted->xsf[0], slotGeometry[k]);
    // All three coordinates of the slot it fills, not of the reconstruction's own queue.
    CHECK_EQ(emitted->layer, captured[slots[k]].layer);
    CHECK_EQ(emitted->draw_seq, captured[slots[k]].draw_seq);
    CHECK_EQ(emitted->flush_ordinal, captured[slots[k]].flush_ordinal);
  }
  // Every un-owned captured item is emitted by pointer at its own index: nothing moved.
  for (size_t i = 0; i < captured.size(); ++i) {
    if (i == 1 || i == 3) {
      CHECK(presentation.mPresentStream[i] == &captured[i]);
    }
  }
  // The stream the painter planner is handed is sorted by (layer, draw_seq) inside one flush epoch,
  // which is what it refuses a run without.
  for (size_t i = 1; i < presentation.mPresentStream.size(); ++i) {
    const RqItem &previous = *presentation.mPresentStream[i - 1];
    const RqItem &current = *presentation.mPresentStream[i];
    CHECK(previous.layer <= current.layer);
    if (previous.layer == current.layer) {
      CHECK(previous.draw_seq <= current.draw_seq);
    }
    CHECK_EQ(current.flush_ordinal, presentation.mPresentStream.front()->flush_ordinal);
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
  for (const auto path : {RenderPath::Gte, RenderPath::Device}) {
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
// WHO MAY PRESENT AN IN-BETWEEN ON A PATH THAT KEEPS THE GUEST'S RENDERER LIVE.
//
// The claim replaced a narrower question — "are your in-betweens made of the guest's own primitives?"
// — with a named basis, because a host rebuild of the guest's own picture out of a read-only reading
// of its memory is admitted for the same reason and used to be refused. Three claims and the paths
// they differ on, one case each.
void test_guest_path_claim_is_what_admits_an_in_between_on_the_guest_path() {
  using Claim = InBetweenStrategy::GuestPathClaim;

  // Each pair is (path, permitted), so the claim is the only thing that differs between the two
  // halves of a row and the path is the only thing that differs between the rows.
  struct Case {
    Claim claim;
    RenderPath path;
    bool permitted;
    const char *what;
  };
  const Case cases[] = {
      // Nothing claimed: the in-between needs the guest's renderer live, so it is Native-only, and
      // the user's broad decision is what admits it there.
      {Claim::None, RenderPath::Gte, false, "a source that claims nothing"},
      // The guest's own captured primitives (Tomba! 2's GuestGeometrySceneSource): unchanged, still
      // admitted on Gte.
      {Claim::GuestPrimitives, RenderPath::Gte, true, "the guest's own primitives"},
      // A host rebuild of the guest's own picture out of read-only guest memory: admitted on Gte for
      // the same reason, and this is the case that used to be refused.
      {Claim::HostRebuiltFromGuestMemory, RenderPath::Gte, true, "a host rebuild of guest memory"},
      // Device is the untouched reference and has no second field to present into, so no claim
      // reaches it.
      {Claim::GuestPrimitives, RenderPath::Device, false, "the guest's primitives on Device"},
      {Claim::HostRebuiltFromGuestMemory, RenderPath::Device, false, "a host rebuild on Device"},
  };

  for (const Case &one : cases) {
    auto game = std::make_unique<Game>();
    auto source = std::make_unique<ClaimingSource>(one.claim);
    Fps60 presentation(*game, std::unique_ptr<InBetweenStrategy>(std::move(source)));
    game->core.rsub.mode.setPath(one.path);
    // `interpolationPermitted` is private to the shipping product, so it is reached the way the
    // product reaches it: through `active()`, which is also the question a player is answered.
    game->mods.fps60 = true;
    CHECK_EQ(presentation.active(), one.permitted);
    // OFF is off whatever is claimed: the claim is an admission, not a request.
    game->mods.fps60 = false;
    CHECK(!presentation.active());
    (void)one.what;
  }

  // The default IS "nothing claimed": a strategy that says nothing is Native-only, which is the safe
  // answer for a title that has not thought about it.
  // Not a static_assert: a type with virtual functions is not a literal type, so the default is
  // checked on an instance, which is where it is actually consulted.
  struct Bare final : InBetweenStrategy {
    bool eligible(const Core &) const override {
      return true;
    }
    bool owns(const RqItem &) const override {
      return false;
    }
    void reconstruct(Core &, float) override {}
    void rotate(Core &) override {}
  };
  const Bare bare;
  CHECK_EQ(static_cast<int>(bare.guestPathClaim()), static_cast<int>(Claim::None));
}

} // namespace

int main() {
  RUN(unowned_items_keep_their_captured_position_between_reconstructed_ones);
  RUN(reconstruction_is_paired_per_producer_not_per_stream_position);
  RUN(mixed_producers_survive_every_reconstructed_slot);
  RUN(guest_path_claim_is_what_admits_an_in_between_on_the_guest_path);
  RUN(disabled_ineligible_and_reference_frames_replay_capture);
  RUN(rotation_is_per_instance_and_once_per_real_fence_even_disabled);
  RUN(no_source_replays_without_synthesizing_a_second_present);
  RUN(failed_reconstruction_restores_display_state);
  RUN(legacy_factory_preserves_uncaptured_endpoint_geometry_and_rotation);
  return pt_summary();
}
