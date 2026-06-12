// psxport hook & override registry. See psxport_hooks.h.

#include "psxport_hooks.h"

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

extern "C" int psxport_on_pc(uint32_t pc, uint32_t instr, uint32_t* gpr, uint32_t* redirect_pc)
{
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

void psxport_clear_hooks()
{
  s_hooks.clear();
  psxport_hook_count = 0;
}
