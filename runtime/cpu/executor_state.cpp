#include "executor_state.h"

#include "core.h"
#include "game.h"
#include "gte_register_transfer.h"
#include "guest_memory_map.h"
#include "r3000_decode.h"
#include "side_effect_journal.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace psx::cpu {
namespace {

// Only the hardware-register window reaches io_read/io_write, the one place a guest observes elapsed time.
bool isDeviceAddress(std::uint32_t address) {
  const std::uint32_t phys = physical(address);
  return phys >= kHardwareBase && phys < kHardwareBase + kHardwareBytes;
}

} // namespace

// ---- Registers ---------------------------------------------------------------------------------

void ExecutorState::setGpr(std::uint32_t reg, std::uint32_t value) {
  if (reg == 0) {
    return;
  }
  core.r[reg] = value;
  if (arriving.reg == reg) {
    arriving.reg = 0;
  }
}

void ExecutorState::delayLoad(std::uint32_t reg, std::uint32_t value) {
  if (reg == 0) {
    return;
  }
  if (arriving.reg == reg) {
    arriving.reg = 0;
  }
  pending = {reg, value};
}

std::uint32_t ExecutorState::mergeBase(std::uint32_t reg) const {
  return arriving.reg == reg && reg != 0 ? arriving.value : core.r[reg];
}

void ExecutorState::settleLoads() {
  if (arriving.reg != 0) {
    core.r[arriving.reg] = arriving.value;
    arriving.reg = 0;
  }
  if (pending.reg != 0) {
    core.r[pending.reg] = pending.value;
    pending.reg = 0;
  }
}

void ExecutorState::writeCop0(std::uint32_t reg, std::uint32_t value) {
  switch (reg) {
  case 1:
  case 4:
  case 8:
  case 14:
  case 15:
    return; // read-only
  default:
    break;
  }
  if (reg >= 16) {
    return;
  }
  const std::uint32_t oldStatus = core.cop0[kCop0Status];
  if (reg == kCop0Cause) {
    core.cop0[kCop0Cause] = (core.cop0[kCop0Cause] & ~0x300u) | (value & 0x300u);
  } else {
    core.cop0[reg] = value;
  }
  if (reg != kCop0Status && reg != kCop0Cause) {
    return;
  }
  const std::uint32_t status = core.cop0[kCop0Status];
  const std::uint32_t cause = core.cop0[kCop0Cause];
  // A pending software interrupt now enabled, or hardware interrupts just enabled: deliver at the block end.
  if ((status & cause & 0x300u) != 0 && (status & 1u) != 0) {
    checkInterrupt = true;
  }
  if (reg == kCop0Status && (~status & 0x401u) == 0 && (~oldStatus & 0x401u) != 0) {
    checkInterrupt = true;
  }
}

void ExecutorState::returnFromException() {
  const std::uint32_t status = core.cop0[kCop0Status];
  writeCop0(kCop0Status, ((status & 0x3Cu) >> 2) | (status & ~0xFu));
}

// ---- GTE ---------------------------------------------------------------------------------------

std::uint32_t ExecutorState::readGteData(std::uint32_t reg) {
  if (reg >= gte::kSxy0 && reg <= gte::kSxyP) {
    return core.gte().projected(std::min(reg, gte::kSxy2) - gte::kSxy0).word();
  }
  return core.gte().readData(reg);
}

// The place of `value` read from GTE data register `reg`, when that is an SXY entry still holding it.
std::optional<NotedPlace> ExecutorState::screenPlace(std::uint32_t reg, std::uint32_t value) {
  if (reg < gte::kSxy0 || reg > gte::kSxyP) {
    return std::nullopt;
  }
  const gte::ProjectedVertex vertex = core.gte().projected(std::min(reg, gte::kSxy2) - gte::kSxy0);
  if (vertex.word() != value) {
    return std::nullopt;
  }
  return NotedPlace{.place = vertex.place(), .site = core.writerLatch.function()};
}

void ExecutorState::cop2Transfer(std::uint32_t instruction, std::uint32_t value) {
  VertexPlaces &places = core.vertexPlaces;
  const std::uint32_t reg = rtField(instruction);
  const std::uint32_t move = rsField(instruction);
  // Every TRZ write replaces the carried translation depth, tracked or not.
  if (move == kCop2Ctc2 && rdField(instruction) == gte::kTranslationZ) {
    const std::optional<NotedPlace> noted = places.tracking() ? places.carried(reg, value) : std::nullopt;
    core.gte().carryTranslation(noted ? noted->translation : GteTranslationDepth{});
    places.endFace();
    return;
  }
  if (!places.tracking()) {
    return;
  }
  if (move == kCop2Mfc2) {
    if (const std::optional<NotedPlace> noted = screenPlace(rdField(instruction), value)) {
      places.carry(reg, value, *noted);
      return;
    }
  }
  if (move == kCop2Mfc2 || move == kCop2Cfc2) {
    places.overwrote(reg);
  }
}

