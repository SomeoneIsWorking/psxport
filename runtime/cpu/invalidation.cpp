#include "invalidation.h"

#include "core.h"
#include "lightrec_executor.h"

#include <algorithm>

namespace psx::cpu {
namespace {

constexpr std::uint64_t kPhysicalAddressSpaceSize = 0x20000000ull;

void invalidatePhysicalSegment(Core &core, std::uint32_t begin, std::uint32_t length) {
  if (length == 0) {
    return;
  }
  const std::uint32_t alignedBegin = begin & ~3u;
  const std::uint64_t end = static_cast<std::uint64_t>(begin) + length;
  const auto alignedEnd = static_cast<std::uint32_t>((end + 3u) & ~std::uint64_t{3u});
  core.lightrecExecutor().invalidate({alignedBegin, alignedEnd});
}

} // namespace

void notifyExecutableWrite(Core &core, GuestAddressRange range, ExecutableWriteSource) {
  if (range.end <= range.begin) {
    return;
  }
  const std::uint64_t length = static_cast<std::uint64_t>(range.end) - range.begin;
  if (length >= kPhysicalAddressSpaceSize) {
    core.lightrecExecutor().invalidateAll();
    return;
  }

  const std::uint32_t begin = range.begin & 0x1fffffffu;
  const std::uint64_t firstLength = std::min(length, kPhysicalAddressSpaceSize - begin);
  invalidatePhysicalSegment(core, begin, static_cast<std::uint32_t>(firstLength));
  const std::uint64_t remaining = length - firstLength;
  if (remaining != 0) {
    invalidatePhysicalSegment(core, 0, static_cast<std::uint32_t>(remaining));
  }
}

void notifyExecutableStateReplaced(Core &core, ExecutableWriteSource) {
  core.lightrecExecutor().invalidateAll();
}

} // namespace psx::cpu
