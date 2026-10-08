// test_bios_load_exec — BIOS A0:0x51 LoadAndExecute, through the SHIPPING dispatch path.
//
// The framework carried a process-scoped `s_loadexec_hook` for this leaf for months and never
// assigned it, so every guest LoadExec fell through `Hle::dispatchBios` as an "unimplemented BIOS
// service" and the executor raised a typed fault that aborted the run (Spyro 1's attract demo hits
// it at `0x8002BB08` -> `0x8005DB24`). What the leaf owes, per nocash psx-spx
// "A(51h) LoadAndExecute(filename,stackbase,stackoffset)", is:
//
//   - SUCCESS: load the executable the guest named from the CD, and START it. It does NOT return to
//     its caller, so the guest's continuation is the loaded program's entry and not the leaf's r[31].
//   - FAILURE: V0 = -1 and the caller resumes at its own r[31]. A failed load is a serviced call,
//     not an unimplemented one.
//
// This drives `psx::cpu::dispatchGuestHostService` at the A0 vector with r[9] = 0x51 — the same entry
// the executor's host-dispatch boundary uses — and the executable bytes come from an injected reader,
// so the loader, the register policy, the invalidation and the jump under test are the shipping ones.
#include "testutil.h"

#include "bios_load_exec.h"
#include "execution_control.h"
#include "game.h"
#include "image_identity.h"
#include "native_dispatch.h"
#include "psx_exe_image.h"

#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kBiosTableA = 0x000000a0u;
constexpr uint32_t kLoadExec = 0x51u;
constexpr uint32_t kBiosFunctionRegister = 9;
constexpr uint32_t kLinkRegister = 31;
constexpr uint32_t kA0 = 4, kA1 = 5, kA2 = 6, kV0 = 2;

// Where the test parks the guest's filename. Well clear of the BIOS work area the HLE publishes.
constexpr uint32_t kGuestFilename = 0x8001a000u;
// The caller's own return address: on success the BIOS must NOT resume here.
constexpr uint32_t kCallerReturn = 0x8002bb10u;

constexpr uint32_t kTextAddress = 0x80010000u;
constexpr uint32_t kTextBytes = 16u;
constexpr uint32_t kEntry = kTextAddress + 8u;
constexpr uint32_t kGlobalPointer = 0x80018000u;
constexpr uint32_t kHeaderStackBase = 0x80100000u;
// Spyro 1 states 0x801FFF00/0 — the top of main RAM, which is a STACK, not a load address.
constexpr uint32_t kCallerStackBase = 0x801fff00u;
constexpr uint32_t kCallerStackOffset = 0x40u;

constexpr uint32_t kMarkerAddress = 0x8003f000u;
constexpr uint32_t kMarkerValue = 0x5a5af00du;

constexpr uint32_t kFailed = 0xffffffffu;

// One real PS-X EXE: the 0x800-byte header plus its text payload, with recognisable instruction
// words so "the payload landed where the header said" is checkable byte for byte.
std::vector<uint8_t>
psxExe(uint32_t entry = kEntry, uint32_t globalPointer = kGlobalPointer, uint32_t stackBase = kHeaderStackBase) {
  std::vector<uint8_t> image(psx::cpu::kPsxExeHeaderBytes + kTextBytes, 0u);
  memcpy(image.data(), "PS-X EXE", 8);
  auto store = [&image](std::size_t offset, uint32_t value) {
    for (unsigned index = 0; index < 4; ++index) {
      image[offset + index] = static_cast<uint8_t>(value >> (index * 8));
    }
  };
  store(0x10, entry);
  store(0x14, globalPointer);
  store(0x18, kTextAddress);
  store(0x1c, kTextBytes);
  store(0x30, stackBase);
  store(0x34, 0u);
  for (uint32_t index = 0; index < kTextBytes / 4u; ++index) {
    store(psx::cpu::kPsxExeHeaderBytes + index * 4u, 0x24020000u + index); // addiu v0, zero, N
  }
  return image;
}

