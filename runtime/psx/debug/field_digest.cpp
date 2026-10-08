#include "field_digest.h"

#include "game.h"

#include <cstring>
#include <lucent/log.h>

namespace psx::diag {

std::uint64_t FieldDigest::hashWords(std::span<const std::uint8_t> bytes) {
  constexpr std::uint64_t kOffsetBasis = 1469598103934665603ull;
  constexpr std::uint64_t kPrime = 1099511628211ull;
  std::uint64_t hash = kOffsetBasis;
  for (std::size_t offset = 0; offset + sizeof(std::uint64_t) <= bytes.size(); offset += sizeof(std::uint64_t)) {
    std::uint64_t word = 0;
    std::memcpy(&word, bytes.data() + offset, sizeof word);
    hash = (hash ^ word) * kPrime;
  }
  return hash;
}

void FieldDigest::recordField(const Game &game) {
  const std::uint64_t field = mFields++;
  if (!lucent::channel_on("fielddigest")) {
    return;
  }
  lucent::info("fielddigest",
               "field={} ticks={} ram={:016x} i_stat={:x} i_mask={:x} pad={:04x}",
               field,
               game.timing.emulatedCpuTicks(),
               hashWords(std::span<const std::uint8_t>(game.core.ram, sizeof game.core.ram)),
               game.hle.i_stat,
               game.hle.i_mask,
               game.pad.buttons);
}

} // namespace psx::diag
