#include "override_differential_contract.h"

#include <lucent/log.h>

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

namespace psx::cpu {
namespace {

inline constexpr std::array<const char *, 32> kRegisterNames = {
    "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
    "s0",   "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "s8", "ra"};
inline constexpr std::uint32_t kPhysicalMask = 0x1FFFFFFFu;
inline constexpr std::uint32_t kRamMirrorSpan = 0x800000u;
inline constexpr std::uint32_t kShownBytes = 16;

std::string hexWord(std::uint32_t value) {
  return lucent::format("0x{:08X}", value);
}

// The physical window [begin, end) of callee-frame residue below the entry stack pointer.
ByteRange deadStackWindow(std::uint32_t entrySp, std::uint32_t deadStackBytes) {
  const std::uint32_t physical = entrySp & kPhysicalMask;
  std::uint32_t base = 0;
  std::uint32_t top = 0;
  if (physical < kRamMirrorSpan) {
    top = physical & (kMainRamBytes - 1u);
    base = 0;
  } else if (physical >= kScratchpadPhysicalBase && physical <= kScratchpadPhysicalBase + kScratchpadBytes) {
    top = physical;
    base = kScratchpadPhysicalBase;
  } else {
    return {};
  }
  const std::uint32_t begin = top - std::min(deadStackBytes, top - base);
  return {begin, top};
}

const std::uint8_t *bytesAt(const MachineSnapshot &state, std::uint32_t physical) {
  if (physical >= kScratchpadPhysicalBase) {
    return state.scratchpad.data() + (physical - kScratchpadPhysicalBase);
  }
  return state.ram.data() + physical;
}

std::string hexBytes(const MachineSnapshot &state, ByteRange range) {
  const std::uint32_t shown = std::min(range.end - range.begin, kShownBytes);
  const std::uint8_t *bytes = bytesAt(state, range.begin);
  std::string text;
  for (std::uint32_t index = 0; index < shown; ++index) {
    text += lucent::format("{:02X}", bytes[index]);
  }
  if (shown < range.end - range.begin) {
    text += lucent::format("... ({} of {} bytes shown)", shown, range.end - range.begin);
  }
  return text;
}

// A GTE data register's ARCHITECTURAL value -- what MFC2 returns and what every GTE command reads --
// from its storage word. The two writers of the one register file store the 16-bit registers
// differently: Lightrec's MTC2 keeps the raw 32-bit word (`lightrec_mtc2`), while the GTE's own
// register port (`GTE_WriteDR`, reached by native overrides through `gte_write_data`) sign-extends
// VZ0..2 and IR0..3 on the way in. Both read back only the low halfword, signed for VZn/IRn and
// unsigned for OTZ/SZn (`lightrec_mfc2`, Beetle's `Vectors(...)`), so the upper half of those words is
// not machine state, and two paths that differ only there have done the same thing. Measured on Spyro
// 2's sector walk, where retail moves a zero-extended `lhu` into VZ0 and an exact native override
// differed from it in that register alone.
std::uint32_t architecturalGteWord(std::size_t index, std::uint32_t word) {
  switch (index) {
  case 1:
  case 3:
  case 5:
  case 8:
  case 9:
  case 10:
  case 11:
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(word & 0xFFFFu)));
  case 7:
  case 16:
  case 17:
  case 18:
  case 19:
    return word & 0xFFFFu;
  default:
    return word;
  }
}

void noteFirst(DifferentialOutcome &outcome, DifferentialDifference difference) {
  if (!outcome.difference) {
    outcome.difference = std::move(difference);
  }
}

void compareRegisters(const CallObservation &original, const CallObservation &native, DifferentialOutcome &outcome) {
  if (original.result.guestPc != native.result.guestPc) {
    ++outcome.registersDiffering;
    noteFirst(outcome, {"continuation pc", hexWord(original.result.guestPc), hexWord(native.result.guestPc)});
  }
  for (const std::uint32_t index : kO32PreservedOrResultRegisters) {
    const std::uint32_t a = original.state.gpr[index];
    const std::uint32_t b = native.state.gpr[index];
    if (a != b) {
      ++outcome.registersDiffering;
      noteFirst(outcome, {lucent::format("register {}", kRegisterNames[index]), hexWord(a), hexWord(b)});
    }
  }
  if (original.state.cop0[kCop0Status] != native.state.cop0[kCop0Status]) {
    ++outcome.registersDiffering;
    noteFirst(outcome,
              {"cop0 status", hexWord(original.state.cop0[kCop0Status]), hexWord(native.state.cop0[kCop0Status])});
  }
  for (std::size_t index = 0; index < kGteRegisterCount; ++index) {
    const std::uint32_t a = architecturalGteWord(index, original.state.gte[index]);
    const std::uint32_t b = architecturalGteWord(index, native.state.gte[index]);
    if (a != b) {
      ++outcome.registersDiffering;
      noteFirst(
          outcome,
          {lucent::format("gte {} register {}", index < 32 ? "data" : "control", index % 32), hexWord(a), hexWord(b)});
    }
  }
}

void judgeRange(const CallObservation &original,
                const CallObservation &native,
                ByteRange range,
                DifferentialOutcome &outcome) {
  if (range.end <= range.begin) {
    return;
  }
  ++outcome.memoryRangesDiffering;
  outcome.memoryBytesDiffering += range.end - range.begin;
  noteFirst(outcome,
            {lucent::format("{} [0x{:08X},0x{:08X})",
                            range.begin >= kScratchpadPhysicalBase ? "scratchpad" : "ram",
                            range.begin,
                            range.end),
             hexBytes(original.state, range),
             hexBytes(native.state, range)});
}

void compareMemory(const CallObservation &original,
                   const CallObservation &native,
                   ByteRange deadStack,
                   DifferentialOutcome &outcome) {
  for (const ByteRange range : differingRanges(original.state, native.state)) {
    const std::uint32_t overlapBegin = std::max(range.begin, deadStack.begin);
    const std::uint32_t overlapEnd = std::min(range.end, deadStack.end);
    if (overlapBegin >= overlapEnd) {
      judgeRange(original, native, range, outcome);
      continue;
    }
    outcome.deadStackBytesIgnored += overlapEnd - overlapBegin;
    judgeRange(original, native, {range.begin, overlapBegin}, outcome);
    judgeRange(original, native, {overlapEnd, range.end}, outcome);
  }
}

void compareSideEffects(const CallObservation &original, const CallObservation &native, DifferentialOutcome &outcome) {
  const std::size_t common = std::min(original.effects.size(), native.effects.size());
  for (std::size_t index = 0; index < common; ++index) {
    if (!(original.effects[index] == native.effects[index])) {
      noteFirst(outcome,
                {lucent::format("side effect #{}", index),
                 describeSideEffect(original.effects[index]),
                 describeSideEffect(native.effects[index])});
      return;
    }
  }
  if (original.effects.size() != native.effects.size()) {
    const auto describeAt = [common](std::span<const SideEffect> effects) {
      return common < effects.size() ? describeSideEffect(effects[common]) : std::string("(none: log ended)");
    };
    noteFirst(
        outcome,
        {lucent::format(
             "side effect #{} (logs of {} and {} effects)", common, original.effects.size(), native.effects.size()),
         describeAt(original.effects),
         describeAt(native.effects)});
  }
}

} // namespace

const char *mipsRegisterName(std::uint32_t index) {
  return index < kRegisterNames.size() ? kRegisterNames[index] : "?";
}

DifferentialOutcome judgeCompletedCall(const CallObservation &original,
                                       const CallObservation &native,
                                       std::uint32_t entrySp,
                                       std::uint32_t deadStackBytes) {
  DifferentialOutcome outcome;
  outcome.sideEffectsOriginal = original.effects.size();
  outcome.sideEffectsNative = native.effects.size();
  compareRegisters(original, native, outcome);
  compareMemory(original, native, deadStackWindow(entrySp, deadStackBytes), outcome);
  compareSideEffects(original, native, outcome);
  outcome.verdict = outcome.difference ? DifferentialVerdict::Mismatch : DifferentialVerdict::Match;
  return outcome;
}

} // namespace psx::cpu
