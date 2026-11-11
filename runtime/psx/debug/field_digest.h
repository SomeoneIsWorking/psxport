// field_digest.h — one line per advance of the display-field clock that names the guest-visible machine
// state, so two runs of the same binary on the same inputs can be compared and the FIRST point at which
// they differ named.
//
// The channel is off unless `PSXPORT_DEBUG=fielddigest` (lucent's channel switch); an unobserved run
// pays one boolean test per advance. Each line is `field=<n-th advance> ticks=<emulated cpu ticks>
// boundary=<field boundary ticks> ram=<hash of the 2 MB main RAM> i_stat=<..> i_mask=<..> pad=<active-low
// buttons>`, with nothing host-derived in it. A present cadence in parts (fps60 under the default
// `GameRuntime::presentDeadline`) advances once per part, so runs of different cadences are compared by
// `ticks`, not by line; inputs sent over the control channel must land on the same frame (pause and step).
#pragma once

#include <cstdint>
#include <span>

class Game;

namespace psx::diag {

class FieldDigest {
public:
  // FNV-1a over 64-bit words. `bytes` must be a multiple of eight long (main RAM is 2 MB).
  [[nodiscard]] static std::uint64_t hashWords(std::span<const std::uint8_t> bytes);

  // Log the state `game` is in after one advance of the display-field clock, when the channel is on.
  void recordField(const Game &game);

private:
  std::uint64_t mFields = 0;
};

} // namespace psx::diag
