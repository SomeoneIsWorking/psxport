#include "arm64_translator.h"

#include "arm64_assembler.h"
#include "guest_memory_map.h"
#include "jit_context.h"
#include "r3000.h"
#include "r3000_decode.h"
#include "vertex_places.h"

#include <cstddef>
#include <cstring>
#include <deque>
#include <vector>

namespace psx::cpu::recompiler {
namespace {

using namespace arm64;

static_assert(offsetof(R3000, hi) == kHiOffset && offsetof(R3000, lo) == kLoOffset, "R3000 register file layout");

// Pinned host registers, all callee-saved so helper calls keep them.
constexpr Reg kCtx = 19;    // JitContext *
constexpr Reg kGpr = 20;    // Core::r
constexpr Reg kExec = 21;   // executed instructions, synced at labels and exits
constexpr Reg kRam = 22;    // Core::ram
constexpr Reg kMemAcc = 23; // memory accesses the inline loads made
constexpr Reg kHeld = 24;   // a delayed load's value until it lands
constexpr Reg kCond = 25;   // the taken flag of the branch whose delay slot runs
constexpr Reg kTarget = 26; // the target of JR/JALR
constexpr Reg kImm = 17;    // scratch for immediates that need a register

#define CTX(field) static_cast<std::uint32_t>(offsetof(JitContext, field))

// What a load still in flight at an instruction does there (the interpreter's arriving/pending pair, resolved here).
enum class Arrival : std::uint8_t {
  None,
  LandBefore, // the instruction neither reads nor writes it: land first
  Hold,       // the instruction reads the old value: land after it, unless it writes the register
  Drop,       // the instruction loads the same register: the first load never lands
  Merge,      // LWL/LWR into the same register merges into the value in flight, which never lands itself
};

std::uint32_t bit(std::uint32_t reg) {
  return reg == 0 ? 0u : 1u << reg;
}

bool isStoreWord(std::uint32_t word) {
  return isStore(primaryOpcode(word));
}

bool leavesEngine(std::uint32_t word) {
  return unknownInstructionFault(word) != nullptr ||
         (primaryOpcode(word) == kSpecial && (functField(word) == kSyscall || functField(word) == kBreak));
}

Arrival arrivalAt(std::uint32_t inFlight, std::uint32_t word) {
  if (inFlight == 0) {
    return Arrival::None;
  }
  if (leavesEngine(word)) {
    return Arrival::LandBefore; // the interpreter lands it as the instruction leaves
  }
  if (delayedLoadTarget(word) == inFlight) {
    const std::uint32_t op = primaryOpcode(word);
    return op == kLwl || op == kLwr ? Arrival::Merge : Arrival::Drop;
  }
  if ((gprReads(word) & bit(inFlight)) != 0) {
    return Arrival::Hold;
  }
  return Arrival::LandBefore;
}

class Translator {
public:
  explicit Translator(const ScannedBlock &block) : block_(block), labels_(block.code.size()) {}

  void translate() {
    prologue();
    const std::vector<ScannedInstruction> &code = block_.code;
    for (std::size_t i = 0; i < code.size(); ++i) {
      const ScannedInstruction &ins = code[i];
      const std::uint32_t inFlight = i > 0 ? delayedLoadTarget(code[i - 1].word) : 0u;
      const Arrival arrival = arrivalAt(inFlight, ins.word);
      if (arrival == Arrival::LandBefore) {
        storeGpr(kHeld, inFlight);
      }
      if (ins.target) {
        syncExecuted();
        a.bind(labels_[i]);
      }
      ++k_;
      const Terminal terminal = body(ins, arrival);
      if (arrival == Arrival::Hold && immediateGprWrite(ins.word) != inFlight) {
        storeGpr(kHeld, inFlight);
      }
      if (delayedLoadTarget(ins.word) != 0) {
        a.movw(kHeld, 0, true);
      }
      if (isStoreWord(ins.word) && !ins.delaySlot) {
        a.cbnz(0, stub(JitExit::Resume, ins.pc + 4u, 0));
      }
      if (terminal.ends && terminal.exit != JitExit::Entry) {
        exitTo(terminal.exit, terminal.pc, 0); // a fault leaves at once, delay slot or not
        break;
      }
      if (ins.delaySlot) {
        edges(code[i - 1], ins);
        if (isUnconditionalJump(code[i - 1].word) || endsBlock(ins.word)) {
          break;
        }
        continue;
      }
      if (terminal.ends) {
        exitTo(terminal.exit, terminal.pc, 0);
        break;
      }
    }
    for (Stub &s : stubs_) {
      a.bind(s.label);
      exitSequence(s.exit, s.pc, s.k, s.pendingReg);
    }
    epilogue();
  }

