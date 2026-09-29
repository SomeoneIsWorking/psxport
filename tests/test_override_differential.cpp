// test_override_differential.cpp — the per-function override differential must say MATCH for a correct
// override and MISMATCH, naming the first difference, for every way an override can be wrong; must say
// INCOMPARABLE, never match, when a path does something it cannot replay; and must leave the run on the
// ORIGINAL's state with no stale translated code.
//
// NEGATIVE FIRST. Every wrong override below is a real failure class a hand-written or generated override
// has: the wrong result register, a store it forgot, a callee-saved register it clobbered, a store the
// original never made, device traffic the original never made, and a BIOS service. Each must come out
// MISMATCH or INCOMPARABLE with the named difference, or the instrument is not a gate.
//
// Every case runs through the PRODUCT route: a guest caller `jal`s the guest function, the executor's
// host-dispatch boundary resolves the installed override, and `NativeDispatcher::invoke` hands the call
// to the differential. Nothing here calls the differential's internals directly.
#include "testutil.h"

#include "core.h"
#include "game.h"
#include "game_runtime.h"
#include "image_identity.h"
#include "lightrec_executor.h"
#include "native_dispatch.h"
#include "override_differential.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace {

using psx::cpu::DifferentialKeyStats;
using psx::cpu::DifferentialVerdict;

constexpr std::uint32_t rZero = 0, rV0 = 2, rA0 = 4, rA1 = 5, rA2 = 6, rT0 = 8, rT1 = 9, rS0 = 16, rSp = 29, rRa = 31;

constexpr std::uint32_t special(std::uint32_t rs, std::uint32_t rt, std::uint32_t rd, std::uint32_t funct) {
  return (rs << 21) | (rt << 16) | (rd << 11) | funct;
}
constexpr std::uint32_t addu(std::uint32_t rd, std::uint32_t rs, std::uint32_t rt) {
  return special(rs, rt, rd, 0x21u);
}
constexpr std::uint32_t immediate(std::uint32_t op, std::uint32_t rt, std::uint32_t rs, std::int32_t imm) {
  return (op << 26) | (rs << 21) | (rt << 16) | (static_cast<std::uint32_t>(imm) & 0xFFFFu);
}
constexpr std::uint32_t addiu(std::uint32_t rt, std::uint32_t rs, std::int32_t imm) {
  return immediate(0x09u, rt, rs, imm);
}
constexpr std::uint32_t lui(std::uint32_t rt, std::uint32_t imm) {
  return immediate(0x0Fu, rt, rZero, static_cast<std::int32_t>(imm));
}
constexpr std::uint32_t lw(std::uint32_t rt, std::uint32_t base, std::int32_t off) {
  return immediate(0x23u, rt, base, off);
}
constexpr std::uint32_t sw(std::uint32_t rt, std::uint32_t base, std::int32_t off) {
  return immediate(0x2Bu, rt, base, off);
}
constexpr std::uint32_t jal(std::uint32_t target) {
  return 0x0C000000u | ((target >> 2u) & 0x03FFFFFFu);
}
constexpr std::uint32_t jrRa() {
  return special(rRa, rZero, rZero, 0x08u);
}
constexpr std::uint32_t syscallInstruction() {
  return 0x0000000Cu;
}
constexpr std::uint32_t nop() {
  return 0;
}

// The guest program. `kCaller` saves `ra`, calls the function under test and returns; the function
// under test computes a0+a1 into v0 through callee-saved s0 (so it builds a stack frame and leaves its
// saved s0 below the caller's sp) and stores the sum to [a2].
constexpr std::uint32_t kCaller = 0x00010000u;
constexpr std::uint32_t kFunction = 0x00010100u;
constexpr std::uint32_t kPatchTarget = 0x00010200u; // a second function whose code the original may patch
constexpr std::uint32_t kOuterReturn = 0x00010F00u;
constexpr std::uint32_t kImageEnd = 0x00011000u;
constexpr std::uint32_t kResult = 0x00080000u;     // where the function stores its sum
constexpr std::uint32_t kExtraStore = 0x00080010u; // where a wrong override adds a store
constexpr std::uint32_t kStackTop = 0x801FFF00u;
constexpr std::uint32_t kIrqMask = 0x1F801074u; // I_MASK: a device register whose read has no side effect
constexpr std::uint32_t kArgA = 0x1234u;
constexpr std::uint32_t kArgB = 0x0101u;
constexpr std::uint32_t kSum = kArgA + kArgB;
constexpr std::uint32_t kCallerS0 = 0x5A5A0001u;

