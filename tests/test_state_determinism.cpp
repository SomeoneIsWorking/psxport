// test_state_determinism.cpp — a machine resumed from a save state executes the SAME guest program as
// the machine that took it, in a DIFFERENT PROCESS.
//
// WHY IT IS TWO PROCESSES, and not two machines in one. Measured, not assumed: Lightrec supports one
// initialized machine per process, and the second `Core` in a process fails initialization outright
// ("Lightrec initialization failed"). The first version of this test built two machines side by side
// and died on exactly that. So the only shape that can compare a resumed machine against the one that
// saved is two processes with a FILE between them — which is also the shape a real workflow uses, and
// the shape that catches a state that only round-trips because some process-global happened to be
// shared.
//
// WHAT IS COMPARED. The synthetic guest below walks a window of guest RAM, folding what it finds into
// a running value, pushing that value through the GTE and out to an MMIO register, for 400 iterations.
// Phase A runs before the state boundary; phase B runs after it. Phase B's output depends on the RAM
// phase A left, so the digest after phase B is a function of everything the state carried.
//
//   --phase save    build a machine, run phase A, WRITE THE STATE to --state, run phase B, print the
//                   digest of all 2 MB of guest RAM
//   --phase load    build a machine, run NO phase A, LOAD --state, run phase B, print the digest
//   --phase control build a machine, run NO phase A and load nothing, run phase B, print the digest
//
// The driver (tests/test_state_determinism_process.py) runs all three and requires save == load and
// save != control. That last comparison is the one that stops the test from passing for the wrong
// reason: if the state file were being ignored, save and load would still agree.
//
// THREE MORE CHECKS THE DRIVER CANNOT MAKE, made here instead. Each is a way the comparison above
// could be satisfied by nothing happening: phase A must MOVE guest RAM; phase B must MOVE it further;
// and the work window must hold a non-zero result. A digest compared against itself passes every one
// of those, which is why they are asserted rather than assumed.
#include "field_digest.h"
#include "game.h"
#include "guest_call.h"
#include "machine_state.h"
#include "memcard.h"
#include "mips_asm.h"
#include "state_file.h"
#include "testutil.h"

#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace {

using namespace psx::test::mips;

// The synthetic guest, at KSEG0 addresses so the dispatcher resolves them against an image entry
// exactly as a real boot's does.
constexpr std::uint32_t kImageBase = 0x00010000u;
constexpr std::uint32_t kOuterReturn = 0x00020000u; // jr ra; nop — the return sentinel
constexpr std::uint32_t kPhaseA = 0x00010000u;
constexpr std::uint32_t kPhaseB = 0x00010800u;
constexpr std::uint32_t kImageEnd = 0x00100000u;
constexpr std::uint32_t kWorkBase = 0x80100000u; // the RAM window the loop walks
constexpr std::uint32_t kWorkLimit = 0x8000u;    // in words
constexpr int kIterations = 400;

// The ONE guest body both phases run. `base` is the address the body is assembled at, because the loop
// back-edge is a PC-RELATIVE branch: its displacement is measured from the instruction AFTER the
// branch, and assembling it without knowing where the branch itself landed produces a displacement
// that is perfectly well formed and points into the middle of the other phase.
std::vector<std::uint32_t> phaseBody(std::uint32_t base, std::uint32_t firstWord) {
  constexpr std::uint32_t kLoopIndex = 4; // the `lw` — where the back-edge goes
  std::vector<std::uint32_t> words = {
      lui(kS0, 0x8010),
      addiu(kS0, kS0, static_cast<std::int32_t>(firstWord * 4u)),
      addiu(kS1, kZero, kIterations),
      addiu(kS2, kZero, 1),
      // loop:
      lw(kT0, kS0, 0),     // base register is the SECOND argument; passing 0 here reads through $zero
      addu(kT1, kT0, kS2), // at guest address 0, which is BIOS territory, and looks like a working
      sw(kT1, kS0, 0),     // loop that simply never touches the work window
      mtc2(kT1, 4),        // a GTE data register the digest must be sensitive to
      cfc2(kT0, 30),       // ... read back through the control port
      addu(kS2, kT0, kS2),
      ori(kT2, kZero, 0x1F80), // %hi/%lo of I_MASK (0x1F801074), which the store below targets
      ori(kT2, kT2, 0x1074),
      sw(kS2, kT2, 0),
      addiu(kS0, kS0, 4),
      addiu(kS1, kS1, -1),
      0,     // the back-edge, patched below once its own index is known
      nop(), // its DELAY SLOT. `jr ra` here would execute on the LAST pass, before the loop
             // condition was re-tested, so the guest returned after ONE iteration while every
             // register said it had hundreds to go.
      jrRa(),
      nop(),
  };
  // The branch's displacement is measured from the instruction AFTER it, so it cannot be encoded until
  // the branch's own position is known. Hard-coding that position is what the first version did, and
  // it was wrong by one word after an instruction was inserted — which produced a displacement that
  // was perfectly well formed and looped back to the counter's INITIALISATION, so the guest ran the
  // loop forever and the budget expired with the counter reading its start value every pass.
  const auto branchIndex = words.size() - 4;
  words[branchIndex] = bne(kS1, kZero, base + static_cast<std::uint32_t>(branchIndex) * 4u, base + kLoopIndex * 4u);
  return words;
}

