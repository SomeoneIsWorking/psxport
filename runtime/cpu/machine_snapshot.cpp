#include "machine_snapshot.h"

#include "core.h"
#include "game.h"
#include "invalidation.h"

#include <algorithm>
#include <cstring>
#include <span>

namespace psx::cpu {
namespace {

inline constexpr std::size_t kSkipChunk = 64;

// Calls `visit(offset, length)` for every maximal run in which `a` and `b` differ.
template <typename Visit>
void forEachDifferingRun(std::span<const std::uint8_t> a, std::span<const std::uint8_t> b, Visit visit) {
  const std::size_t size = std::min(a.size(), b.size());
  std::size_t index = 0;
  while (index < size) {
    // Equal memory is the overwhelmingly common case, so whole chunks are skipped before bytes are.
    if (index + kSkipChunk <= size && std::memcmp(a.data() + index, b.data() + index, kSkipChunk) == 0) {
      index += kSkipChunk;
      continue;
    }
    if (a[index] == b[index]) {
      ++index;
      continue;
    }
    const std::size_t begin = index;
    while (index < size && a[index] != b[index]) {
      ++index;
    }
    visit(begin, index - begin);
  }
}

void restoreRegion(Core &core,
                   std::span<std::uint8_t> live,
                   std::span<const std::uint8_t> saved,
                   std::uint32_t physicalBase,
                   RestoreReport &report) {
  forEachDifferingRun(live, saved, [&](std::size_t offset, std::size_t length) {
    std::memcpy(live.data() + offset, saved.data() + offset, length);
    const auto begin = physicalBase + static_cast<std::uint32_t>(offset);
    notifyExecutableWrite(core, {begin, begin + static_cast<std::uint32_t>(length)}, ExecutableWriteSource::Savestate);
    ++report.rangesWritten;
    report.bytesWritten += length;
  });
}

} // namespace

MachineSnapshot MachineSnapshot::capture(const Core &core) {
  MachineSnapshot snapshot;
  std::copy_n(core.r, snapshot.gpr.size(), snapshot.gpr.begin());
  snapshot.hi = core.hi;
  snapshot.lo = core.lo;
  snapshot.pc = core.pc;
  std::copy_n(core.cop0, snapshot.cop0.size(), snapshot.cop0.begin());
  if (core.game != nullptr) {
    std::copy_n(core.game->gte.REG, snapshot.gte.size(), snapshot.gte.begin());
    snapshot.gteFlags = core.game->gte.FLAGS;
  }
  snapshot.ram.assign(core.ram, core.ram + kMainRamBytes);
  snapshot.scratchpad.assign(core.scratch, core.scratch + kScratchpadBytes);
  return snapshot;
}

RestoreReport MachineSnapshot::restoreInto(Core &core) const {
  std::copy(gpr.begin(), gpr.end(), core.r);
  core.r[0] = 0;
  core.hi = hi;
  core.lo = lo;
  core.pc = pc;
  std::copy(cop0.begin(), cop0.end(), core.cop0);
  if (core.game != nullptr) {
    std::copy(gte.begin(), gte.end(), core.game->gte.REG);
    core.game->gte.FLAGS = gteFlags;
  }
  RestoreReport report;
  restoreRegion(core, {core.ram, kMainRamBytes}, ram, 0, report);
  restoreRegion(core, {core.scratch, kScratchpadBytes}, scratchpad, kScratchpadPhysicalBase, report);
  return report;
}

std::vector<ByteRange> differingRanges(const MachineSnapshot &a, const MachineSnapshot &b) {
  std::vector<ByteRange> ranges;
  const auto collect =
      [&](std::span<const std::uint8_t> left, std::span<const std::uint8_t> right, std::uint32_t base) {
        forEachDifferingRun(left, right, [&](std::size_t offset, std::size_t length) {
          const auto begin = base + static_cast<std::uint32_t>(offset);
          ranges.push_back({begin, begin + static_cast<std::uint32_t>(length)});
        });
      };
  collect(a.ram, b.ram, 0);
  collect(a.scratchpad, b.scratchpad, kScratchpadPhysicalBase);
  return ranges;
}

} // namespace psx::cpu