constexpr std::array<std::uint32_t, 7> kCallerCode = {
    addiu(rSp, rSp, -16),
    sw(rRa, rSp, 12),
    jal(kFunction),
    nop(),
    lw(rRa, rSp, 12),
    jrRa(),
    addiu(rSp, rSp, 16),
};

constexpr std::array<std::uint32_t, 8> kSumCode = {
    addiu(rSp, rSp, -8),
    sw(rS0, rSp, 4),
    addu(rS0, rA0, rA1),
    addu(rV0, rS0, rZero),
    sw(rV0, rA2, 0),
    lw(rS0, rSp, 4),
    jrRa(),
    addiu(rSp, rSp, 8),
};

// The same contract, plus a device read the result depends on: v0 = a0 + a1 + I_MASK.
constexpr std::array<std::uint32_t, 7> kDeviceCode = {
    lui(rT1, 0x1F80u),
    lw(rT0, rT1, 0x1074),
    addu(rV0, rA0, rA1),
    addu(rV0, rV0, rT0),
    sw(rV0, rA2, 0),
    jrRa(),
    nop(),
};

// An original that enters a critical section through the kernel: a syscall the journal cannot replay.
constexpr std::array<std::uint32_t, 6> kSyscallCode = {
    addiu(rA0, rZero, 1),
    syscallInstruction(),
    addu(rV0, rA0, rZero),
    sw(rV0, rA2, 0),
    jrRa(),
    nop(),
};

// kPatchTarget returns a constant; the patching original rewrites that constant's instruction to 2.
constexpr std::uint32_t kPatchedBefore = addiu(rV0, rZero, 1);
constexpr std::uint32_t kPatchedAfter = addiu(rV0, rZero, 2);
// The patched instruction is the block's FIRST word on purpose. Lightrec revokes a block only when the
// code-LUT entry at its start is cleared, so a write to an interior word (here, the delay slot) is NOT
// honoured by `lightrec_invalidate` at all — a pre-existing S015 defect recorded in
// docs/issues/0050-centralize-lightrec-code-invalidation.md with this fixture as its reproduction.
constexpr std::array<std::uint32_t, 3> kPatchTargetCode = {kPatchedBefore, jrRa(), nop()};
// v0 = a0 + a1; *a2 = v0; and *(a1_target) = the patched instruction, with the target passed in t1 so
// the optimizer cannot see a constant store into code.
constexpr std::array<std::uint32_t, 7> kPatchingCode = {
    addu(rV0, rA0, rA1),
    sw(rV0, rA2, 0),
    lw(rT0, rA2, 4),
    nop(), // MIPS I load delay: the store must see the loaded t0, not the stale one

    sw(rT0, rT1, 0),
    jrRa(),
    nop(),
};

class Runtime final : public GameRuntime {
public:
  void *createContext(Core &) override {
    return nullptr;
  }
  void destroyContext(void *) override {}
  void registerOverrides(Game &) override {}
  void bootInit(Core &) override {}
  RenderCapabilities renderCapabilities() const override {
    return RenderCapabilities::direct();
  }
  bool guestVramIsPicture(const Game &) const override {
    return false;
  }
};

int g_nativeCalls = 0;