  Assembler a;

private:
  struct Terminal {
    bool ends = false;
    JitExit exit = JitExit::Entry;
    std::uint32_t pc = 0;
  };
  struct Stub {
    Label label;
    JitExit exit = JitExit::Entry;
    std::uint32_t pc = 0;
    std::uint32_t k = 0;
    std::uint32_t pendingReg = 0;
  };

  // ---- Frame ----

  void prologue() {
    a.stpxPre(29, 30, kSp, -96);
    a.addxi(29, kSp, 0);
    a.stpx(19, 20, kSp, 16);
    a.stpx(21, 22, kSp, 32);
    a.stpx(23, 24, kSp, 48);
    a.stpx(25, 26, kSp, 64);
    a.movx(kCtx, 0);
    a.ldrx(kGpr, kCtx, CTX(gpr));
    a.ldrx(kRam, kCtx, CTX(ram));
    a.ldrx(9, kCtx, CTX(executed));
    a.ldrx(kExec, 9, 0);
    a.movw(kMemAcc, 0u);
  }

  // w0 holds the exit code and ctx->exitPc the exit pc.
  void epilogue() {
    a.bind(exit_);
    a.ldrx(9, kCtx, CTX(executed));
    a.strx(kExec, 9, 0);
    a.ldrx(9, kCtx, CTX(memoryAccesses));
    a.ldrx(10, 9, 0);
    a.addx(10, 10, kMemAcc);
    a.strx(10, 9, 0);
    a.ldpx(25, 26, kSp, 64);
    a.ldpx(23, 24, kSp, 48);
    a.ldpx(21, 22, kSp, 32);
    a.ldpx(19, 20, kSp, 16);
    a.ldpxPost(29, 30, kSp, 96);
    a.ret();
  }

  // ---- Exits ----

  void syncExecuted() {
    if (k_ != 0) {
      a.addxi(kExec, kExec, k_);
      k_ = 0;
    }
  }

  void exitSequence(JitExit exit, std::uint32_t pc, std::uint32_t k, std::uint32_t pendingReg) {
    if (pendingReg != 0) {
      a.movw(9, pendingReg);
      a.strw(9, kCtx, CTX(pendingReg));
      a.strw(kHeld, kCtx, CTX(pendingValue));
    }
    if (k != 0) {
      a.addxi(kExec, kExec, k);
    }
    a.movw(9, pc);
    a.strw(9, kCtx, CTX(exitPc));
    a.movw(0, static_cast<std::uint32_t>(exit));
    a.b(exit_);
  }

  // An exit on the straight path at the current count.
  void exitTo(JitExit exit, std::uint32_t pc, std::uint32_t pendingReg) {
    exitSequence(exit, pc, k_, pendingReg);
  }

  // An out-of-line exit at the current count, taken by a branch to the returned label.
  Label &stub(JitExit exit, std::uint32_t pc, std::uint32_t pendingReg) {
    stubs_.push_back({{}, exit, pc, k_, pendingReg});
    return stubs_.back().label;
  }

  // ---- Guest registers ----

  void loadGpr(Reg w, std::uint32_t reg) {
    if (reg == 0) {
      a.movw(w, kZr, true);
    } else {
      a.ldrw(w, kGpr, reg * 4u);
    }
  }
  void storeGpr(Reg w, std::uint32_t reg) {
    if (reg != 0) {
      a.strw(w, kGpr, reg * 4u);
    }
  }

  void addImmediate(Reg rd, Reg rn, std::uint32_t value) {
    const auto signedValue = static_cast<std::int32_t>(value);
    if (signedValue >= 0 && signedValue < 4096) {
      a.addwi(rd, rn, value);
    } else if (signedValue < 0 && signedValue > -4096) {
      a.subwi(rd, rn, static_cast<std::uint32_t>(-signedValue));
    } else {
      a.movw(kImm, value);
      a.addw(rd, rn, kImm);
    }
  }