// The reader the CD device would answer with, over one constructed file, keyed by the exact ISO path
// the service derives from the guest's "device:path" string.
class FakeDisc {
public:
  void publish(const char *isoPath, std::vector<uint8_t> bytes) {
    files_.emplace_back(isoPath, std::move(bytes));
  }
  psx::hle::DiscFileReader reader() {
    return [this](const char *isoPath, std::vector<uint8_t> &out) {
      for (const auto &file : files_) {
        if (std::strcmp(file.first.c_str(), isoPath) == 0) {
          out = file.second;
          return true;
        }
      }
      return false;
    };
  }

private:
  std::vector<std::pair<std::string, std::vector<uint8_t>>> files_;
};

struct BiosCall {
  psx::cpu::ExecutionResult result;
  bool handled = false;
};

// One guest A0 call, exactly as the tail-jump stubs issue it: the A0 vector as the entry, the
// function number in r[9].
BiosCall callBiosA0(Core &core, uint32_t function) {
  core.r[kBiosFunctionRegister] = function;
  const psx::cpu::ExecutionResult result = psx::cpu::dispatchGuestHostService(core, kBiosTableA);
  return {result, true};
}

void writeFilename(Core &core, const char *text) {
  for (uint32_t index = 0; text[index] != 0; ++index) {
    core.mem_w8(kGuestFilename + index, static_cast<uint8_t>(text[index]));
  }
  core.mem_w8(kGuestFilename + static_cast<uint32_t>(std::strlen(text)), 0u);
}

// A fresh machine with the guest's registers armed for one LoadExec, and the CD reader the BIOS
// leaf uses replaced by `disc` exactly as `Game::wireRuntimeMembers` binds the real one — the seam
// is the shipping one, not a second dispatch path.
std::unique_ptr<Game> armedGuest(FakeDisc &disc, const char *filename, uint32_t stackBase, uint32_t stackOffset) {
  auto game = std::make_unique<Game>();
  Core &core = game->core;
  game->hle.loadExecReader = disc.reader();
  writeFilename(core, filename);
  core.r[kA0] = kGuestFilename;
  core.r[kA1] = stackBase;
  core.r[kA2] = stackOffset;
  core.r[kV0] = 0xdeadbeefu;
  core.r[kLinkRegister] = kCallerReturn;
  core.mem_w32(kMarkerAddress, kMarkerValue);
  return game;
}

// SUCCESS: the named executable is loaded, published as an image, and STARTED. Nothing about the
// BIOS arm of `dispatchGuestHostService` may hand this caller its r[31] back.
void test_load_exec_loads_and_starts_the_named_executable() {
  FakeDisc disc;
  disc.publish("\\S0\\CRASH.EXE;1", psxExe());
  auto game = armedGuest(disc, "cdrom:\\S0\\CRASH.EXE;1", kCallerStackBase, kCallerStackOffset);
  Core &core = game->core;

  const BiosCall call = callBiosA0(core, kLoadExec);
  CHECK(call.handled);
  CHECK(!call.result.detail.empty());
  // No exit reason at all is a fault; a serviced LoadExec resumes, either at the new program or at
  // its caller. Anything else means the leaf is not the BIOS's.
  CHECK(call.result.reason == psx::cpu::ExecutionExitReason::GuestReturn);
  // NOT the caller's r[31]: on success the BIOS starts the loaded program and never returns.
  CHECK_EQ(call.result.guestPc, kEntry);
  CHECK(core.r[kLinkRegister] != kCallerReturn);
  CHECK_EQ(core.pc, kEntry);
  CHECK_EQ(core.r[kV0], 0u);
  // The caller's stack wins over the header's, because stackbase/stackoffset are the point of a2/a3.
  CHECK_EQ(core.r[29], kCallerStackBase + kCallerStackOffset);
  CHECK_EQ(core.r[30], kCallerStackBase + kCallerStackOffset);
  CHECK_EQ(core.r[28], kGlobalPointer);
  // The payload landed where the header said, instruction for instruction.
  for (uint32_t index = 0; index < kTextBytes / 4u; ++index) {
    CHECK_EQ(core.mem_r32(kTextAddress + index * 4u), 0x24020000u + index);
  }
  // It is an EXECUTABLE PUBLICATION, not a raw copy: the entry resolves to a named resident image,
  // which is what the executor will translate the new program through.
  const auto identity = core.currentImageIdentity(kEntry);
  CHECK(identity.has_value());
  const auto described = core.imageCatalog().describe(*identity);
  CHECK(described.has_value());
  CHECK_STREQ(described->name.c_str(), "cdrom:\\S0\\CRASH.EXE;1");
  CHECK_EQ(core.imageCatalog().activeCount(), 1u);
}

