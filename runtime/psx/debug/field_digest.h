// field_digest.h — one line per display field that names the guest-visible machine state, so two runs
// of the same binary on the same inputs can be compared field by field and the FIRST field at which
// they differ named.
//
// The channel is off unless `PSXPORT_DEBUG=fielddigest` (lucent's channel switch); an unobserved run
// pays one boolean test per field. Each line is `field=<n> ticks=<emulated cpu ticks> ram=<hash of
// the 2 MB main RAM> i_stat=<..> i_mask=<..> pad=<active-low buttons>`, with nothing host-derived in
// it: no wall-clock time, no thread timing, no addresses. Two runs that are a pure function of the
// guest and its inputs print identical lines (tools/determinism_check.py compares them).
#pragma once

#include <cstdint>
#include <span>

class Game;

namespace psx::diag {

class FieldDigest {
public:
  // FNV-1a over 64-bit words. `bytes` must be a multiple of eight long (main RAM is 2 MB).
  [[nodiscard]] static std::uint64_t hashWords(std::span<const std::uint8_t> bytes);

  // Log the state `game` is in at the end of one display field, when the channel is on. The field
  // number counts the calls made on this instance.
  void recordField(const Game &game);

private:
  std::uint64_t mFields = 0;
};

} // namespace psx::diag
