// guest_redraw.h — a guest draw routine run again inside its own logic frame for an in-between: the title
// writes the in-between's inputs into guest memory, the original routine draws them with guest time, interrupts
// and devices held, the title reads the packets it linked, and every byte written since is put back.
//
// The routine is the guest's own code, so its projection is the GTE's and its topology the guest's; the
// title decides which inputs move (camera, object positions and angles) and pairs each object by its guest
// address. Nothing the run does outlives the GuestRedraw but the stores it keeps: memory, registers, GTE and the writer
// latch are as they were. One opened inside another's run nests, and the outer run still puts back what the inner kept.
// The run cannot suspend at a turn's end, so an override's original call inside it gets the run's budget.
#pragma once

#include "core.h"
#include "frame_record.h"
#include "gte_control.h"
#include "projected_vertex.h"
#include "side_effect_journal.h"

#include <array>
#include <cstdint>
#include <functional>
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

  // The run's stores wholly inside the `bytes` at `address`, and the vertex places noted there, outlive it (unless an
  // enclosing run puts them back); declared before the stores.
  void keep(std::uint32_t address, std::uint32_t bytes);
  // The run did not draw what it was for: every kept store goes back too.
  void keepNothing();

  // `word` at `address` through the guest's memory, put back with the rest.
  void writeWord(std::uint32_t address, std::uint32_t word);
  // A byte or an aligned halfword at `address`; the rest of its word is kept.
  void writeByte(std::uint32_t address, std::uint8_t byte);
  void writeHalf(std::uint32_t address, std::uint16_t half);
  // A packet vertex at `address` with its sub-pixel place (EmitMemory::storeVertex).
  void writeVertex(std::uint32_t address, const gte::ProjectedVertex &vertex);
  // The source, destination and size words after the GP0(80) command word at `command`, each keeping the part of its
  // fields past their whole pixels that `copy.place` gives (decodeCopy).
  void writeCopy(std::uint32_t command, const VramCopy &copy);

  // The original routine at `entry` (its override suppressed) with `arguments` in a0..a3. False, logged, when
  // it did not return or reached a device or a syscall (a host body stops at it): its packets then do not describe
  // the in-between.
  [[nodiscard]] bool run(std::uint32_t entry, std::span<const std::uint32_t> arguments);
  // The same as a guest call to `entry`: its override, if any, runs. A draw section's steps are called this way.
  [[nodiscard]] bool dispatch(std::uint32_t entry, std::span<const std::uint32_t> arguments);
  // Calls to `callees`, host services (native overrides, HLE) whose work belongs to the real frame, return to
  // their caller at once wherever the run reaches them, their registers as the call found them: only for call
  // sites that read nothing back (PlaySound from a draw that overwrites v0). Aborts at a callee that is guest code.
  void applied(std::span<const std::uint32_t> callees);

  // A stretch of a dispatchTo body the run does not execute: the run stops on reaching `at`, `host` (when set) does
  // the stretch's work, and the run resumes at `resume`. With `resume` 0, `at` is a callee's entry whose device work
  // the real frame already did (a draw's leading PutDrawEnv): the run resumes at the return address the call left,
  // so the callee does not run and its delay slot did.
  struct Skip {
    std::uint32_t at = 0;
    std::uint32_t resume = 0;
    std::function<void()> host;
  };

  // The routine at `entry` (no override of its own) from its start until execution reaches `end`, a block start of
  // its body where its device tail begins; the run then continues from the section's sp and ra. Each of `skips`,
  // in the order the body reaches them, is stepped over once.
  [[nodiscard]] bool dispatchTo(std::uint32_t entry,
                                std::uint32_t end,
                                std::span<const std::uint32_t> arguments,
                                std::span<const Skip> skips = {});

private:
  enum class Call : std::uint8_t { Original, Dispatched, ToEnd };
  // A ToEnd call resumes `entry`'s body at `from` (its start when 0).
  [[nodiscard]] bool call(std::uint32_t entry,
                          std::span<const std::uint32_t> arguments,
                          Call kind,
                          std::uint32_t end = 0,
                          std::uint32_t from = 0);
  // Whether the run reached no device or syscall; the first it reached is logged and reported as a refusal.
  [[nodiscard]] bool reachedNoDevice(std::uint32_t entry);

  Core &core_;
  GteGuard gte_;
  std::array<std::uint32_t, 32> gpr_{};
  std::uint32_t hi_ = 0;
  std::uint32_t lo_ = 0;
  std::uint32_t pc_ = 0;
  std::array<std::uint32_t, 16> cop0_{};
  // The call stack the run's stores are attributed to, as the frame left it.
  cpu::GuestWriterLatch writerLatch_;
  cpu::SideEffectJournal journal_{cpu::SideEffectMode::Discard, {}};
  cpu::SideEffectJournal::Scope journalScope_;
};

} // namespace psx::present