  void compareImmediate(Reg rn, std::uint32_t value) {
    const auto signedValue = static_cast<std::int32_t>(value);
    if (signedValue >= 0 && signedValue < 4096) {
      a.cmpwi(rn, value);
    } else if (signedValue < 0 && signedValue > -4096) {
      a.cmnwi(rn, static_cast<std::uint32_t>(-signedValue));
    } else {
      a.movw(kImm, value);
      a.cmpw(rn, kImm);
    }
  }

  // The effective address of a load or store into w9.
  void effectiveAddress(const ScannedInstruction &ins) {
    loadGpr(9, rsField(ins.word));
    addImmediate(9, 9, signedImmediate(ins.word));
  }

  void callHelper(Helper helper) {
    a.movx(0, kCtx);
    a.ldrx(16, kCtx, CTX(helpers) + helper * 8u);
    a.blr(16);
  }

  // A memory helper commits guest time at a device access, so it reads the count through this instruction.
  void publishExecuted() {
    a.addxi(9, kExec, k_);
    a.ldrx(10, kCtx, CTX(executed));
    a.strx(9, 10, 0);
  }

  // ---- Instructions ----

  // The instruction's own work. A delayed load leaves its value in w0; a store leaves the helper's self-modified
  // flag in w0.
  Terminal body(const ScannedInstruction &ins, Arrival arrival) {
    const std::uint32_t word = ins.word;
    const std::uint32_t rs = rsField(word);
    const std::uint32_t rt = rtField(word);
    const std::uint32_t rd = rdField(word);
    if (unknownInstructionFault(word) != nullptr) {
      return {true, JitExit::Fault, ins.pc};
    }
    switch (primaryOpcode(word)) {
    case kSpecial:
      return special(ins);
    case kRegimm:
      loadGpr(9, rs);
      a.cmpwi(9, 0);
      a.csetw(kCond, (rt & 1u) != 0 ? kGe : kLt);
      if ((rt & 0x1Eu) == 0x10u) {
        a.movw(9, ins.pc + 8u);
        storeGpr(9, kRa);
      }
      break;
    case kJ:
      break;
    case kJal:
      a.movw(9, ins.pc + 8u);
      storeGpr(9, kRa);
      break;
    case kBeq:
    case kBne:
      loadGpr(9, rs);
      loadGpr(10, rt);
      a.cmpw(9, 10);
      a.csetw(kCond, primaryOpcode(word) == kBeq ? kEq : kNe);
      break;
    case kBlez:
    case kBgtz:
      loadGpr(9, rs);
      a.cmpwi(9, 0);
      a.csetw(kCond, primaryOpcode(word) == kBlez ? kLe : kGt);
      break;
    case kAddi:
    case kAddiu:
      if (rt != 0) {
        loadGpr(9, rs);
        addImmediate(9, 9, signedImmediate(word));
        storeGpr(9, rt);
      }
      break;
    case kSlti:
    case kSltiu:
      if (rt != 0) {
        loadGpr(9, rs);
        compareImmediate(9, signedImmediate(word));
        a.csetw(9, primaryOpcode(word) == kSlti ? kLt : kLo);
        storeGpr(9, rt);
      }
      break;
    case kAndi:
    case kOri:
    case kXori:
      if (rt != 0) {
        loadGpr(9, rs);
        logicalImmediate(primaryOpcode(word), 9, 9, immediate(word));
        storeGpr(9, rt);
      }
      break;
    case kLui:
      if (rt != 0) {
        a.movw(9, immediate(word) << 16);
        storeGpr(9, rt);
      }
      break;
    case kCop0:
      if (rs == kMfc || rs == kCfc) {
        if (rd < 16) {
          a.ldrx(9, kCtx, CTX(cop0));
          a.ldrw(0, 9, rd * 4u);
        } else {
          a.movw(0, 0u);
        }
      } else if (rs == kMtc || rs == kCtc) {
        loadGpr(2, rt);
        a.movw(1, rd);
        callHelper(kHelperMtc0);
        if (endsBlock(word)) {
          return {true, JitExit::Entry, ins.pc + 4u};
        }
      } else {
        callHelper(kHelperRfe);
      }
      break;
    case kCop2:
      if ((word & (1u << 25)) != 0) {
        a.movw(1, word);
        callHelper(kHelperGteCommand);
      } else if (rs == kMfc || rs == kCfc) {
        a.movw(1, word);
        callHelper(rs == kMfc ? kHelperMfc2 : kHelperCfc2);
      } else {
        loadGpr(2, rt);
        a.movw(1, word);
        callHelper(rs == kMtc ? kHelperMtc2 : kHelperCtc2);
      }
      break;
    case kLb:
    case kLh:
    case kLw:
    case kLbu:
    case kLhu:
      load(ins);
      break;
    case kLwl:
    case kLwr:
      effectiveAddress(ins);
      a.movw(2, 9, true);
      if (arrival == Arrival::Merge) {
        a.movw(3, kHeld, true);
      } else {
        loadGpr(3, rt);
      }
      a.movw(1, word);
      publishExecuted();
      callHelper(kHelperLoadUnaligned);
      break;
    case kLwc2:
      effectiveAddress(ins);
      a.movw(2, 9, true);
      a.movw(1, word);
      publishExecuted();
      callHelper(kHelperLwc2);
      break;
    case kSb:
    case kSh:
    case kSw:
    case kSwl:
    case kSwr:
    case kSwc2:
      effectiveAddress(ins);
      a.movw(3, 9, true);
      a.movw(2, ins.pc);
      a.movw(1, word);
      publishExecuted();
      callHelper(kHelperStore);
      break;
    default:
      return {true, JitExit::Fault, ins.pc};
    }
    return {};
  }

