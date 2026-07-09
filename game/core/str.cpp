// game/core/str.cpp — see str.h. WIDE-RE TIER DRAFT, UNWIRED/UNVERIFIED (docs/fleet-workflow.md §6/§9).
#include "core.h"
#include "str.h"

// FUN_80079528 — strlen. RE (tools/disas.py 0x80079528 --all 20, cross-checked against
// generated/shard_2.c:10049 gen_func_80079528, which is instruction-exact ground truth):
//   v0 = *a0;
//   if (v0 == 0) { v1 = 0; goto done; }
//   do { a0++; v0 = *a0; v1++; } while (v0 != 0);
//   done: v0 = v1; return;
// i.e. a byte-for-byte transcription of libc strlen(). No stack frame, no sub-calls — a true
// leaf. Return value goes in v0 (c->r[2]); this native form returns it directly AND mirrors it
// into c->r[2] so a call site that inspects the ABI register after `rec_dispatch`-style plumbing
// still sees the right value if this draft is later wired as an override.
uint32_t Str::length(Core* c, uint32_t addr) {
  uint32_t p = addr;
  uint32_t len = 0;
  while (c->mem_r8(p) != 0) {
    p += 1;
    len += 1;
  }
  c->r[2] = len;
  return len;
}
