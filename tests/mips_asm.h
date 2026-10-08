// mips_asm.h — the handful of R3000 instruction ENCODERS psxport's synthetic-guest tests need.
//
// WHY THIS IS A HEADER AND NOT A SECOND COPY. Two tests assemble guest words inline (the override
// differential and the save-state determinism check), and a hand-decoded J-type or branch target is
// exactly the mistake this workspace has already made once with real effect: a listing hand-decoded
// as if a J target were PC-relative produced five wrong call targets out of seven, every one of them
// a plausible guest address, so it survived review. One encoder, one place to get it right.
//
// `jal`'s target field is absolute; a branch's displacement is relative to the instruction AFTER the
// branch, which is why `bne` takes the address the branch itself is assembled at. Getting that
// argument wrong yields a displacement that is perfectly well-formed and lands somewhere else.
#ifndef PSXPORT_TESTS_MIPS_ASM_H
#define PSXPORT_TESTS_MIPS_ASM_H

#include <cstdint>

namespace psx::test::mips {

constexpr std::uint32_t kZero = 0, kV0 = 2, kA0 = 4, kA1 = 5, kA2 = 7, kT0 = 8, kT1 = 9, kT2 = 10, kS0 = 16, kS1 = 17,
                        kS2 = 18, kSp = 29, kRa = 31;

constexpr std::uint32_t special(std::uint32_t rs, std::uint32_t rt, std::uint32_t rd, std::uint32_t funct) {
  return (rs << 21) | (rt << 16) | (rd << 11) | funct;
}
constexpr std::uint32_t addu(std::uint32_t rd, std::uint32_t rs, std::uint32_t rt) {
  return special(rs, rt, rd, 0x21u);
}
constexpr std::uint32_t sw(std::uint32_t rt, std::uint32_t base, std::int32_t off) {
  return (0x2Bu << 26) | (base << 21) | (rt << 16) | (static_cast<std::uint32_t>(off) & 0xFFFFu);
}
constexpr std::uint32_t lw(std::uint32_t rt, std::uint32_t base, std::int32_t off) {
  return (0x23u << 26) | (base << 21) | (rt << 16) | (static_cast<std::uint32_t>(off) & 0xFFFFu);
}
constexpr std::uint32_t addiu(std::uint32_t rt, std::uint32_t rs, std::int32_t imm) {
  return (0x09u << 26) | (rs << 21) | (rt << 16) | (static_cast<std::uint32_t>(imm) & 0xFFFFu);
}
constexpr std::uint32_t lui(std::uint32_t rt, std::uint32_t imm) {
  return (0x0Fu << 26) | (rt << 16) | (static_cast<std::uint32_t>(imm) & 0xFFFFu);
}
constexpr std::uint32_t ori(std::uint32_t rt, std::uint32_t rs, std::uint32_t imm) {
  return (0x0Du << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFFu);
}
constexpr std::uint32_t jal(std::uint32_t target) {
  return 0x0C000000u | ((target >> 2u) & 0x03FFFFFFu);
}
constexpr std::uint32_t jr(std::uint32_t rs) {
  return special(rs, kZero, kZero, 0x08u);
}
constexpr std::uint32_t jrRa() {
  return jr(kRa);
}
constexpr std::uint32_t bne(std::uint32_t rs, std::uint32_t rt, std::uint32_t atBranch, std::uint32_t target) {
  const std::uint32_t displacement = (target - (atBranch + 4u)) >> 2u;
  return (0x05u << 26) | (rs << 21) | (rt << 16) | (displacement & 0xFFFFu);
}
constexpr std::uint32_t nop() {
  return 0;
}

// The shared body of all four COP2 moves; declared before them so the header reads top-down.
constexpr std::uint32_t cop2Move(std::uint32_t sub, std::uint32_t rt, std::uint32_t reg) {
  return (0x10u << 26) | (sub << 21) | (rt << 16) | ((reg & 0x1Fu) << 11);
}
constexpr std::uint32_t syscallInstruction() {
  return 0x0000000Cu;
}

// COP2 (GTE) — mtc2 moves a GPR into a GTE data register, cfc2 reads a GTE control register back.
// A loop that touches both makes the GTE section of a save state observable in the resulting digest.
//
// All four COP2 MOVES share the primary opcode 010000; the RS field (bits 25..21) selects which:
// MFC2 00000, CFC2 00010, MTC2 00100, CTC2 00110. Two ways to get this wrong, both of which assemble
// something that still RUNS and silently means the other thing:
//
//   * treating the four as four primary opcodes (0x10/0x12/0x14/0x16) — 0x12 is an ordinary `swc2`,
//     so the program's writes go to memory instead of to the GTE and nothing reports an error;
//   * putting the GTE register number in bits 4..0 instead of the `rd` slot at bits 15..11 — those are
//     the FUNCTION field, which is zero for a COP2 move.
//
// The field layout is lightrec's own decoder (disassembler.h `enum cp2_basic_opcodes`, compared against
// `op.r.rs` in lightrec_mfc), not the manual's shorthand.
constexpr std::uint32_t mfc2(std::uint32_t rt, std::uint32_t reg) {
  return cop2Move(0x00u, rt, reg);
}
constexpr std::uint32_t cfc2(std::uint32_t rt, std::uint32_t reg) {
  return cop2Move(0x02u, rt, reg);
}
constexpr std::uint32_t mtc2(std::uint32_t rt, std::uint32_t reg) {
  return cop2Move(0x04u, rt, reg);
}
constexpr std::uint32_t ctc2(std::uint32_t rt, std::uint32_t reg) {
  return cop2Move(0x06u, rt, reg);
}

} // namespace psx::test::mips

#endif // PSXPORT_TESTS_MIPS_ASM_H