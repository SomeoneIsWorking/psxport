// The host-dispatch verdict cache answers "what is this guest address" for every block boundary. A
// cached verdict is only correct while every input it was derived from is unchanged, so each test here
// changes one input and requires the answer to move with it. The unit tests pin the cache's own
// contract; the dispatcher tests drive the shipping `classifyGuestHostDispatch` path.
#include "execution_control.h"
#include "game.h"
#include "lightrec_executor.h"

#include "dynarec_test_fixture.h"
#include "hle.h"
#include "host_dispatch_cache.h"
#include "legacy_game_config.h"
#include "native_dispatch.h"
#include "testutil.h"

#include <memory>

namespace {

using namespace dynarec_test;
using psx::cpu::DispatchEpoch;
using psx::cpu::GuestHostDispatchKind;
using psx::cpu::HostDispatchVerdictCache;

constexpr std::uint32_t kEntry = 0x80010040u;
constexpr std::uint32_t kPadEnableEntry = 0x8000E884u;

void nativeNoop(Core *) {}

std::uint64_t cacheHits(Core &core) {
  return core.nativeDispatcher().verdicts().stats().hits;
}

std::uint64_t cacheMisses(Core &core) {
  return core.nativeDispatcher().verdicts().stats().misses;
}

std::uint64_t cacheFlushes(Core &core) {
  return core.nativeDispatcher().verdicts().stats().flushes;
}

void test_a_stored_verdict_is_found_and_an_unseen_address_is_not() {
  HostDispatchVerdictCache cache;
  cache.observe({.images = 1});
  CHECK(!cache.find(kEntry).has_value());
  cache.store(kEntry, GuestHostDispatchKind::HostService);
  CHECK(cache.find(kEntry).has_value());
  CHECK(*cache.find(kEntry) == GuestHostDispatchKind::HostService);
  CHECK(!cache.find(kEntry + 4u).has_value());
  CHECK_EQ(cache.stats().hits, 2u);
  CHECK_EQ(cache.stats().misses, 2u);
}

void test_each_epoch_input_discards_every_verdict() {
  const DispatchEpoch base{.images = 3, .overrides = 5, .services = 7, .game = &base};
  const DispatchEpoch changed[] = {
      {.images = 4, .overrides = 5, .services = 7, .game = &base},
      {.images = 3, .overrides = 6, .services = 7, .game = &base},
      {.images = 3, .overrides = 5, .services = 8, .game = &base},
      {.images = 3, .overrides = 5, .services = 7, .game = nullptr},
  };
  for (const DispatchEpoch &next : changed) {
    HostDispatchVerdictCache cache;
    cache.observe(base);
    const std::uint64_t flushesBefore = cache.stats().flushes;
    cache.store(kEntry, GuestHostDispatchKind::Fault);
    cache.observe(base); // the same epoch again must keep it
    CHECK(cache.find(kEntry).has_value());
    CHECK_EQ(cache.stats().flushes, flushesBefore);
    cache.observe(next);
    CHECK(!cache.find(kEntry).has_value());
    CHECK_EQ(cache.stats().flushes, flushesBefore + 1u);
  }
}

void test_a_colliding_address_replaces_rather_than_answers_for_the_other() {
  HostDispatchVerdictCache cache;
  cache.observe({.images = 1});
  // The table is direct-mapped on the word index; these two share a slot.
  constexpr std::uint32_t kOther = kEntry + (1u << 16);
  cache.store(kEntry, GuestHostDispatchKind::HostService);
  cache.store(kOther, GuestHostDispatchKind::Fault);
  CHECK(!cache.find(kEntry).has_value());
  CHECK(*cache.find(kOther) == GuestHostDispatchKind::Fault);
}

void test_classification_is_remembered_and_follows_an_override_install_and_removal() {
  Runtime runtime;
  auto game = makeGame(runtime);
  Core &core = game->core;
  const auto image = core.imageCatalog().activate("cache-test", {0x00010000u, 0x00020000u}, 1u);
  const psx::cpu::NativeKey key{image, kEntry};

  CHECK(psx::cpu::classifyGuestHostDispatch(core, kEntry) == GuestHostDispatchKind::ExecuteGuest);
  const std::uint64_t hitsBefore = cacheHits(core);
  const std::uint64_t missesBefore = cacheMisses(core);
  CHECK(psx::cpu::classifyGuestHostDispatch(core, kEntry) == GuestHostDispatchKind::ExecuteGuest);
  CHECK_EQ(cacheHits(core), hitsBefore + 1u); // the second answer came from the cache
  CHECK_EQ(cacheMisses(core), missesBefore);

  CHECK(core.nativeDispatcher().install({key, "cache-test", nativeNoop}));
  CHECK(psx::cpu::classifyGuestHostDispatch(core, kEntry) == GuestHostDispatchKind::HostService);
  CHECK(core.nativeDispatcher().remove(key));
  CHECK(psx::cpu::classifyGuestHostDispatch(core, kEntry) == GuestHostDispatchKind::ExecuteGuest);
}

void test_classification_follows_the_image_that_owns_the_address() {
  Runtime runtime;
  auto game = makeGame(runtime);
  Core &core = game->core;
  const auto image = core.imageCatalog().activate("cache-test", {0x00010000u, 0x00020000u}, 1u);

  CHECK(psx::cpu::classifyGuestHostDispatch(core, kEntry) == GuestHostDispatchKind::ExecuteGuest);
  CHECK(core.imageCatalog().deactivate(image));
  // A RAM address in no resident image is the typed fault, and the cache must not keep saying "run it".
  CHECK(psx::cpu::classifyGuestHostDispatch(core, kEntry) == GuestHostDispatchKind::Fault);
  const auto replacement = core.imageCatalog().activate("cache-test-2", {0x00010000u, 0x00020000u}, 2u);
  CHECK(psx::cpu::classifyGuestHostDispatch(core, kEntry) == GuestHostDispatchKind::ExecuteGuest);

  // An override keyed to the replaced residency does not claim the address under the new one.
  CHECK(core.nativeDispatcher().install({{image, kEntry}, "stale", nativeNoop}));
  CHECK(psx::cpu::classifyGuestHostDispatch(core, kEntry) == GuestHostDispatchKind::ExecuteGuest);
  CHECK(core.nativeDispatcher().install({{replacement, kEntry}, "current", nativeNoop}));
  CHECK(psx::cpu::classifyGuestHostDispatch(core, kEntry) == GuestHostDispatchKind::HostService);
  CHECK(core.imageCatalog().subtractRange(replacement, {kEntry & 0x1fffffffu, (kEntry & 0x1fffffffu) + 4u}) >= 1u);
  CHECK(psx::cpu::classifyGuestHostDispatch(core, kEntry) == GuestHostDispatchKind::Fault);
}

// While an original is running, its own override is suppressed. A verdict remembered from before the
// suppression began ("host service") would send the original straight back into the override.
int observedDuringSuppression = -1;
int observedAfterSuppression = -1;
psx::cpu::NativeKey suppressedKey{};

void nativeObservesSuppression(Core *core) {
  observedDuringSuppression = static_cast<int>(psx::cpu::classifyGuestHostDispatch(*core, suppressedKey.address));
}

void nativeCallsItsOriginal(Core *core) {
  psx::cpu::callOriginalToReturn(*core, suppressedKey, psx::cpu::ExecutionBudget::fromCycles(1000), "cache-test");
  observedAfterSuppression = static_cast<int>(psx::cpu::classifyGuestHostDispatch(*core, suppressedKey.address));
}

void test_a_suppressed_override_is_classified_as_guest_code_for_exactly_the_suppression() {
  Runtime runtime;
  auto game = makeGame(runtime);
  Core &core = game->core;
  const auto image = core.imageCatalog().activate("cache-test", {0x00010000u, 0x00020000u}, 1u);
  constexpr std::uint32_t kOuter = 0x80010100u;
  constexpr std::uint32_t kInner = 0x80010200u;
  suppressedKey = {image, kOuter};
  // outer: jal inner; nop; jr ra; nop      inner: jr ra; nop  (both are replaced by overrides until suppressed)
  core.mem_w32(kOuter, 0x03e0f021u); // addu s8, ra, zero
  core.mem_w32(kOuter + 4u, encodeJal(kInner));
  core.mem_w32(kOuter + 8u, 0u);
  core.mem_w32(kOuter + 12u, 0x03c00008u); // jr s8
  core.mem_w32(kOuter + 16u, 0u);
  core.mem_w32(kInner, 0x03e00008u); // jr ra
  core.mem_w32(kInner + 4u, 0u);
  CHECK(core.nativeDispatcher().install({{image, kOuter}, "outer", nativeCallsItsOriginal}));
  CHECK(core.nativeDispatcher().install({{image, kInner}, "inner", nativeObservesSuppression}));
  CHECK(psx::cpu::classifyGuestHostDispatch(core, kOuter) == GuestHostDispatchKind::HostService);

  core.r[31] = 0x80010f00u;
  const auto result = psx::cpu::dispatchGuest(core, kOuter, psx::cpu::ExecutionBudget::fromCycles(1000));
  CHECK(result.returned());
  CHECK_EQ(observedDuringSuppression, static_cast<int>(GuestHostDispatchKind::ExecuteGuest));
  CHECK_EQ(observedAfterSuppression, static_cast<int>(GuestHostDispatchKind::HostService));
}

void test_a_platform_service_registration_changes_the_answer() {
  Runtime runtime;
  auto game = makeGame(runtime);
  Core &core = game->core;
  GameConfig config{};
  config.hle.windowLo[0] = kEntry;
  config.hle.windowHi[0] = kEntry + 0x100u;
  core.cfg = &config;
  core.imageCatalog().activate("cache-test", {0x00010000u, 0x00020000u}, 1u);

  CHECK(psx::cpu::classifyGuestHostDispatch(core, kEntry) == GuestHostDispatchKind::ExecuteGuest);
  const std::uint64_t flushesBefore = cacheFlushes(core);
  CHECK(game->platform_hle.register_(kEntry, nativeNoop));
  CHECK(psx::cpu::classifyGuestHostDispatch(core, kEntry) == GuestHostDispatchKind::HostService);
  CHECK(cacheFlushes(core) > flushesBefore);
  core.cfg = nullptr;
}

void test_the_pad_work_area_entries_are_never_remembered() {
  // Their answer reads live guest memory (the B0 table's published base), which no revision counter
  // watches, so a remembered verdict could outlive the memory it was read from.
  Runtime runtime;
  auto game = makeGame(runtime);
  Core &core = game->core;
  CHECK(Hle::isPadWorkAreaEntry(kPadEnableEntry));
  CHECK(!Hle::isPadWorkAreaEntry(kEntry));
  const std::uint64_t hitsBefore = cacheHits(core);
  const std::uint64_t missesBefore = cacheMisses(core);
  psx::cpu::classifyGuestHostDispatch(core, kPadEnableEntry);
  psx::cpu::classifyGuestHostDispatch(core, kPadEnableEntry);
  CHECK_EQ(cacheHits(core), hitsBefore);
  CHECK_EQ(cacheMisses(core), missesBefore);
}

} // namespace

int main() {
  RUN(a_stored_verdict_is_found_and_an_unseen_address_is_not);
  RUN(each_epoch_input_discards_every_verdict);
  RUN(a_colliding_address_replaces_rather_than_answers_for_the_other);
  RUN(classification_is_remembered_and_follows_an_override_install_and_removal);
  RUN(classification_follows_the_image_that_owns_the_address);
  RUN(a_suppressed_override_is_classified_as_guest_code_for_exactly_the_suppression);
  RUN(a_platform_service_registration_changes_the_answer);
  RUN(the_pad_work_area_entries_are_never_remembered);
  return pt_summary();
}