  Terminal special(const ScannedInstruction &ins) {
    const std::uint32_t word = ins.word;
    const std::uint32_t rs = rsField(word);
    const std::uint32_t rt = rtField(word);
    const std::uint32_t rd = rdField(word);
    const std::uint32_t shift = shamtField(word);
    const std::uint32_t funct = functField(word);
    switch (funct) {
    case kSyscall:
      return {true, JitExit::Syscall, ins.pc};
    case kBreak:
      return {true, JitExit::Break, ins.pc};
    case kJr:
      loadGpr(kTarget, rs);
      return {};
    case kJalr:
      loadGpr(kTarget, rs);
      a.movw(9, ins.pc + 8u);
      storeGpr(9, rd);
      return {};
    case kMthi:
    case kMtlo:
      loadGpr(9, rs);
      a.strw(9, kGpr, funct == kMthi ? kHiOffset : kLoOffset);
      return {};
    case kMult:
    case kMultu:
      loadGpr(9, rs);
      loadGpr(10, rt);
      if (funct == kMult) {
        a.smull(11, 9, 10);
      } else {
        a.umull(11, 9, 10);
      }
      a.strw(11, kGpr, kLoOffset);
      a.lsrxi(11, 11, 32);
      a.strw(11, kGpr, kHiOffset);
      return {};
    case kDiv:
    case kDivu:
      loadGpr(1, rs);
      loadGpr(2, rt);
      callHelper(funct == kDiv ? kHelperDiv : kHelperDivu);
      return {};
    default:
      break;
    }
    if (rd == 0) {
      return {}; // every remaining form only writes rd
    }
    switch (funct) {
    case kSll:
    case kSrl:
    case kSra:
      loadGpr(9, rt);
      if (shift != 0) {
        if (funct == kSll) {
          a.lslwi(9, 9, shift);
        } else if (funct == kSrl) {
          a.lsrwi(9, 9, shift);
        } else {
          a.asrwi(9, 9, shift);
        }
      }
      break;
    case kSllv:
    case kSrlv:
    case kSrav:
      loadGpr(9, rt);
      loadGpr(10, rs);
      if (funct == kSllv) {
        a.lslvw(9, 9, 10);
      } else if (funct == kSrlv) {
        a.lsrvw(9, 9, 10);
      } else {
        a.asrvw(9, 9, 10);
      }
      break;
    case kMfhi:
      a.ldrw(9, kGpr, kHiOffset);
      break;
    case kMflo:
      a.ldrw(9, kGpr, kLoOffset);
      break;
    default:
      loadGpr(9, rs);
      loadGpr(10, rt);
      switch (funct) {
      case kAdd:
      case kAddu:
        a.addw(9, 9, 10);
        break;
      case kSub:
      case kSubu:
        a.subw(9, 9, 10);
        break;
      case kAnd:
        a.andw(9, 9, 10);
        break;
      case kOr:
        a.orrw(9, 9, 10);
        break;
      case kXor:
        a.eorw(9, 9, 10);
        break;
      case kNor:
        a.orrw(9, 9, 10);
        a.ornw(9, kZr, 9);
        break;
      case kSlt:
        a.cmpw(9, 10);
        a.csetw(9, kLt);
        break;
      default: // kSltu
        a.cmpw(9, 10);
        a.csetw(9, kLo);
        break;
      }
      break;
    }
    storeGpr(9, rd);
    return {};
  }

