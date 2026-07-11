// game/core/str.cpp — see str.h. WIRED (2026-07-10 verify pass, docs/fleet-workflow.md §9).
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

// ------------------------------------------------------------------------------------------------
// WIRING (verify pass, 2026-07-10, docs/fleet-workflow.md §9): re-diffed byte-for-byte against
// generated/shard_2.c:10049 -- exact strlen transcription, no bugs found. Checked every call site
// (generated/shard_0.c:12595, shard_5.c:4668, shard_6.c:4476, shard_7.c:12065/12089): a0(r4) is
// never read back for a "leftover cursor" the way Font::measureLineWidth's caller does -- every
// call site overwrites r4 before its next use, so no leftover-register mirroring is needed here.
// PLAIN intra-shard C calls at every call site (func_80079528(c), not rec_dispatch) -> wired via
// the oracle-gated engine_set_override_main thunk, SBS core B keeps running the pure gen_func_* body.
namespace {
void ov_strLength(Core* c) {
  Str::length(c, c->r[4]);
  // Mirror gen_func_80079528's v1 (r3) output (shard_2.c): the substrate's loop counter ends up
  // in r3 == r2 (the length) at return. The native C++ body only sets r2.
  c->r[3] = c->r[2];
}
}  // namespace

extern void gen_func_80079528(Core*);

void str_wide_re_install() {
  static bool done = false;
  if (done) return;
  done = true;
  extern void engine_set_override_main(uint32_t, OverrideFn, OverrideFn);
  engine_set_override_main(0x80079528u, ov_strLength, gen_func_80079528);
}
