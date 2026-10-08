// Executable-write invalidations are counted per source, and a burst store that the caller notifies
// once costs one invalidation, not one per word.
#include "game.h"
#include "invalidation.h"
#include "lightrec_executor.h"
#include "testutil.h"

#include <memory>

namespace {

constexpr uint32_t kRam = 0x80110000u;
constexpr std::size_t kMapped = static_cast<std::size_t>(psx::cpu::ExecutableWriteSource::MappedStore);
constexpr std::size_t kDma = static_cast<std::size_t>(psx::cpu::ExecutableWriteSource::Dma);

} // namespace

static void test_a_guest_store_is_counted_against_mapped_store() {
  auto game = std::make_unique<Game>();
  const auto before = game->core.lightrecExecutor().counters();
  game->core.mem_w32(kRam, 1u);
  const auto after = game->core.lightrecExecutor().counters();
  CHECK_EQ(after.invalidations - before.invalidations, 1u);
  CHECK_EQ(after.invalidationsBySource[kMapped] - before.invalidationsBySource[kMapped], 1u);
  CHECK_EQ(after.invalidationsBySource[kDma], before.invalidationsBySource[kDma]);
}

static void test_a_labelled_store_is_counted_against_its_source() {
  auto game = std::make_unique<Game>();
  const auto before = game->core.lightrecExecutor().counters();
  game->core.mem_w32(kRam, 1u, psx::cpu::ExecutableWriteSource::Cpu);
  const auto after = game->core.lightrecExecutor().counters();
  const auto cpu = static_cast<std::size_t>(psx::cpu::ExecutableWriteSource::Cpu);
  CHECK_EQ(after.invalidationsBySource[cpu] - before.invalidationsBySource[cpu], 1u);
  CHECK_EQ(after.invalidationsBySource[kMapped], before.invalidationsBySource[kMapped]);
}

static void test_an_unnotified_burst_costs_one_dma_invalidation() {
  auto game = std::make_unique<Game>();
  const auto before = game->core.lightrecExecutor().counters();
  for (uint32_t i = 0; i < 64; i++) {
    game->core.mem_w32_unnotified(kRam + i * 4u, 0xA5A50000u + i);
  }
  CHECK_EQ(game->core.lightrecExecutor().counters().invalidations, before.invalidations);
  psx::cpu::notifyExecutableWrite(game->core, {kRam, kRam + 64u * 4u}, psx::cpu::ExecutableWriteSource::Dma);
  const auto after = game->core.lightrecExecutor().counters();
  CHECK_EQ(after.invalidations - before.invalidations, 1u);
  CHECK_EQ(after.invalidationsBySource[kDma] - before.invalidationsBySource[kDma], 1u);
  CHECK_EQ(game->core.mem_r32(kRam + 63u * 4u), 0xA5A50000u + 63u);
}

int main() {
  RUN(a_guest_store_is_counted_against_mapped_store);
  RUN(a_labelled_store_is_counted_against_its_source);
  RUN(an_unnotified_burst_costs_one_dma_invalidation);
  return pt_summary();
}
