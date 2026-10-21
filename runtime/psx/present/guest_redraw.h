// guest_redraw.h — a guest draw routine run again inside its own logic frame for an in-between: the title
// writes the in-between's inputs into guest memory, the original routine draws them with guest time, interrupts
// and devices held, the title reads the packets it linked, and every byte written since is put back.
//
// The routine is the guest's own code, so its projection is the GTE's and its topology the guest's; the
// title decides which inputs move (camera, object positions and angles) and pairs each object by its guest
// address. Nothing the run does outlives the GuestRedraw: memory, registers and GTE are as they were.
// The run cannot suspend at a turn's end, so an override's original call inside it gets the run's budget.
#pragma once

#include "core.h"
#include "gte_control.h"
#include "side_effect_journal.h"

#include <array>
#include <cstdint>
#include <span>

namespace psx::present {

class GuestRedraw {
public:
  explicit GuestRedraw(Core &core);
  ~GuestRedraw();
  GuestRedraw(const GuestRedraw &) = delete;
  GuestRedraw &operator=(const GuestRedraw &) = delete;
  GuestRedraw(GuestRedraw &&) = delete;
  GuestRedraw &operator=(GuestRedraw &&) = delete;

  // `word` at `address` through the guest's memory, put back with the rest.
  void writeWord(std::uint32_t address, std::uint32_t word);

  // The original routine at `entry` (its override suppressed) with `arguments` in a0..a3. False, logged, when
  // it did not return or reached a device or a syscall: its packets then do not describe the in-between.
  [[nodiscard]] bool run(std::uint32_t entry, std::span<const std::uint32_t> arguments);

private:
  Core &core_;
  GteGuard gte_;
  std::array<std::uint32_t, 32> gpr_{};
  std::uint32_t hi_ = 0;
  std::uint32_t lo_ = 0;
  std::uint32_t pc_ = 0;
  std::array<std::uint32_t, 16> cop0_{};
  cpu::SideEffectJournal journal_{cpu::SideEffectMode::Discard, {}};
  cpu::SideEffectJournal::Scope journalScope_;
};

} // namespace psx::present
