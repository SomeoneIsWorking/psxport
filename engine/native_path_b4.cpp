// Hand-native boot→cutscene path — batch b4: higher-level init nodes whose whole callee set is native.
// Sub-calls via rec_dispatch(c, addr). RE'd from the gen_func bodies; post-`jr ra` over-run dropped.
// A/B-verified (non-leaf: stack-region frame-slot diffs are benign; globals/pool/scratchpad must match).
#include "core.h"
#include <stdint.h>

static inline void call_fn(Core* c, uint32_t fn) { rec_dispatch(c, fn); }

// 0x8007B18C — top-level object-pool init. Calls 0x8004FB20 then 0x800798F8; zeroes 520 contiguous
// 68-byte slots at 0x800F2740; builds a downward-growing free-list of slot pointers at 0x800E7E74
// (head init 0x800ED8C0, pushing the 520 slot payloads last→first, payload base 0x800FB11C step -68);
// records the free count (520) at 0x800ED098; then runs eight further sub-inits.
static void ov_8007B18C(Core* c) {
  call_fn(c, 0x8004FB20u);
  call_fn(c, 0x800798F8u);

  for (int i = 0; i < 520; i++) {
    c->r[4] = 0x800F2740u + (uint32_t)i * 68u; c->r[5] = 0; c->r[6] = 68;
    call_fn(c, 0x8009A420u);                              // memset(slot, 0, 68)
  }

  c->mem_w32(0x800E7E74u, 0x800ED8C0u);                   // free-list head
  uint32_t payload = 0x800FB11Cu;                         // last slot (0x800FB160 - 68)
  for (int i = 0; i < 520; i++) {
    uint32_t head = c->mem_r32(0x800E7E74u);
    c->mem_w32(0x800E7E74u, head - 4);
    c->mem_w32(head - 4, payload);
    payload -= 68u;
  }
  c->mem_w16(0x800ED098u, 520);                           // free count

  call_fn(c, 0x8007ACC4u);
  call_fn(c, 0x8007A810u);
  call_fn(c, 0x8007AC14u);
  call_fn(c, 0x8007AC40u);
  call_fn(c, 0x8007AC6Cu);
  call_fn(c, 0x8007AC98u);
  call_fn(c, 0x8007AD14u);
  call_fn(c, 0x8007AD40u);
}

void games_native_path_b4_init(void) {
}
