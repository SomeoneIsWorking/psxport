#include "oracle_boundary.h"
#include "oracle_shim.h"
#include "oracle_snapshot.h"

#include "mednafen-types.h" // Its C++ standard-library includes require C++ linkage.

extern "C" {
#include "cpu.h"
#include "dma_dpcr.h"
#include "psx.h"
}
#include "gte_state.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

unsigned checks = 0;
void require(bool condition, const char *message) {
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "oracle snapshot: FAIL %s (check %u)\n", message, checks);
    std::exit(1);
  }
}

std::vector<unsigned char> read(const std::filesystem::path &path) {
  std::ifstream file(path, std::ios::binary);
  require(file.good(), "snapshot is readable");
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void write(const std::filesystem::path &path, const std::vector<unsigned char> &data) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
  require(file.good(), "fixture write completed");
}

void save(const std::filesystem::path &path) {
  require(oracle_snapshot_save(path.string().c_str()), "save complete owner state");
}
void load(const std::filesystem::path &path) {
  require(oracle_snapshot_load(path.string().c_str()), "restore complete owner state");
}

constexpr uint32_t kEntry = 0x80010000u;

void program() {
  // Load delay followed by a taken branch. Both intermediate pipeline states must survive restart.
  constexpr std::array<uint32_t, 8> code{0x8e080000u, // lw t0,0(s0)
                                         0x01004821u, // addu t1,t0,zero
                                         0x01005021u, // addu t2,t0,zero
                                         0x11480001u, // beq t2,t0,+1
                                         0x250b0001u, // addiu t3,t0,1 (delay slot)
                                         0x256c0002u, // addiu t4,t3,2
                                         0x00000000u,
                                         0x00000000u};
  require(oracle_load_exe(code.data(), sizeof(code), kEntry, kEntry, 0, 0x801fff00u), "load pipeline fixture");
  CPU_GPR(PSX_CPU)[16] = 0x80011000u;
  const uint32_t value = 0x12345678u;
  std::memcpy(oracle_main_ram() + 0x11000u, &value, sizeof(value));
  ScratchRAM->data8[1023] = 0x5a;
  CPU_SetCOP0(3, 0x13572468u);
  GTE_CurState()->REG[63] = 0xabcdef01u;
  GTE_CurState()->FLAGS = 0x12345000u;
  DMA_DPCR_LoadStateValue(0x33333333u);
}

void step(unsigned count) {
  for (unsigned i = 0; i < count; ++i) {
    require(oracle_step() == ORACLE_STOP_BUDGET, "clean independent instruction step");
  }
}

void roundtrip(const std::filesystem::path &root) {
  program();
  step(1);
  require(PSX_CPU->BACKED_LDWhich == 8u, "snapshot reaches a pending load");
  const auto pending = root / "pending.snapshot", expected = root / "expected.snapshot",
             actual = root / "actual.snapshot";
  save(pending);
  step(3);
  require(PSX_CPU->BDBT != 0, "window reaches pending branch delay");
  save(expected);
  load(pending);
  step(3);
  save(actual);
  require(read(expected) == read(actual), "pending-load replay preserves every serialized owner field");
  step(2);
  save(pending);
  load(expected);
  step(2);
  save(actual);
  require(read(pending) == read(actual), "branch-delay replay preserves every serialized owner field");
  require(CPU_GPR(PSX_CPU)[9] == 0u && CPU_GPR(PSX_CPU)[10] == 0x12345678u && CPU_GPR(PSX_CPU)[12] == 0x1234567bu,
          "load/branch semantics reached both old and committed values");
  require(ScratchRAM->data8[1023] == 0x5a && CPU_GetCOP0(3) == 0x13572468u && GTE_CurState()->REG[63] == 0xabcdef01u &&
              GTE_CurState()->FLAGS == 0x12345000u && DMA_DPCR_SaveStateValue() == 0x33333333u,
          "scratch CP0 raw GTE and DPCR survived");
}

void rejection(const std::filesystem::path &root) {
  program();
  const auto before = root / "before.snapshot", invalid = root / "invalid.snapshot", after = root / "after.snapshot";
  save(before);
  auto corrupt = read(before);
  corrupt.back() ^= 1;
  write(invalid, corrupt);
  require(!oracle_snapshot_load(invalid.string().c_str()), "checksum corruption refuses");
  save(after);
  require(read(before) == read(after), "corrupt restore leaves live state unchanged");
  corrupt.resize(20);
  write(invalid, corrupt);
  require(!oracle_snapshot_load(invalid.string().c_str()), "truncated header refuses");
  PSX_CPU->BACKED_LDWhich = 33;
  save(invalid);
  load(before);
  require(!oracle_snapshot_load(invalid.string().c_str()), "invalid pending-load target refuses with valid checksum");
  save(after);
  require(read(before) == read(after), "invalid execution-state restore is atomic");
}

