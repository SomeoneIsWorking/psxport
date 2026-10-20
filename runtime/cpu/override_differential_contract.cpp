#include "override_differential_contract.h"

#include "gte_registers.h"

#include <lucent/log.h>

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

namespace psx::cpu {
namespace {

inline constexpr std::size_t kGteWords = sizeof(GteRawState::reg) / sizeof(std::uint32_t);
inline constexpr std::uint32_t kShownBytes = 16;

std::string hexWord(std::uint32_t value) {
  return lucent::format("0x{:08X}", value);
}

// The physical window [begin, end) of callee-frame residue below the entry stack pointer.
ByteRange deadStackWindow(DeadStack deadStack) {
  const std::uint32_t address = physical(deadStack.entrySp);
  std::uint32_t base = 0;
  std::uint32_t top = 0;
  if (address < kRamMirrorSpan) {
    top = *mainRamOffset(address);
  } else if (address >= kScratchpadPhysicalBase && address <= kScratchpadPhysicalBase + kScratchpadBytes) {
    top = address;
    base = kScratchpadPhysicalBase;
  } else {
    return {};
  }
  const std::uint32_t begin = top - std::min(deadStack.bytes, top - base);
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

// MFC2 reads VZn/IRn as a signed and OTZ/SZn as an unsigned halfword, so the upper half is not state.
std::array<std::uint32_t, kGteWords> architecturalGte(const GteRawState &gte) {
  std::array<std::uint32_t, kGteWords> words{};
  std::copy_n(gte.reg, kGteWords, words.begin());
  for (const std::uint32_t index : {gte::kVz0, gte::kVz1, gte::kVz2, gte::kIr0, gte::kIr1, gte::kIr2, gte::kIr3}) {
    words[index] =
        static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(gte.reg[index] & 0xFFFFu)));
  }
  for (const std::uint32_t index : {gte::kOtz, gte::kSz0, gte::kSz1, gte::kSz2, gte::kSz3}) {
    words[index] &= 0xFFFFu;
  }
  return words;
}

void noteFirst(DifferentialOutcome &outcome, DifferentialDifference difference) {
  if (!outcome.difference) {
    outcome.difference = std::move(difference);
  } else if (outcome.further.size() < kListedFurtherDifferences) {
    outcome.further.push_back(std::move(difference));
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
      noteFirst(outcome, {lucent::format("register {}", gprName(index)), hexWord(a), hexWord(b)});
    }
  }
  if (original.state.cop0[kCop0Status] != native.state.cop0[kCop0Status]) {
    ++outcome.registersDiffering;
    noteFirst(outcome,
              {"cop0 status", hexWord(original.state.cop0[kCop0Status]), hexWord(native.state.cop0[kCop0Status])});
  }
  const auto originalGte = architecturalGte(original.state.gte);
  const auto nativeGte = architecturalGte(native.state.gte);
  for (std::size_t index = 0; index < kGteWords; ++index) {
    const std::uint32_t a = originalGte[index];
    const std::uint32_t b = nativeGte[index];
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

DifferentialOutcome
judgeCompletedCall(const CallObservation &original, const CallObservation &native, DeadStack deadStack) {
  DifferentialOutcome outcome;
  outcome.sideEffectsOriginal = original.effects.size();
  outcome.sideEffectsNative = native.effects.size();
  compareRegisters(original, native, outcome);
  compareMemory(original, native, deadStackWindow(deadStack), outcome);
  compareSideEffects(original, native, outcome);
  outcome.verdict = outcome.difference ? DifferentialVerdict::Mismatch : DifferentialVerdict::Match;
  return outcome;
}

} // namespace psx::cpu