// FAILURE 1: the name is not on the disc. That is the BIOS's -1 branch, and the caller keeps going.
void test_load_exec_refuses_a_file_that_is_not_on_the_disc() {
  FakeDisc disc;
  auto game = armedGuest(disc, "cdrom:\\S0\\NOTHERE.EXE;1", kCallerStackBase, 0u);
  Core &core = game->core;

  const BiosCall call = callBiosA0(core, kLoadExec);
  CHECK(call.handled);
  CHECK_EQ(call.result.reason, psx::cpu::ExecutionExitReason::GuestReturn);
  // Failure RETURNS. Its continuation is the leaf's own r[31].
  CHECK_EQ(call.result.guestPc, kCallerReturn);
  CHECK_EQ(core.r[kV0], kFailed);
  // A refused load changes nothing: no image, no bytes, no register the caller can see moved.
  CHECK_EQ(core.imageCatalog().activeCount(), 0u);
  CHECK(!core.currentImageIdentity(kEntry).has_value());
  CHECK_EQ(core.mem_r32(kMarkerAddress), kMarkerValue);
  CHECK(!core.executionControl().pending());
}

// FAILURE 2: the bytes are on the disc but are not a PS-X EXE. Refused at admission, so Core RAM,
// the image catalog and the guest's registers are exactly as they were.
void test_load_exec_refuses_bytes_that_are_not_a_psx_exe() {
  FakeDisc disc;
  std::vector<uint8_t> garbage(0x800 + kTextBytes, 0xa5u);
  disc.publish("\\S0\\CRASH.EXE;1", garbage);
  auto game = armedGuest(disc, "cdrom:\\S0\\CRASH.EXE;1", kCallerStackBase, 0u);
  Core &core = game->core;

  const BiosCall call = callBiosA0(core, kLoadExec);
  CHECK(call.handled);
  CHECK_EQ(call.result.reason, psx::cpu::ExecutionExitReason::GuestReturn);
  CHECK_EQ(call.result.guestPc, kCallerReturn);
  CHECK_EQ(core.r[kV0], kFailed);
  CHECK_EQ(core.imageCatalog().activeCount(), 0u);
  CHECK_EQ(core.mem_r32(kMarkerAddress), kMarkerValue);
  CHECK_EQ(core.r[29], 0u); // the stack the caller asked for was never installed
  CHECK(!core.executionControl().pending());
}

// FAILURE 3: a filename longer than the bound is CUT by the bounded read, and admitting the cut name
// would load a different file than the guest named.
void test_load_exec_refuses_a_filename_the_bounded_read_cut() {
  FakeDisc disc;
  const std::string tooLong = "cdrom:\\" + std::string(300, 'X') + ".EXE;1";
  auto game = armedGuest(disc, tooLong.c_str(), kCallerStackBase, 0u);
  Core &core = game->core;

  const BiosCall call = callBiosA0(core, kLoadExec);
  CHECK(call.handled);
  CHECK_EQ(call.result.reason, psx::cpu::ExecutionExitReason::GuestReturn);
  CHECK_EQ(call.result.guestPc, kCallerReturn);
  CHECK_EQ(core.r[kV0], kFailed);
  CHECK_EQ(core.imageCatalog().activeCount(), 0u);
}