void correctSum(Core *core) {
  ++g_nativeCalls;
  core->r[rV0] = core->r[rA0] + core->r[rA1];
  core->mem_w32(core->r[rA2], core->r[rV0]);
}
void wrongResult(Core *core) {
  ++g_nativeCalls;
  core->r[rV0] = core->r[rA0] + core->r[rA1] + 1u;
  core->mem_w32(core->r[rA2], core->r[rA0] + core->r[rA1]);
}
void missingStore(Core *core) {
  ++g_nativeCalls;
  core->r[rV0] = core->r[rA0] + core->r[rA1];
}
void clobberedS0(Core *core) {
  ++g_nativeCalls;
  core->r[rV0] = core->r[rA0] + core->r[rA1];
  core->mem_w32(core->r[rA2], core->r[rV0]);
  core->r[rS0] = 0xDEADBEEFu;
}
void extraStore(Core *core) {
  ++g_nativeCalls;
  correctSum(core);
  --g_nativeCalls;
  core->mem_w32(kExtraStore, 0x0BADF00Du);
}
void extraDeviceWrite(Core *core) {
  ++g_nativeCalls;
  correctSum(core);
  --g_nativeCalls;
  core->mem_w32(kIrqMask, 0u);
}
void biosCall(Core *core) {
  ++g_nativeCalls;
  correctSum(core);
  --g_nativeCalls;
  core->r[rT1] = 0x3Cu; // A0:3C putchar — any BIOS service; the journal must withhold it
  psx::cpu::dispatchGuestToReturn(*core, 0xA0u, psx::cpu::ExecutionBudget::fromCycles(1000), "test bios call");
}
void correctDeviceSum(Core *core) {
  ++g_nativeCalls;
  core->r[rV0] = core->r[rA0] + core->r[rA1] + core->mem_r32(kIrqMask);
  core->mem_w32(core->r[rA2], core->r[rV0]);
}
void correctPatching(Core *core) {
  ++g_nativeCalls;
  // Executes the patch target from the RESTORED entry bytes first, so Lightrec holds a block translated
  // from the unpatched instruction when the differential puts the original's bytes back.
  const std::uint32_t target = core->r[rT1];
  psx::cpu::dispatchGuestToReturn(
      *core, kPatchTarget, psx::cpu::ExecutionBudget::fromCycles(100000), "test patch-target warm");
  core->r[rV0] = core->r[rA0] + core->r[rA1];
  core->mem_w32(core->r[rA2], core->r[rV0]);
  core->mem_w32(target, core->mem_r32(core->r[rA2] + 4u));
}

template <std::size_t N> void writeCode(Core &core, std::uint32_t at, const std::array<std::uint32_t, N> &code) {
  for (std::size_t i = 0; i < N; ++i) {
    core.mem_w32(at + static_cast<std::uint32_t>(i) * 4u, code[i]);
  }
}

std::string reportPath(const char *name) {
  return std::string("override_differential_test/") + name + ".json";
}

struct Fixture {
  Runtime runtime;
  std::unique_ptr<Game> game;
  psx::cpu::NativeKey key{};

  template <std::size_t N>
  Fixture(const char *name,
          const std::array<std::uint32_t, N> &functionCode,
          psx::cpu::NativeFunction native,
          std::string_view selectors = "under-test",
          std::int64_t first = 16,
          std::int64_t every = 64) {
    psxport_install_game(runtime);
    game = std::make_unique<Game>();
    Core &core = game->core;
    const psx::cpu::ImageIdentity image =
        core.imageCatalog().activate("override-diff", {kCaller, kImageEnd}, 0x4F444946u);
    writeCode(core, kCaller, kCallerCode);
    writeCode(core, kFunction, functionCode);
    writeCode(core, kPatchTarget, kPatchTargetCode);
    core.mem_w32(kOuterReturn, jrRa());
    core.mem_w32(kOuterReturn + 4u, nop());
    key = {image, kFunction};
    if (!core.nativeDispatcher().install({key, "under-test", native})) {
      std::fprintf(stderr, "test setup REFUSED: override did not install\n");
      std::abort();
    }
    core.nativeDispatcher().attachDifferential(std::make_unique<psx::cpu::OverrideDifferential>(
        core,
        psx::cpu::overrideDifferentialConfigFrom(
            selectors, first, every, psx::cpu::kDefaultDifferentialDeadStackBytes, reportPath(name))));
    g_nativeCalls = 0;
  }