// ---- Memory ------------------------------------------------------------------------------------

// A store's vertex place: a SWC2 of an SXY entry takes the GTE's, a SW the one its register carries.
void ExecutorState::storedPlace(std::uint32_t instruction, std::uint32_t address, std::uint32_t value) {
  VertexPlaces &places = core.vertexPlaces;
  if (!places.tracking()) {
    return;
  }
  const std::uint32_t reg = rtField(instruction);
  if (primaryOpcode(instruction) == kOpSw) {
    places.stored(reg, address, value);
    return;
  }
  if (primaryOpcode(instruction) != kOpSwc2) {
    return;
  }
  if (const std::optional<NotedPlace> noted = screenPlace(reg, value)) {
    places.note(address, value, *noted);
  }
}

// A load into a GPR: a whole word carries the place stored with it, anything else ends what the GPR carried.
void ExecutorState::loadedPlace(std::uint32_t instruction, std::uint32_t address, std::uint32_t value) {
  VertexPlaces &places = core.vertexPlaces;
  if (!places.tracking()) {
    return;
  }
  const std::uint32_t op = primaryOpcode(instruction);
  if (op == kOpLw) {
    if (places.mayLoadFace(instruction)) {
      places.faceLoaded({.function = core.writerLatch.function(), .opcode = instruction, .word = value});
    }
    places.loaded(rtField(instruction), address, value);
  } else if (op >= kOpLb && op <= kOpLwr) {
    places.overwrote(rtField(instruction));
  }
}

// It never re-enters a memory access, and leaves the host turn and spin sampling to the segment end.
void ExecutorState::beforeAccess(std::uint32_t address) {
  ++counters.memoryAccesses;
  if (core.game == nullptr || !isDeviceAddress(address)) {
    return;
  }
  // The override differential's shadow path holds guest time; its device reads are replayed.
  if (core.sideEffectJournal != nullptr && core.sideEffectJournal->withholdsGuestTime()) {
    return;
  }
  const std::uint32_t instructions = segmentClock.commitThrough(executed);
  if (instructions == 0) {
    return;
  }
  ++counters.deviceClockCommits;
  counters.deviceClockCommitInstructions += instructions;
  core.game->timing.advanceGuestInstructionTicks(instructions);
}

std::uint32_t ExecutorState::load(std::uint32_t instruction, std::uint32_t address) {
  beforeAccess(address);
  std::uint32_t value = 0;
  switch (primaryOpcode(instruction)) {
  case kLb:
  case kLbu:
    value = core.mem_r8(address);
    break;
  case kLh:
  case kLhu:
    value = core.mem_r16(address);
    break;
  default:
    value = core.mem_r32(address);
    break;
  }
  loadedPlace(instruction, address, value);
  return value;
}

std::uint32_t ExecutorState::loadValue(std::uint32_t instruction, std::uint32_t address) {
  const std::uint32_t value = load(instruction, address);
  switch (primaryOpcode(instruction)) {
  case kLb:
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int8_t>(value)));
  case kLh:
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(value)));
  default:
    return value;
  }
}

std::uint32_t ExecutorState::loadUnaligned(std::uint32_t instruction, std::uint32_t address, std::uint32_t base) {
  const std::uint32_t word = load(instruction, address & ~3u);
  const std::uint32_t shift = (address & 3u) * 8u;
  return primaryOpcode(instruction) == kLwl ? (base & (0x00FFFFFFu >> shift)) | (word << (24u - shift))
                                            : (base & (0xFFFFFF00u << (24u - shift))) | (word >> shift);
}

void ExecutorState::store(std::uint32_t instruction, std::uint32_t address, std::uint32_t value, std::uint32_t width) {
  beforeAccess(address);
  switch (width) {
  case 1:
    core.mem_w8(address, static_cast<std::uint8_t>(value), ExecutableWriteSource::Cpu);
    break;
  case 2:
    core.mem_w16(address, static_cast<std::uint16_t>(value), ExecutableWriteSource::Cpu);
    break;
  default:
    core.mem_w32(address, value, ExecutableWriteSource::Cpu);
    storedPlace(instruction, address, value);
    break;
  }
}