// The header's own stack is what the loaded program runs on when the caller states none, and a
// header with no stack at all falls back to the conventional boot stack rather than leaving sp zero.
void test_an_unstated_stack_leaves_the_loaded_programs_own_stack_in_place() {
  {
    FakeDisc disc;
    disc.publish("\\MAIN.EXE;1", psxExe());
    auto game = armedGuest(disc, "cdrom:\\MAIN.EXE;1", 0u, 0u);
    const BiosCall call = callBiosA0(game->core, kLoadExec);
    CHECK_EQ(call.result.guestPc, kEntry);
    CHECK_EQ(game->core.r[29], kHeaderStackBase);
  }
  {
    FakeDisc disc;
    disc.publish("\\MAIN.EXE;1", psxExe(kEntry, kGlobalPointer, 0u));
    auto game = armedGuest(disc, "cdrom:\\MAIN.EXE;1", 0u, 0u);
    const BiosCall call = callBiosA0(game->core, kLoadExec);
    CHECK_EQ(call.result.guestPc, kEntry);
    CHECK(game->core.r[29] != 0u);
  }
}

// A refusal NEVER leaves the machine half-changed. The stated stack is resolved before a byte moves,
// so a caller that asked for a stack outside guest memory gets -1 with an untouched machine — not a
// published payload and a resident image identity the guest can only escape by dying.
void test_a_refused_load_leaves_no_image_and_no_bytes_behind() {
  FakeDisc disc;
  disc.publish("\\S0\\CRASH.EXE;1", psxExe());
  // A stack whose base+offset runs past the top of the address space.
  auto game = armedGuest(disc, "cdrom:\\S0\\CRASH.EXE;1", 0xfffff000u, 0x2000u);
  Core &core = game->core;

  const BiosCall call = callBiosA0(core, kLoadExec);
  CHECK_EQ(call.result.reason, psx::cpu::ExecutionExitReason::GuestReturn);
  CHECK_EQ(call.result.guestPc, kCallerReturn);
  CHECK_EQ(core.r[kV0], kFailed);
  CHECK_EQ(core.imageCatalog().activeCount(), 0u);
  CHECK(!core.currentImageIdentity(kEntry).has_value());
  // The payload's own first instruction is nowhere in RAM: the refusal happened BEFORE admission.
  CHECK(core.mem_r32(kTextAddress) != 0x24020000u);
  CHECK_EQ(core.r[29], 0u); // the impossible stack was never installed
  CHECK_EQ(core.r[28], 0u); // nor was a gp
  CHECK(!core.executionControl().pending());
}

// 0x51 is CLAIMED and 0x50 is not: answering LoadExec must not have turned its neighbour into a
// serviced leaf, and the neighbour's answer is still the pre-existing typed fault.
void test_loadexec_did_not_claim_a_neighbouring_leaf() {
  FakeDisc disc;
  auto game = armedGuest(disc, "cdrom:\\S0\\CRASH.EXE;1", 0u, 0u);
  Core &core = game->core;

  const BiosCall neighbour = callBiosA0(core, kLoadExec - 1u);
  CHECK_EQ(neighbour.result.reason, psx::cpu::ExecutionExitReason::Fault);
  CHECK_STREQ(neighbour.result.detail.c_str(), "unimplemented BIOS service");
  CHECK_EQ(core.r[kV0], 0xdeadbeefu);

  // With nothing published on the disc, 0x51 is serviced and answers -1 rather than faulting.
  const BiosCall served = callBiosA0(core, kLoadExec);
  CHECK_EQ(served.result.reason, psx::cpu::ExecutionExitReason::GuestReturn);
  CHECK_EQ(served.result.guestPc, kCallerReturn);
  CHECK_EQ(core.r[kV0], kFailed);
}

} // namespace

int main() {
  RUN(load_exec_loads_and_starts_the_named_executable);
  RUN(load_exec_refuses_a_file_that_is_not_on_the_disc);
  RUN(load_exec_refuses_bytes_that_are_not_a_psx_exe);
  RUN(load_exec_refuses_a_filename_the_bounded_read_cut);
  RUN(an_unstated_stack_leaves_the_loaded_programs_own_stack_in_place);
  RUN(a_refused_load_leaves_no_image_and_no_bytes_behind);
  RUN(loadexec_did_not_claim_a_neighbouring_leaf);
  return pt_summary();
}