  Core &core() {
    return game->core;
  }

  // One guest call of the function under test, through the product dispatch route.
  void call(std::uint32_t a = kArgA, std::uint32_t b = kArgB) {
    Core &c = core();
    c.r[rA0] = a;
    c.r[rA1] = b;
    c.r[rA2] = kResult;
    c.r[rT1] = kPatchTarget;
    c.r[rS0] = kCallerS0;
    c.r[rSp] = kStackTop;
    c.r[rRa] = kOuterReturn;
    psx::cpu::dispatchGuestToReturn(c, kCaller, psx::cpu::ExecutionBudget::fromCycles(1'000'000), "test caller");
  }

  const DifferentialKeyStats &stats() {
    const psx::cpu::OverrideDifferential *differential = core().nativeDispatcher().differential();
    if (differential == nullptr || differential->keys().size() != 1) {
      std::fprintf(stderr, "test REFUSED: expected exactly one shadowed key\n");
      std::abort();
    }
    return differential->keys().front();
  }
};

std::string readFile(const std::string &path) {
  std::ifstream in(path);
  std::stringstream text;
  text << in.rdbuf();
  return text.str();
}

bool contains(const std::string &text, const std::string &needle) {
  return text.find(needle) != std::string::npos;
}

// POSITIVE: a correct override matches, the native really ran, and the continued run holds the
// ORIGINAL's results — the sum in v0 and in RAM, and the caller's s0 and sp intact.
static void test_a_correct_override_matches_and_the_run_continues_from_the_original(void) {
  Fixture f("correct", kSumCode, &correctSum);
  f.call();
  const DifferentialKeyStats &stats = f.stats();
  CHECK_EQ(stats.callsSeen, 1u);
  CHECK_EQ(stats.sampled, 1u);
  CHECK_EQ(stats.match, 1u);
  CHECK_EQ(stats.mismatch, 0u);
  CHECK_EQ(stats.incomparable, 0u);
  CHECK_EQ(g_nativeCalls, 1);
  CHECK_EQ(f.core().r[rV0], kSum);
  CHECK_EQ(f.core().mem_r32(kResult), kSum);
  CHECK_EQ(f.core().r[rS0], kCallerS0);
  CHECK_EQ(f.core().r[rSp], kStackTop);
  // The original saved s0 below the caller's sp and the native did not: that residue is the dead-stack
  // window working, counted rather than silently dropped.
  CHECK(stats.deadStackBytesIgnored > 0u);
}

static void test_a_wrong_result_register_is_a_mismatch_naming_v0(void) {
  Fixture f("wrong-v0", kSumCode, &wrongResult);
  f.call();
  const DifferentialKeyStats &stats = f.stats();
  CHECK_EQ(stats.mismatch, 1u);
  CHECK_EQ(stats.match, 0u);
  CHECK(stats.firstMismatch.has_value());
  CHECK(stats.firstMismatch->difference->what == "register v0");
  CHECK(stats.firstMismatch->difference->original == "0x00001335");
  CHECK(stats.firstMismatch->difference->native == "0x00001336");
  // The run continues from the original whatever the override did.
  CHECK_EQ(f.core().r[rV0], kSum);
}

static void test_a_missing_store_is_a_mismatch_naming_the_ram_range(void) {
  Fixture f("missing-store", kSumCode, &missingStore);
  f.call();
  const DifferentialKeyStats &stats = f.stats();
  CHECK_EQ(stats.mismatch, 1u);
  CHECK(stats.firstMismatch->difference->what == "ram [0x00080000,0x00080002)");
  CHECK(stats.firstMismatch->difference->original == "3513");
  CHECK(stats.firstMismatch->difference->native == "0000");
  CHECK_EQ(f.core().mem_r32(kResult), kSum);
}

