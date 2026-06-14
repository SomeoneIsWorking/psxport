// S1 semantic test: run three hand-verified leaf functions from the recompiled core and
// check results, including delay-slot effects. Anchors the emitter to real behavior before
// the full differential harness (S4).
#include "r3000.h"
#include <stdio.h>

void func_80089A30(R3000*);  // lui v0,0x800B ; jr ra ; (DS) addiu v0,v0,-16428
void func_800535D4(R3000*);  // lbu v0,374(a0); jr ra ; (DS) addiu v0,v0,1
void func_800269EC(R3000*);  // lui v1,0x800C; li v0,1; jr ra; (DS) sb v0,-2033(v1)

static int fails = 0;
static void check(const char* name, uint32_t got, uint32_t want) {
  if (got == want) { printf("ok   %-14s = 0x%08X\n", name, got); }
  else { printf("FAIL %-14s = 0x%08X (want 0x%08X)\n", name, got, want); fails++; }
}

int main(void) {
  R3000 c = {0};

  // 1) pure register: v0 = 0x800B0000 - 16428 = 0x800ABFD4 (delay slot must apply)
  func_80089A30(&c);
  check("80089A30.v0", c.r[2], 0x800ABFD4u);

  // 2) load + delay slot: v0 = mem8(a0+374) + 1
  c.r[4] = 0x80010000u;                  // a0 -> RAM base
  mem_w8(0x80010000u + 374, 0x42);
  func_800535D4(&c);
  check("800535D4.v0", c.r[2], 0x43u);

  // 3) const + store side effect: v0 = 1 ; sb 1 -> 0x800C0000-2033 = 0x800BF80F
  c.r[2] = 0;
  func_800269EC(&c);
  check("800269EC.v0", c.r[2], 1u);
  check("800269EC.mem", mem_r8(0x800BF80Fu), 1u);

  // 4) override mechanism: register a native override for 0x80089A30 and confirm the
  //    wrapper runs it instead of the recomp body; then a super-call wrapping the body.
  extern void gen_func_80089A30(R3000*);
  static int ov_hit = 0;
  void native_ov(R3000* x) { ov_hit = 1; x->r[2] = 0xCAFEu; }
  void wrap_ov(R3000* x) { gen_func_80089A30(x); x->r[2] += 1; }  // super-call + augment

  rec_set_override(0x80089A30u, native_ov);
  R3000 c2 = {0}; func_80089A30(&c2);
  check("ovr.replaces", c2.r[2], 0xCAFEu);
  check("ovr.fired", (uint32_t)ov_hit, 1u);

  rec_set_override(0x80089A30u, wrap_ov);
  R3000 c3 = {0}; func_80089A30(&c3);
  check("ovr.supercall", c3.r[2], 0x800ABFD5u);  // body 0x800ABFD4 + 1

  rec_set_override(0x80089A30u, 0);              // toggle off -> recomp body again
  R3000 c4 = {0}; func_80089A30(&c4);
  check("ovr.toggleoff", c4.r[2], 0x800ABFD4u);

  printf("\n%s\n", fails ? "FAILED" : "all leaf tests passed");
  return fails ? 1 : 0;
}