void ExecutorState::storeInstruction(std::uint32_t instruction,
                                     std::uint32_t pc,
                                     std::uint32_t address,
                                     std::uint32_t segmentInstructions) {
  StoreObserverTargetCounts *const observed = storeReport.armed ? observedStore(pc) : nullptr;
  if (observed != nullptr) {
    observeStore(*observed, pc, instruction, StoreObservationPhase::Before, segmentInstructions);
  }
  const std::uint32_t rt = rtField(instruction);
  const std::uint32_t *const r = core.r;
  switch (primaryOpcode(instruction)) {
  case kSb:
    store(instruction, address, r[rt], 1);
    break;
  case kSh:
    store(instruction, address, r[rt], 2);
    break;
  case kSw:
    store(instruction, address, r[rt], 4);
    break;
  case kSwc2:
    store(instruction, address, readGteData(rt), 4);
    break;
  default: {
    const std::uint32_t aligned = address & ~3u;
    beforeAccess(aligned);
    const std::uint32_t word = core.mem_r32(aligned);
    const std::uint32_t shift = (address & 3u) * 8u;
    const std::uint32_t merged = primaryOpcode(instruction) == kSwl
                                     ? (word & (0xFFFFFF00u << shift)) | (r[rt] >> (24u - shift))
                                     : (word & (0x00FFFFFFu >> (24u - shift))) | (r[rt] << shift);
    store(instruction, aligned, merged, 4);
    break;
  }
  }
  if (observed != nullptr) {
    observeStore(*observed, pc, instruction, StoreObservationPhase::After, segmentInstructions);
  }
}

bool ExecutorState::fetch(std::uint32_t pc, std::uint32_t &instruction) const {
  const std::uint32_t phys = physical(pc) & ~3u;
  if (phys < kRamMirrorSpan) {
    std::memcpy(&instruction, core.ram + (phys & (kMainRamBytes - 1u)), 4);
    return true;
  }
  if (phys >= kScratchpadPhysicalBase && phys < kScratchpadPhysicalBase + kScratchpadBytes) {
    std::memcpy(&instruction, core.scratch + (phys - kScratchpadPhysicalBase), 4);
    return true;
  }
  return false;
}

// ---- Store observer ----------------------------------------------------------------------------

StoreObserverTargetCounts *ExecutorState::observedStore(std::uint32_t pc) {
  for (std::size_t i = 0; i < storeReport.targetCount; ++i) {
    if (storeReport.targets[i].guestPc == pc) {
      return &storeReport.targets[i];
    }
  }
  return nullptr;
}

void ExecutorState::observeStore(StoreObserverTargetCounts &target,
                                 std::uint32_t pc,
                                 std::uint32_t instruction,
                                 StoreObservationPhase phase,
                                 std::uint32_t segmentInstructions) {
  std::array<std::uint32_t, 34> gpr{};
  std::copy_n(core.r, 32, gpr.begin());
  gpr[32] = core.lo;
  gpr[33] = core.hi;
  std::array<std::uint32_t, 32> cp0{};
  std::copy_n(core.cop0, 16, cp0.begin());
  std::array<std::uint32_t, 32> cp2Data{};
  std::array<std::uint32_t, 32> cp2Control{};
  exportGteRegisters(core.gte().registers(), cp2Data, cp2Control);
  ResolvedStoreTarget resolved;
  resolved.baseRegister = rsField(instruction);
  resolved.sourceRegister = rtField(instruction);
  resolved.displacement = static_cast<std::int32_t>(signedImmediate(instruction));
  resolved.address = gpr[resolved.baseRegister] + static_cast<std::uint32_t>(resolved.displacement);
  resolved.value = gpr[resolved.sourceRegister];
  resolved.valid = true;
  if (phase == StoreObservationPhase::Before) {
    ++target.before;
    if (target.lastTarget.valid && target.lastTarget.address != resolved.address) {
      ++target.distinctTargets;
    } else if (!target.lastTarget.valid) {
      target.firstTarget = resolved;
    }
    target.previousTarget = target.lastTarget;
    target.lastTarget = resolved;
  } else {
    ++target.after;
  }
  const StoreObservation observation{
      pc, phase, segmentInstructions, instruction, resolved, gpr, cp0, cp2Data, cp2Control};
  ++storeReport.observations;
  storeCallback(observation, storeContext);
}

} // namespace psx::cpu