  void logicalImmediate(std::uint32_t op, Reg rd, Reg rn, std::uint32_t value) {
    bool encoded = false;
    if (op == kAndi) {
      encoded = a.andwi(rd, rn, value);
    } else if (op == kOri) {
      encoded = value == 0 ? (a.movw(rd, rn, true), true) : a.orrwi(rd, rn, value);
    } else {
      encoded = value == 0 ? (a.movw(rd, rn, true), true) : a.eorwi(rd, rn, value);
    }
    if (encoded) {
      return;
    }
    a.movw(kImm, value);
    if (op == kAndi) {
      a.andw(rd, rn, kImm);
    } else if (op == kOri) {
      a.orrw(rd, rn, kImm);
    } else {
      a.eorw(rd, rn, kImm);
    }
  }

  // LB/LH/LW/LBU/LHU into w0. Main RAM and the scratchpad are read inline unless the vertex places would note
  // something for this load (VertexPlaces::LoadGate); every other address goes through the helper.
  void load(const ScannedInstruction &ins) {
    const std::uint32_t op = primaryOpcode(ins.word);
    const std::uint32_t rt = rtField(ins.word);
    Label slow;
    Label scratchpad;
    Label located;
    Label plain;
    Label done;
    effectiveAddress(ins);
    if (op == kLw) {
      a.tstwi(9, 3u);
      a.bcond(kNe, slow);
    } else if (op == kLh || op == kLhu) {
      a.tstwi(9, 1u);
      a.bcond(kNe, slow);
    }
    // w10: byte offset into the region at x13; w12: the region word's vertex-place slot.
    a.andwi(11, 9, 0x1FFFFC00u);
    a.movw(12, kScratchpadPhysicalBase);
    a.cmpw(11, 12);
    a.bcond(kEq, scratchpad);
    a.tstwi(9, 0x1F800000u); // outside the 8 MiB of RAM mirrors
    a.bcond(kNe, slow);
    a.andwi(10, 9, kMainRamBytes - 1u);
    a.movx(13, kRam);
    a.lsrwi(12, 10, 2);
    a.b(located);
    a.bind(scratchpad);
    a.andwi(10, 9, kScratchpadBytes - 1u);
    a.ldrx(13, kCtx, CTX(scratch));
    a.lsrwi(12, 10, 2);
    a.addwiHigh(12, 12, VertexPlaces::kScratchpadSlot >> 12);
    a.bind(located);
    a.ldrx(11, kCtx, CTX(tracking));
    a.ldrb(11, 11, 0);
    a.cbz(11, plain);
    if (rt != 0) {
      a.ldrx(11, kCtx, CTX(carriedMask));
      a.ldrw(11, 11, 0);
      a.tbnz(11, rt, slow);
    }
    if (op == kLw) {
      a.ldrx(11, kCtx, CTX(faceLoadMask));
      a.ldrw(11, 11, 0);
      a.movw(14, VertexPlaces::faceLoadBit(ins.word));
      a.andw(11, 11, 14);
      a.cbnz(11, slow);
      if (rt != 0) {
        a.ldrx(11, kCtx, CTX(placedWords));
        a.ldrx(11, 11, 0);
        a.cbzx(11, plain);
        a.lsrwi(14, 12, 6);
        a.ldrxScaled(11, 11, 14);
        a.lsrvx(11, 11, 12);
        a.tbnz(11, 0, slow);
      }
    }
    a.bind(plain);
    switch (op) {
    case kLb:
      a.ldrsbwr(0, 13, 10);
      break;
    case kLbu:
      a.ldrbr(0, 13, 10);
      break;
    case kLh:
      a.ldrshwr(0, 13, 10);
      break;
    case kLhu:
      a.ldrhr(0, 13, 10);
      break;
    default:
      a.ldrwr(0, 13, 10);
      break;
    }
    a.addxi(kMemAcc, kMemAcc, 1);
    a.b(done);
    a.bind(slow);
    a.movw(2, 9, true);
    a.movw(1, ins.word);
    publishExecuted();
    callHelper(kHelperLoad);
    a.bind(done);
  }

