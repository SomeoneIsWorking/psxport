#include "oracle_boundary.h"

#include "mednafen-types.h" // Its C++ standard-library includes require C++ linkage.

extern "C" {
#include "cpu.h"
#include "oracle_shim.h"
#include "psx.h"
}
#include "gte_state.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace {

struct Boundary {
  uint32_t pc{}, next_pc{}, branch_pending{}, load_pending{}, irq_pending{}, device_pending{};
  std::array<uint32_t, 32> gpr{};
  uint32_t lo{}, hi{};
  std::array<uint32_t, 32> cp0{};
  std::array<uint32_t, 64> gte{};
  uint32_t gte_flags{};
};

bool words(std::istream &input, const char *name, uint32_t *out, size_t count) {
  std::string token;
  if (!(input >> token) || token != name) {
    std::fprintf(stderr, "oracle boundary: missing/out-of-order field %s\n", name);
    return false;
  }
  for (size_t i = 0; i < count; ++i) {
    if (!(input >> token)) {
      return false;
    }
    const char *first = token.data();
    if (token.starts_with("0x")) {
      first += 2;
    }
    const auto result = std::from_chars(first, token.data() + token.size(), out[i], 16);
    if (result.ec != std::errc{} || result.ptr != token.data() + token.size()) {
      std::fprintf(stderr, "oracle boundary: invalid hexadecimal word %s[%zu]\n", name, i);
      return false;
    }
  }
  return true;
}

bool registers(const char *path, Boundary &state) {
  std::ifstream input(path);
  std::string header;
  if (!(input >> header) || header != "ORACLE_BOUNDARY_V1") {
    return false;
  }
  if (!words(input, "pc", &state.pc, 1) || !words(input, "next_pc", &state.next_pc, 1) ||
      !words(input, "branch_pending", &state.branch_pending, 1) ||
      !words(input, "load_pending", &state.load_pending, 1) || !words(input, "irq_pending", &state.irq_pending, 1) ||
      !words(input, "device_pending", &state.device_pending, 1) ||
      !words(input, "gpr", state.gpr.data(), state.gpr.size()) || !words(input, "lo", &state.lo, 1) ||
      !words(input, "hi", &state.hi, 1) || !words(input, "cp0", state.cp0.data(), state.cp0.size()) ||
      !words(input, "gte", state.gte.data(), state.gte.size()) || !words(input, "gte_flags", &state.gte_flags, 1)) {
    return false;
  }
  if (input >> header) {
    std::fprintf(stderr, "oracle boundary: unexpected trailing field %s\n", header.c_str());
    return false;
  }
  return input.eof() && !input.bad();
}

bool memory(const char *path, size_t required, std::vector<unsigned char> &out) {
  std::ifstream input(path, std::ios::binary);
  out.resize(required);
  if (!input.read(reinterpret_cast<char *>(out.data()), static_cast<std::streamsize>(required)) ||
      input.peek() != std::char_traits<char>::eof() || input.bad()) {
    std::fprintf(stderr, "oracle boundary: %s must contain exactly %zu bytes\n", path, required);
    return false;
  }
  return true;
}

} // namespace

extern "C" int oracle_boundary_import(const char *registers_path, const char *ram_path, const char *scratch_path) {
  if (!registers_path || !ram_path || !scratch_path || !oracle_main_ram()) {
    return 0;
  }
  Boundary state;
  std::vector<unsigned char> ram, scratch;
  if (!registers(registers_path, state) || !memory(ram_path, oracle_ram_size(), ram) ||
      !memory(scratch_path, 1024, scratch)) {
    std::fprintf(stderr, "oracle boundary: incomplete architectural input; live state unchanged\n");
    return 0;
  }
  if (state.branch_pending || state.load_pending || state.irq_pending || state.device_pending ||
      state.next_pc != state.pc + 4u || (state.pc & 3u) || (state.pc & 0x1fffffffu) >= oracle_ram_size() ||
      state.gpr[0] || (state.cp0[12] & (1u << 16)) || (state.cp0[13] & 0xff00u)) {
    std::fprintf(stderr,
                 "oracle boundary: pending branch/load/IRQ/device, non-main-RAM PC, nonzero r0, "
                 "or cache-isolated state cannot be normalized\n");
    return 0;
  }
  // Reconstruct as well as power the CPU: CPU_Power alone retains constructor-owned halt,
  // address-map and dummy-load state from a previous window. Inputs were fully validated above.
  oracle_teardown();
  if (!oracle_init()) {
    return 0;
  }
  if (!oracle_load_exe(ram.data(), static_cast<uint32_t>(ram.size()), 0, state.pc, state.gpr[28], state.gpr[29])) {
    return 0;
  }
  std::copy(state.gpr.begin(), state.gpr.end(), CPU_GPR(PSX_CPU));
  CPU_LO(PSX_CPU) = state.lo;
  CPU_HI(PSX_CPU) = state.hi;
  for (unsigned reg = 0; reg < state.cp0.size(); ++reg) {
    CPU_SetCOP0(reg, state.cp0[reg]);
  }
  GteRegs *gte = GTE_CurState();
  std::copy(state.gte.begin(), state.gte.end(), gte->REG);
  gte->FLAGS = state.gte_flags;
  std::copy(scratch.begin(), scratch.end(), ScratchRAM->data8);
  std::fprintf(stderr,
               "oracle boundary: NORMALIZED architectural CPU window: 32 GPR, HI/LO, PC, "
               "32 CP0, 64 GTE+flags, %zu RAM and %zu scratch bytes; reset-derived timing, "
               "cache, IRQ and DPCR; not an exact console snapshot\n",
               ram.size(),
               scratch.size());
  return 1;
}
