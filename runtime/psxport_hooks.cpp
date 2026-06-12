// psxport hook & override registry. See psxport_hooks.h.

#include "psxport_hooks.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>

namespace {

struct Hook
{
  uint32_t pc;
  uint32_t expected_instr; // 0 = no signature check
  psxport_hook_fn fn;
};

std::vector<Hook> s_hooks;

} // namespace

uint32_t psxport_hook_count = 0;
uint8_t* psxport_cov_bitmap = nullptr;
int psxport_cd_instant = []() {
  const char* v = std::getenv("PSXPORT_CD_INSTANT");
  return v ? static_cast<int>(std::strtol(v, nullptr, 0)) : -1; // -1 = unset
}();
int psxport_cdc_log = []() { const char* v = std::getenv("PSXPORT_CDC_LOG"); return (v && *v && *v != '0') ? 1 : 0; }();

uint32_t psxport_last_pc = 0;
unsigned psxport_frame = 0;

extern "C" int psxport_on_pc(uint32_t pc, uint32_t instr, uint32_t* gpr, uint32_t* redirect_pc)
{
  psxport_last_pc = pc;
  pc &= 0x1FFFFFFF; // KSEG-agnostic
  for (const Hook& h : s_hooks)
  {
    if (h.pc != pc)
      continue;
    if (h.expected_instr != 0 && h.expected_instr != instr)
      continue; // different overlay resident at this address
    return h.fn(pc, gpr, redirect_pc);
  }
  return PSXPORT_HOOK_CONTINUE;
}

void psxport_add_hook(uint32_t pc, uint32_t expected_instr, psxport_hook_fn fn)
{
  s_hooks.push_back({pc & 0x1FFFFFFF, expected_instr, fn});
  psxport_hook_count = static_cast<uint32_t>(s_hooks.size());
}

void psxport_dump_cpu_state(const uint8_t* ram)
{
  const uint32_t* gpr = psxport_cpu_gpr();
  static const char* names[34] = {"zr", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3",
                                  "t4", "t5", "t6", "t7", "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
                                  "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra", "lo", "hi"};
  fprintf(stderr, "=== emulated CPU state: pc=%08X ===\n", psxport_last_pc);
  for (int i = 0; i < 32; i += 4)
  {
    fprintf(stderr, "  %s=%08X %s=%08X %s=%08X %s=%08X\n", names[i], gpr[i], names[i + 1], gpr[i + 1], names[i + 2],
            gpr[i + 2], names[i + 3], gpr[i + 3]);
  }
  fprintf(stderr, "  ra=%08X is the immediate caller; stack scan:\n", gpr[31]);
  const uint32_t sp = gpr[29] & 0x1FFFFF;
  int found = 0;
  for (uint32_t off = 0; off < 0x800 && found < 16; off += 4)
  {
    uint32_t w;
    if (sp + off + 4 > 0x200000)
      break;
    memcpy(&w, ram + sp + off, 4);
    // plausible return address: word-aligned RAM text, preceded by jal/jalr
    if ((w & 0xF0000003) == 0x80000000 && (w & 0x1FFFFF) >= 0x8000 && (w & 0x1FFFFF) < 0x1FFFF8)
    {
      uint32_t prev;
      memcpy(&prev, ram + ((w - 8) & 0x1FFFFF), 4);
      const bool is_jal = (prev >> 26) == 3;
      const bool is_jalr = ((prev >> 26) == 0) && ((prev & 0x3F) == 9);
      if (is_jal || is_jalr)
      {
        char target[16] = "reg";
        if (is_jal)
          snprintf(target, sizeof(target), "%08X", ((w - 8) & 0xF0000000) | ((prev & 0x3FFFFFF) << 2));
        fprintf(stderr, "    sp+%03X: ret=%08X  (caller %s -> %s)\n", off, w, is_jal ? "jal" : "jalr", target);
        found++;
      }
    }
  }
  if (!found)
    fprintf(stderr, "    (no plausible return addresses in first 2KB of stack)\n");
}

void psxport_clear_hooks()
{
  s_hooks.clear();
  psxport_hook_count = 0;
}