static void test_a_clobbered_callee_saved_register_is_a_mismatch_naming_s0(void) {
  Fixture f("clobbered-s0", kSumCode, &clobberedS0);
  f.call();
  const DifferentialKeyStats &stats = f.stats();
  CHECK_EQ(stats.mismatch, 1u);
  CHECK(stats.firstMismatch->difference->what == "register s0");
  CHECK(stats.firstMismatch->difference->native == "0xDEADBEEF");
  CHECK_EQ(f.core().r[rS0], kCallerS0);
}

static void test_an_extra_store_is_a_mismatch_and_does_not_survive(void) {
  Fixture f("extra-store", kSumCode, &extraStore);
  f.call();
  const DifferentialKeyStats &stats = f.stats();
  CHECK_EQ(stats.mismatch, 1u);
  CHECK(stats.firstMismatch->difference->what == "ram [0x00080010,0x00080014)");
  CHECK(stats.firstMismatch->difference->native == "0DF0AD0B");
  CHECK_EQ(f.core().mem_r32(kExtraStore), 0u);
}

static void test_device_traffic_the_original_never_made_is_a_mismatch_and_is_swallowed(void) {
  Fixture f("extra-device-write", kSumCode, &extraDeviceWrite);
  f.game->hle.i_mask = 0x55u;
  f.call();
  const DifferentialKeyStats &stats = f.stats();
  CHECK_EQ(stats.mismatch, 1u);
  CHECK(stats.firstMismatch->difference->what == "side effect #0 (logs of 0 and 1 effects)");
  // The shadow path's write never reached the device.
  CHECK_EQ(f.game->hle.i_mask, 0x55u);
}

// POSITIVE with device traffic: the original's device read is live, the native's is REPLAYED from the
// journal (so it sees the same value even though the device is not asked again), and the logs match.
static void test_an_override_replaying_the_originals_device_read_matches(void) {
  Fixture f("device-read", kDeviceCode, &correctDeviceSum);
  f.game->hle.i_mask = 0x55u;
  f.call();
  const DifferentialKeyStats &stats = f.stats();
  CHECK_EQ(stats.match, 1u);
  CHECK_EQ(stats.mismatch, 0u);
  CHECK_EQ(f.core().r[rV0], kSum + 0x55u);
}

static void test_a_native_bios_service_is_incomparable_never_a_match(void) {
  Fixture f("native-bios", kSumCode, &biosCall);
  f.call();
  const DifferentialKeyStats &stats = f.stats();
  CHECK_EQ(stats.incomparable, 1u);
  CHECK_EQ(stats.match, 0u);
  CHECK_EQ(stats.mismatch, 0u);
  CHECK_EQ(stats.incomparableByReason.size(), 1u);
  CHECK(contains(stats.incomparableByReason.begin()->first, "native path performed bios-call @0x000000A0"));
}

// An original that performs an unreplayable service is incomparable and the NATIVE IS NOT RUN, because
// running it would need the service again; the run keeps the original's own result.
static void test_an_original_syscall_is_incomparable_and_the_native_is_not_run(void) {
  Fixture f("original-syscall", kSyscallCode, &correctSum);
  f.call();
  const DifferentialKeyStats &stats = f.stats();
  CHECK_EQ(stats.incomparable, 1u);
  CHECK_EQ(g_nativeCalls, 0);
  CHECK(contains(stats.incomparableByReason.begin()->first, "original path performed syscall"));
  CHECK_EQ(f.core().mem_r32(kResult), 1u);
}

static void test_sampling_takes_the_first_n_then_every_kth(void) {
  Fixture f("sampling", kSumCode, &correctSum, "under-test", 2, 3);
  for (int i = 0; i < 7; ++i) {
    f.call();
  }
  const DifferentialKeyStats &stats = f.stats();
  CHECK_EQ(stats.callsSeen, 7u);
  CHECK_EQ(stats.sampled, 4u); // calls 1, 2, 3, 6
  CHECK_EQ(stats.match, 4u);
  CHECK_EQ(g_nativeCalls, 7); // 4 shadowed + 3 dispatched normally
}

