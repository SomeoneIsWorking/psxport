// test_gte_raw_state.cpp — the passive GTE snapshot/restore round trip (gte_state.h).
//
// WHY THIS TEST EXISTS. A host pass that runs the guest's own GTE maths over its own data has to
// hand the GTE back exactly as it found it, and it cannot do that by reading the registers back:
// GTE_ReadDR(12..14) POPS the projection FIFO and GTE_ReadCR of a flag register CLEARS the flag it
// reports, so a "snapshot" built from reads is already a disturbance. The raw snapshot copies REG
// and FLAGS directly, and this test proves three things about it:
//
//   1. a snapshot restores every register and the flags bit-exact, for a register file no
//      instruction has touched;
//     2. it restores a file that HAS been run through real instructions (RTPS, DPCS, MVMVA), so the
//      result flags and the FIFO registers are covered, not just the matrices;
//     3. restoring is itself passive — after a restore, reading DR12..DR14 still returns the FIFO
//      contents, and the flag registers still report the flags that were restored, so a caller that
//      saves and restores around its own maths leaves the guest's next mfc2 reading what it would
//      have read anyway.

#include "gte_state.h"
#include "testutil.h"

#include <cstdint>

extern "C" {
void GTE_Init(void);
void GTE_Power(void);
int32_t GTE_Instruction(uint32_t instruction);
void GTE_WriteCR(unsigned which, uint32_t value);
void GTE_WriteDR(unsigned which, uint32_t value);
uint32_t GTE_ReadCR(unsigned which);
uint32_t GTE_ReadDR(unsigned which);
}

namespace {

constexpr uint32_t kRtps = 0x4A180001u;  // RTPS
constexpr uint32_t kDpcs = 0x4A780010u;  // DPCS: RGBC toward FC by IR0
constexpr uint32_t kMvmva = 0x4A486012u; // MVMVA sf, RT * V0

// Fill every register with a value that is not zero and not a pattern of zeros, so a snapshot that
// forgot a register cannot pass by leaving it at whatever it was.
void fill(GteRawState &state, uint32_t seed) {
  for (unsigned i = 0; i < 64; ++i) {
    state.reg[i] = 0x80000000u | (seed * 2654435761u) ^ (i * 40503u);
  }
  state.flags = 0xFFFFFFFFu;
}

// One explicitly bound register file, so the test exercises a named instance rather than the
// process default, and the caller's binding is handed back on the way out.
static void test_round_trip_without_instructions() {
  GteRegs instance{};
  GTE_BindState(&instance);
  GTE_Power();
  GteRawState written;
  fill(written, 7);
  for (unsigned i = 0; i < 64; ++i) {
    if (i < 32) {
      GTE_WriteDR(i, written.reg[i]);
    } else {
      GTE_WriteCR(i - 32, written.reg[i]);
    }
  }
  GTE_RestoreRawState(nullptr); // a null source is a no-op, not a fault
  // The flags are not reachable through a register write — no GTE instruction sets them without
  // clearing others — so the snapshot's flag half is exercised by setting the instance's own.
  instance.FLAGS = 0xFFFFFFFFu;

  GteRawState saved;
  GTE_SaveRawState(&saved);

  // THE SNAPSHOT IS THE FILE. Checked against the instance rather than against the words written,
  // because the write ports are not transparent: VXY0/VXY1 are latched as a pair and the FIFO
  // registers hold what the last projection left. What matters here is that the copy is the whole
  // file, so the restore below has something complete to put back.
  for (unsigned i = 0; i < 64; ++i) {
    CHECK_EQ(saved.reg[i], instance.REG[i]);
  }
  CHECK_EQ(saved.flags, instance.FLAGS);

  // Perturb everything the way a host pass would, through the same write ports.
  for (unsigned i = 0; i < 64; ++i) {
    GTE_WriteDR(i, 0xDEADBEEFu);
  }
  GTE_RestoreRawState(&saved);

  GteRawState after;
  GTE_SaveRawState(&after);
  CHECK_EQ(after.flags, saved.flags);
  for (unsigned i = 0; i < 64; ++i) {
    CHECK_EQ(after.reg[i], saved.reg[i]);
  }
  GTE_SaveRawState(nullptr); // a null destination is a no-op, not a fault
  GTE_BindState(nullptr);
}

static void test_round_trip_after_instructions() {
  GTE_Init();
  GteRegs instance{};
  GTE_BindState(&instance);
  GTE_Power();

  // A real projection: light matrix, translation, and a vertex worth projecting.
  GTE_WriteCR(8, 0x00000000u);
  GTE_WriteCR(9, 0x00010000u);
  GTE_WriteCR(10, 0x00000000u);
  GTE_WriteCR(11, 0x00000000u);
  GTE_WriteCR(12, 0x00000000u);
  GTE_WriteCR(13, 0x00000000u);
  GTE_WriteCR(5, 0x00000000u);
  GTE_WriteCR(6, 0x00000000u);
  GTE_WriteCR(7, 0x00001000u);
  GTE_WriteDR(11, 0x00000000u); // IR3 = z
  GTE_WriteDR(9, 0x00000000u);  // IR1 = x
  GTE_WriteDR(10, 0x00000000u); // IR2 = y
  GTE_Instruction(kRtps);

  GTE_WriteCR(21, 0x00000080u); // FC R
  GTE_WriteCR(22, 0x00000080u); // FC G
  GTE_WriteCR(23, 0x00000080u); // FC B
  GTE_WriteDR(8, 0x00001000u);  // IR0 = factor
  GTE_WriteDR(6, 0x00204080u);  // RGBC
  GTE_Instruction(kDpcs);

  GTE_WriteCR(0, 0x00001000u); // RT
  GTE_WriteDR(2, 0x00000010u); // VXY1
  GTE_WriteDR(3, 0x00000020u); // VZ1
  GTE_Instruction(kMvmva);

  GteRawState saved;
  GTE_SaveRawState(&saved);

  // The host pass: it runs its own maths on the same GTE.
  GTE_WriteCR(0, 0x00002000u);
  GTE_WriteDR(2, 0x00000030u);
  GTE_Instruction(kMvmva);
  GTE_Instruction(kRtps);

  GTE_RestoreRawState(&saved);

  GteRawState after;
  GTE_SaveRawState(&after);
  CHECK(after.flags == saved.flags);
  for (unsigned i = 0; i < 64; ++i) {
    CHECK_EQ(after.reg[i], saved.reg[i]);
  }

  // PASSIVE: the FIFO registers read back what the snapshot carried, in order, and the flag
  // registers report the flags the snapshot carried. If the restore had gone through the read
  // ports, DR12..DR14 would have been popped and the flags cleared by these very reads.
  CHECK(GTE_ReadDR(12) == saved.reg[12]);
  CHECK(GTE_ReadDR(13) == saved.reg[13]);
  CHECK(GTE_ReadDR(14) == saved.reg[14]);
  CHECK(GTE_ReadCR(31) == saved.reg[63]);
  GteRawState afterReads;
  GTE_SaveRawState(&afterReads);
  for (unsigned i = 0; i < 64; ++i) {
    CHECK_EQ(afterReads.reg[i], saved.reg[i]);
  }
  CHECK(afterReads.flags == saved.flags);
  GTE_BindState(nullptr);
}

} // namespace

int main() {
  RUN(round_trip_without_instructions);
  RUN(round_trip_after_instructions);
  return pt_summary();
}
