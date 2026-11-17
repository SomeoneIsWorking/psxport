#include "recompiler.h"

#include "arm64_translator.h"
#include "core.h"
#include "cpu_divide.h"
#include "executor_state.h"
#include "guest_memory_map.h"
#include "r3000_decode.h"

#include <lucent/log.h>

#include <string>

namespace psx::cpu::recompiler {
namespace {

// Translated code for one executor; when full, every block is dropped and translation starts over.
constexpr std::size_t kCodeMemoryBytes = std::size_t{32} << 20;
// Blocks longer than this run on the interpreter.
constexpr std::size_t kMaxBlockInstructions = 1024;

Recompiler &owner(JitContext *context) {
  return *static_cast<Recompiler *>(context->owner);
}

} // namespace

std::unique_ptr<Recompiler> Recompiler::create(ExecutorState &state) {
#if defined(__aarch64__)
  std::unique_ptr<CodeMemory> memory = CodeMemory::create(kCodeMemoryBytes);
  if (memory == nullptr) {
    lucent::warn("recompiler",
                 "the host refused {} MiB of executable memory; guest code is interpreted",
                 kCodeMemoryBytes >> 20);
    return nullptr;
  }
  return std::unique_ptr<Recompiler>(new Recompiler(state, std::move(memory)));
#else
  (void)state;
  return nullptr;
#endif
}

Recompiler::Recompiler(ExecutorState &state, std::unique_ptr<CodeMemory> memory)
    : state_(state), memory_(std::move(memory)) {
  Core &core = state.core;
  context_.gpr = core.r;
  context_.ram = core.ram;
  context_.cop0 = core.cop0;
  context_.executed = &state.executed;
  context_.memoryAccesses = &state.counters.memoryAccesses;
  context_.stop = reinterpret_cast<const std::uint8_t *>(&state.stopRequested);
  context_.scratch = core.scratch;
  const VertexPlaces::LoadGate gate = core.vertexPlaces.loadGate();
  context_.tracking = gate.tracking;
  context_.carriedMask = gate.carriedMask;
  context_.faceLoadMask = gate.faceLoadMask;
  context_.placedWords = gate.placedWords;
  context_.owner = this;
  context_.helpers[kHelperLoad] = reinterpret_cast<void *>(&helperLoad);
  context_.helpers[kHelperLoadUnaligned] = reinterpret_cast<void *>(&helperLoadUnaligned);
  context_.helpers[kHelperStore] = reinterpret_cast<void *>(&helperStore);
  context_.helpers[kHelperLwc2] = reinterpret_cast<void *>(&helperLwc2);
  context_.helpers[kHelperGteCommand] = reinterpret_cast<void *>(&helperGteCommand);
  context_.helpers[kHelperMfc2] = reinterpret_cast<void *>(&helperMfc2);
  context_.helpers[kHelperCfc2] = reinterpret_cast<void *>(&helperCfc2);
  context_.helpers[kHelperMtc2] = reinterpret_cast<void *>(&helperMtc2);
  context_.helpers[kHelperCtc2] = reinterpret_cast<void *>(&helperCtc2);
  context_.helpers[kHelperMtc0] = reinterpret_cast<void *>(&helperMtc0);
  context_.helpers[kHelperRfe] = reinterpret_cast<void *>(&helperRfe);
  context_.helpers[kHelperDiv] = reinterpret_cast<void *>(&helperDiv);
  context_.helpers[kHelperDivu] = reinterpret_cast<void *>(&helperDivu);
}

Recompiler::~Recompiler() {
  logReport();
}

BlockOutcome Recompiler::interpret(std::uint32_t pc, SegmentLimits limits, Refusal reason) {
  ++counters_.interpreted[static_cast<std::size_t>(reason)];
  return interpretBlock(state_, pc, pc, limits);
}

CompiledBlock *Recompiler::compile(std::uint32_t pc) {
  const ScannedBlock scanned = scanBlock(state_, pc, kMaxBlockInstructions);
  CompiledBlock block;
  block.pc = pc;
  block.refusal = scanned.refusal;
  if (scanned.refusal == Refusal::NotMainRam) {
    return nullptr;
  }
  block.ramBegin = scanned.ramBegin;
  block.ramEnd = scanned.ramBegin + static_cast<std::uint32_t>(scanned.code.size() * 4u);
  if (block.ramEnd > kMainRamBytes) {
    block.ramEnd = kMainRamBytes;
  }
  if (scanned.refusal == Refusal::None) {
    block.entryConflicts = loadDelayConflicts(scanned.code.front().word);
    block.code = translateArm64(scanned, *memory_);
    if (block.code == nullptr) {
      if (running_ != nullptr) {
        block.refusal = Refusal::CodeMemoryBusy;
        return nullptr;
      }
      cache_.clear();
      memory_->reset();
      ++counters_.flushes;
      block.code = translateArm64(scanned, *memory_);
      if (block.code == nullptr) {
        block.refusal = Refusal::TooLong;
      }
    }
    if (block.code != nullptr) {
      ++counters_.translated;
      counters_.translatedInstructions += scanned.code.size();
      const std::size_t before = memory_->used();
      (void)before;
    }
  }
  return cache_.insert(block);
}

BlockOutcome Recompiler::runBlock(std::uint32_t pc, SegmentLimits limits) {
  if (state_.storeReport.armed) {
    return interpret(pc, limits, Refusal::StoreObserverArmed);
  }
  CompiledBlock *block = cache_.find(pc);
  if (block == nullptr) {
    block = compile(pc);
    if (block == nullptr) {
      return interpret(
          pc, limits, physical(pc) < kRamMirrorSpan && (pc & 3u) == 0 ? Refusal::CodeMemoryBusy : Refusal::NotMainRam);
    }
  }
  if (block->code == nullptr) {
    return interpret(pc, limits, block->refusal);
  }
  DelayedLoad &pending = state_.pending;
  if (pending.reg != 0) {
    if ((block->entryConflicts & (1u << pending.reg)) != 0) {
      return interpret(pc, limits, Refusal::PendingLoadAtEntry);
    }
    state_.core.r[pending.reg] = pending.value;
    pending.reg = 0;
  }
  context_.segmentEnd = limits.start + limits.budget;
  context_.selfModified = 0;
  context_.pendingReg = 0;
  segmentStart_ = limits.start;
  running_ = block;
  const auto exit = static_cast<JitExit>(block->code(&context_));
  running_ = nullptr;
  ++counters_.translatedRuns;
  if (context_.pendingReg != 0) {
    pending = {context_.pendingReg, context_.pendingValue};
  }
  const std::uint32_t exitPc = context_.exitPc;
  switch (exit) {
  case JitExit::Entry:
    return {BlockEnd::Entry, exitPc, exitPc + 4u, nullptr};
  case JitExit::Syscall:
    return {BlockEnd::Syscall, exitPc, exitPc + 4u, nullptr};
  case JitExit::Break:
    return {BlockEnd::Break, exitPc, exitPc + 4u, nullptr};
  case JitExit::Fault: {
    ++state_.counters.faults;
    std::uint32_t word = 0;
    state_.fetch(exitPc, word);
    const char *fault = unknownInstructionFault(word);
    return {BlockEnd::Fault, exitPc, exitPc + 4u, fault != nullptr ? fault : "unknown instruction"};
  }
  case JitExit::Resume:
    ++counters_.resumes;
    return interpretBlock(state_, exitPc, block->pc, limits);
  }
  return {BlockEnd::Fault, exitPc, exitPc + 4u, "translated block returned an unknown exit"};
}

void Recompiler::invalidate(GuestAddressRange range) {
  const std::uint32_t begin = range.begin;
  const std::uint32_t end = range.end < kRamMirrorSpan ? range.end : kRamMirrorSpan;
  if (begin >= end) {
    return;
  }
  if (end - begin >= kMainRamBytes) {
    invalidateAll();
    return;
  }
  const std::uint32_t first = begin & (kMainRamBytes - 1u);
  const std::uint32_t last = first + (end - begin);
  bool dropped = false;
  if (last <= kMainRamBytes) {
    dropped = cache_.invalidateRam(first, last);
  } else {
    dropped = cache_.invalidateRam(first, kMainRamBytes);
    dropped = cache_.invalidateRam(0, last - kMainRamBytes) || dropped;
  }
  if (dropped) {
    ++counters_.dropped;
  }
}

void Recompiler::invalidateAll() {
  if (cache_.liveBlocks() != 0) {
    ++counters_.dropped;
  }
  cache_.invalidateRam(0, kMainRamBytes);
}

void Recompiler::logReport() const {
  std::string refused;
  std::uint64_t interpreted = 0;
  for (std::size_t reason = 1; reason < counters_.interpreted.size(); ++reason) {
    interpreted += counters_.interpreted[reason];
    if (counters_.interpreted[reason] != 0) {
      refused += lucent::format(" {}={}", refusalName(static_cast<Refusal>(reason)), counters_.interpreted[reason]);
    }
  }
  if (counters_.translatedRuns == 0 && interpreted == 0) {
    return; // dropped before any block ran (PSXPORT_EXEC_INTERPRET)
  }
  lucent::info("recompiler",
               "translated={} blocks ({} guest instructions, {} KiB of host code in use) translated_runs={} "
               "interpreted_runs={} resumes={} invalidations_that_dropped={} flushes={}; interpreted by reason:{}",
               counters_.translated,
               counters_.translatedInstructions,
               memory_->used() >> 10,
               counters_.translatedRuns,
               interpreted,
               counters_.resumes,
               counters_.dropped,
               counters_.flushes,
               refused.empty() ? " none" : refused);
}

// ---- Helpers ---------------------------------------------------------------------------------

std::uint32_t Recompiler::helperLoad(JitContext *context, std::uint32_t instruction, std::uint32_t address) {
  return owner(context).state_.loadValue(instruction, address);
}

std::uint32_t Recompiler::helperLoadUnaligned(JitContext *context,
                                              std::uint32_t instruction,
                                              std::uint32_t address,
                                              std::uint32_t base) {
  return owner(context).state_.loadUnaligned(instruction, address, base);
}

std::uint32_t
Recompiler::helperStore(JitContext *context, std::uint32_t instruction, std::uint32_t pc, std::uint32_t address) {
  Recompiler &self = owner(context);
  ExecutorState &state = self.state_;
  state.storeInstruction(instruction, pc, address, static_cast<std::uint32_t>(state.executed - self.segmentStart_));
  if (self.running_ != nullptr && !self.running_->valid) {
    context->selfModified = 1;
    return 1;
  }
  return 0;
}

void Recompiler::helperLwc2(JitContext *context, std::uint32_t instruction, std::uint32_t address) {
  ExecutorState &state = owner(context).state_;
  const std::uint32_t value = state.load(instruction, address);
  state.core.gte().writeData(rtField(instruction), value);
}

void Recompiler::helperGteCommand(JitContext *context, std::uint32_t instruction) {
  owner(context).state_.core.gte().execute(instruction);
}

std::uint32_t Recompiler::helperMfc2(JitContext *context, std::uint32_t instruction) {
  ExecutorState &state = owner(context).state_;
  const std::uint32_t value = state.readGteData(rdField(instruction));
  state.cop2Transfer(instruction, value);
  return value;
}

std::uint32_t Recompiler::helperCfc2(JitContext *context, std::uint32_t instruction) {
  ExecutorState &state = owner(context).state_;
  const std::uint32_t value = state.core.gte().readControl(rdField(instruction));
  state.cop2Transfer(instruction, value);
  return value;
}

void Recompiler::helperMtc2(JitContext *context, std::uint32_t instruction, std::uint32_t value) {
  owner(context).state_.core.gte().writeData(rdField(instruction), value);
}

void Recompiler::helperCtc2(JitContext *context, std::uint32_t instruction, std::uint32_t value) {
  ExecutorState &state = owner(context).state_;
  state.core.gte().writeControl(rdField(instruction), value);
  state.cop2Transfer(instruction, value);
}

void Recompiler::helperMtc0(JitContext *context, std::uint32_t reg, std::uint32_t value) {
  owner(context).state_.writeCop0(reg, value);
}

void Recompiler::helperRfe(JitContext *context) {
  owner(context).state_.returnFromException();
}

void Recompiler::helperDiv(JitContext *context, std::uint32_t numerator, std::uint32_t denominator) {
  Core &core = owner(context).state_.core;
  const Division operands{numerator, denominator};
  core.lo = quotient(operands);
  core.hi = remainder(operands);
}

void Recompiler::helperDivu(JitContext *context, std::uint32_t numerator, std::uint32_t denominator) {
  Core &core = owner(context).state_.core;
  core.lo = denominator == 0 ? 0xFFFFFFFFu : numerator / denominator;
  core.hi = denominator == 0 ? numerator : numerator % denominator;
}

} // namespace psx::cpu::recompiler