// A selector that never samples a call is a FAILURE in the report, not a quiet pass.
static void test_a_requested_override_with_zero_samples_is_a_failure(void) {
  Fixture f("zero-samples", kSumCode, &correctSum, "under-test,never-installed");
  f.call();
  const psx::cpu::OverrideDifferential &differential = *f.core().nativeDispatcher().differential();
  CHECK_EQ(differential.sampledFor(differential.config().selectors[1]), 0u);
  CHECK(differential.writeReport(true));
  const std::string report = readFile(reportPath("zero-samples"));
  CHECK(contains(report, "\"schema\": \"psxport.override-differential/1\""));
  CHECK(contains(report, "\"complete\": true"));
  CHECK(contains(report, "selector 'never-installed' sampled 0 calls"));
  CHECK(!contains(report, "selector 'under-test' sampled 0"));
}

static void test_an_address_selector_selects_the_override_at_that_entry(void) {
  Fixture f("address-selector", kSumCode, &correctSum, "0x00010100");
  f.call();
  CHECK_EQ(f.stats().match, 1u);
}

// RESTORED CODE IS NEVER STALE. The original patches kPatchTarget's instruction; the native (correct)
// executes kPatchTarget from the restored, UNPATCHED bytes, so Lightrec holds a block translated from
// them. Continuing from the original puts the patched bytes back — and a later call must execute them.
static void test_restoring_the_originals_code_bytes_invalidates_the_translated_block(void) {
  Fixture f("code-restore", kPatchingCode, &correctPatching);
  f.core().mem_w32(kResult + 4u, kPatchedAfter);
  f.core().r[rRa] = kOuterReturn;
  psx::cpu::dispatchGuestToReturn(
      f.core(), kPatchTarget, psx::cpu::ExecutionBudget::fromCycles(100000), "test patch-target before");
  CHECK_EQ(f.core().r[rV0], 1u);
  const std::uint64_t invalidationsBefore = f.core().lightrecExecutor().counters().invalidations;
  f.call();
  const DifferentialKeyStats &stats = f.stats();
  CHECK_EQ(stats.match, 1u);
  CHECK_EQ(f.core().mem_r32(kPatchTarget), kPatchedAfter);
  CHECK(stats.restoredRanges >= 2u);
  CHECK(f.core().lightrecExecutor().counters().invalidations - invalidationsBefore >= stats.restoredRanges);
  f.core().r[rRa] = kOuterReturn;
  psx::cpu::dispatchGuestToReturn(
      f.core(), kPatchTarget, psx::cpu::ExecutionBudget::fromCycles(100000), "test patch-target after");
  CHECK_EQ(f.core().r[rV0], 2u);
}

} // namespace

int main() {
  RUN(a_correct_override_matches_and_the_run_continues_from_the_original);
  RUN(a_wrong_result_register_is_a_mismatch_naming_v0);
  RUN(a_missing_store_is_a_mismatch_naming_the_ram_range);
  RUN(a_clobbered_callee_saved_register_is_a_mismatch_naming_s0);
  RUN(an_extra_store_is_a_mismatch_and_does_not_survive);
  RUN(device_traffic_the_original_never_made_is_a_mismatch_and_is_swallowed);
  RUN(an_override_replaying_the_originals_device_read_matches);
  RUN(a_native_bios_service_is_incomparable_never_a_match);
  RUN(an_original_syscall_is_incomparable_and_the_native_is_not_run);
  RUN(sampling_takes_the_first_n_then_every_kth);
  RUN(a_requested_override_with_zero_samples_is_a_failure);
  RUN(an_address_selector_selects_the_override_at_that_entry);
  RUN(restoring_the_originals_code_bytes_invalidates_the_translated_block);
  return pt_summary();
}