void writeCode(Core &core, std::uint32_t at, const std::vector<std::uint32_t> &words) {
  for (std::size_t i = 0; i < words.size(); ++i) {
    core.mem_w32(at + static_cast<std::uint32_t>(i) * 4u, words[i]);
  }
}

struct Machine {
  std::unique_ptr<Game> game;
  std::string cardPath;
  Core &core() {
    return game->core;
  }
};

Machine makeMachine() {
  Machine machine;
  machine.cardPath = "test_state_determinism.mcr";
  machine.game = std::make_unique<Game>();
  Core &core = machine.game->core;
  static GameConfig config{};
  config.cardDefaultPath = machine.cardPath.c_str();
  core.cfg = &config;
  std::remove(machine.cardPath.c_str());
  card_overrides_init(machine.game.get());
  core.imageCatalog().activate("state-determinism", {kImageBase, kImageEnd}, 0x53544154u);

  core.mem_w32(kOuterReturn, jrRa());
  core.mem_w32(kOuterReturn + 4u, nop());
  writeCode(core, kPhaseA, phaseBody(kPhaseA, 0));
  writeCode(core, kPhaseB, phaseBody(kPhaseB, kIterations));
  for (std::uint32_t i = 0; i < kWorkLimit; ++i) {
    core.mem_w32(kWorkBase + i * 4u, 0);
  }
  return machine;
}

void runPhase(Core &core, std::uint32_t entry) {
  core.r[kRa] = kOuterReturn;
  core.r[kSp] = 0x801FFF00u;
  psx::cpu::dispatchGuestToReturn(
      core, entry, psx::cpu::ExecutionBudget::fromCycles(2'000'000), "state determinism guest phase");
}

std::uint64_t ramDigest(const Core &core) {
  return psx::diag::FieldDigest::hashWords(std::span<const std::uint8_t>(core.ram, sizeof core.ram));
}

void test_the_guest_program_actually_moves_ram(void) {
  // Every digest comparison in the driver rests on this. A program that changed nothing would make
  // save == load true and control != save false in a way that reads as a working determinism proof.
  Machine machine = makeMachine();
  Core &core = machine.core();
  const std::uint64_t before = ramDigest(core);
  runPhase(core, kPhaseA);
  const std::uint64_t afterA = ramDigest(core);
  CHECK_MSG(afterA != before, "phase A left guest RAM untouched");
  runPhase(core, kPhaseB);
  const std::uint64_t afterB = ramDigest(core);
  CHECK_MSG(afterB != afterA, "phase B left guest RAM untouched");
  CHECK_MSG(core.mem_r32(kWorkBase) != 0, "the work window's first word is still zero after both phases");
  CHECK_MSG(core.mem_r32(kWorkBase + kIterations * 4u) != 0,
            "the window phase B wrote is still zero, so the two phases are not walking different ranges");
}

} // namespace

int main(int argc, char **argv) {
  const bool asPhase = argc >= 2 && std::strcmp(argv[1], "--phase") == 0;
  if (!asPhase) {
    RUN(the_guest_program_actually_moves_ram);
    std::remove("test_state_determinism.mcr");
    return pt_summary();
  }

  const std::string phase = argc >= 3 ? argv[2] : "";
  const std::string statePath = argc >= 4 ? argv[3] : "";
  if (phase != "save" && phase != "load" && phase != "control") {
    std::fprintf(stderr, "usage: %s --phase save|load|control <state-file>\n", argv[0]);
    return 2;
  }

  Machine machine = makeMachine();
  Core &core = machine.core();
  if (phase == "save") {
    runPhase(core, kPhaseA);
    psx::state::MachineState state(*machine.game);
    std::string error;
    const auto image = state.capture(error);
    if (!image) {
      std::fprintf(stderr, "state capture FAILED: %s\n", error.c_str());
      return 1;
    }
    if (!psx::state::writeFile(statePath, *image, error)) {
      std::fprintf(stderr, "state write FAILED: %s\n", error.c_str());
      return 1;
    }
    std::fprintf(stderr, "state written: %zu bytes\n", image->size());
    runPhase(core, kPhaseB);
  } else if (phase == "load") {
    psx::state::MachineState state(*machine.game);
    std::string error;
    const auto bytes = psx::state::readFile(statePath, error);
    if (!bytes) {
      std::fprintf(stderr, "state read FAILED: %s\n", error.c_str());
      return 1;
    }
    const auto outcome = state.restore(*bytes, error);
    if (!outcome) {
      std::fprintf(stderr, "state load FAILED: %s\n", error.c_str());
      return 1;
    }
    runPhase(core, kPhaseB);
  } else {
    runPhase(core, kPhaseB);
  }

  std::printf("%016llx\n", static_cast<unsigned long long>(ramDigest(core)));
  std::remove("test_state_determinism.mcr");
  return 0;
}