void taint(const std::filesystem::path &root) {
  constexpr std::array<uint32_t, 2> code{0x3c081f80u, 0x8d091814u}; // GPUSTAT is unsupported.
  require(oracle_load_exe(code.data(), sizeof(code), kEntry, kEntry, 0, 0), "load unsupported-device fixture");
  step(1);
  require(oracle_step() == ORACLE_STOP_HARDWARE, "unsupported GPU read taints oracle");
  const auto file = root / "tainted.snapshot";
  save(file);
  program();
  load(file);
  require(oracle_step() == ORACLE_STOP_HARDWARE, "restored hardware taint prevents continuation");
  OracleDeviceState devices{};
  require(!oracle_capture_devices(&devices), "restored hardware taint refuses valid device evidence");
}

void boundary(const std::filesystem::path &root) {
  program();
  std::vector<unsigned char> ram(oracle_main_ram(), oracle_main_ram() + oracle_ram_size());
  std::vector<unsigned char> scratch(ScratchRAM->data8, ScratchRAM->data8 + 1024);
  const auto registers = root / "boundary.txt", ram_path = root / "boundary.ram", scratch_path = root / "boundary.spad";
  write(ram_path, ram);
  write(scratch_path, scratch);
  auto register_file = [&](unsigned pending) {
    std::ofstream out(registers);
    out << "ORACLE_BOUNDARY_V1\npc 80010000\nnext_pc 80010004\nbranch_pending 0\nload_pending " << pending
        << "\nirq_pending 0\ndevice_pending 0\ngpr" << std::hex;
    for (unsigned i = 0; i < 32; ++i) {
      out << ' ' << CPU_GPR(PSX_CPU)[i];
    }
    out << "\nlo 0\nhi 0\ncp0";
    for (unsigned i = 0; i < 32; ++i) {
      out << ' ' << CPU_GetCOP0(i);
    }
    out << "\ngte";
    for (uint32_t value : GTE_CurState()->REG) {
      out << ' ' << value;
    }
    out << "\ngte_flags " << GTE_CurState()->FLAGS << '\n';
    require(out.good(), "write architectural boundary fixture");
  };
  register_file(0);
  step(2);
  require(oracle_boundary_import(registers.string().c_str(), ram_path.string().c_str(), scratch_path.string().c_str()),
          "explicit clean boundary imports");
  require(oracle_timestamp() == 0 && PSX_CPU->BACKED_PC == kEntry && PSX_CPU->BACKED_LDWhich == 34,
          "normalized timing and clean pipeline are explicit");
  require(DMA_DPCR_SaveStateValue() == DMA_DPCR_RESET, "normalized DPCR uses owner reset value");
  const auto before = root / "boundary-before.snapshot", after = root / "boundary-after.snapshot";
  save(before);
  CPU_SetHalt_method(PSX_CPU, true);
  PSX_CPU->addr_mask[4] = 0;
  PSX_CPU->DummyPage[0] = 0;
  CPU_LD_DUMMY(PSX_CPU) = 0xfeedfaceu;
  CPU_SetEventNT(123456);
  require(oracle_boundary_import(registers.string().c_str(), ram_path.string().c_str(), scratch_path.string().c_str()),
          "normalization reconstructs constructor-owned CPU state");
  save(after);
  require(read(before) == read(after), "normalization is independent of prior halt, map, dummy and deadline state");
  register_file(1);
  require(!oracle_boundary_import(registers.string().c_str(), ram_path.string().c_str(), scratch_path.string().c_str()),
          "pending load refuses normalization");
  save(after);
  require(read(before) == read(after), "refused normalization preserves complete live state");
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: test_oracle_snapshot SCRATCH_DIRECTORY\n");
    return 2;
  }
  const std::filesystem::path root(argv[1]);
  std::filesystem::create_directories(root);
  require(oracle_init(), "initialize isolated oracle");
  roundtrip(root);
  rejection(root);
  taint(root);
  boundary(root);
  oracle_teardown();
  std::fprintf(
      stdout,
      "oracle snapshot: PASS %u checks; load/branch replay, atomic refusal, sticky taint, normalized boundary\n",
      checks);
  return 0;
}