  // ---- Branches ----

  // After the delay slot `slot` of `branch`: where the block goes next.
  void edges(const ScannedInstruction &branch, const ScannedInstruction &slot) {
    const std::uint32_t word = branch.word;
    const std::uint32_t inFlight = delayedLoadTarget(slot.word);
    const std::uint32_t fallThrough = slot.pc + 4u;
    const std::uint32_t op = primaryOpcode(word);
    if (op == kSpecial) { // JR, JALR
      if (inFlight != 0) {
        a.movw(9, inFlight);
        a.strw(9, kCtx, CTX(pendingReg));
        a.strw(kHeld, kCtx, CTX(pendingValue));
      }
      syncExecuted();
      a.strw(kTarget, kCtx, CTX(exitPc));
      a.movw(0, static_cast<std::uint32_t>(JitExit::Entry));
      a.b(exit_);
      return;
    }
    const std::uint32_t target = op == kJ || op == kJal ? (fallThrough & 0xF0000000u) | ((word & 0x03FFFFFFu) << 2)
                                                        : branch.pc + 4u + (signedImmediate(word) << 2);
    if (op == kJ || op == kJal) {
      exitTo(JitExit::Entry, target, inFlight);
      return;
    }
    if (isUnconditionalJump(word)) {
      // The always-taken forms still test their register (BLEZ with rs == rt): taken or not, the block ends.
      if (inFlight != 0) {
        a.movw(9, inFlight);
        a.strw(9, kCtx, CTX(pendingReg));
        a.strw(kHeld, kCtx, CTX(pendingValue));
      }
      syncExecuted();
      a.movw(9, target);
      a.movw(10, fallThrough);
      a.cmpwi(kCond, 0);
      a.cselw(9, 9, 10, kNe);
      a.strw(9, kCtx, CTX(exitPc));
      a.movw(0, static_cast<std::uint32_t>(JitExit::Entry));
      a.b(exit_);
      return;
    }
    Label notTaken;
    a.cbz(kCond, notTaken);
    const long landing = block_.indexOf(target);
    if (endsBlock(slot.word)) {
      exitTo(JitExit::Entry, target, 0);
      a.bind(notTaken);
      exitTo(JitExit::Entry, fallThrough, 0);
      return;
    }
    if (landing >= 0) {
      // Stays in the block unless the segment's budget is spent or a stop was requested at the branch.
      a.addxi(9, kExec, k_ - 1u);
      a.ldrx(10, kCtx, CTX(segmentEnd));
      a.cmpx(9, 10);
      a.bcond(kHs, stub(JitExit::Entry, target, inFlight));
      a.ldrx(10, kCtx, CTX(stop));
      a.ldrb(10, 10, 0);
      a.cbnz(10, stub(JitExit::Entry, target, inFlight));
      if (isStoreWord(slot.word)) {
        a.ldrw(10, kCtx, CTX(selfModified));
        a.cbnz(10, stub(JitExit::Resume, target, 0));
      }
      storeGpr(kHeld, inFlight);
      a.addxi(kExec, kExec, k_);
      a.b(labels_[static_cast<std::size_t>(landing)]);
    } else {
      exitTo(JitExit::Entry, target, inFlight);
    }
    a.bind(notTaken);
    if (isStoreWord(slot.word)) {
      a.ldrw(10, kCtx, CTX(selfModified));
      a.cbnz(10, stub(JitExit::Resume, fallThrough, 0));
    }
  }

  const ScannedBlock &block_;
  std::vector<Label> labels_;
  std::deque<Stub> stubs_;
  Label exit_;
  // Instructions run since kExec was last advanced; every label is bound at 0.
  std::uint32_t k_ = 0;
};

} // namespace

BlockCode translateArm64(const ScannedBlock &block, CodeMemory &memory) {
  Translator translator(block);
  translator.translate();
  const std::vector<std::uint32_t> &words = translator.a.words();
  const std::size_t bytes = words.size() * sizeof(std::uint32_t);
  if (bytes > memory.remaining()) {
    return nullptr;
  }
  std::uint8_t *const destination = memory.cursor();
  {
    const CodeMemory::WriteScope writable(memory);
    std::memcpy(destination, words.data(), bytes);
  }
  memory.commit(bytes);
  return reinterpret_cast<BlockCode>(destination);
}

} // namespace psx::cpu::recompiler
