// ObjectTable::dispatch — see object_table.h. Faithful port of guest FUN_80026C88 with the
// existing `disp26c88verify` A/B gate preserved.
#include "object_table.h"
#include "core.h"
#include "cfg.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void rec_dispatch(Core*, uint32_t);
void rec_super_call(Core*, uint32_t);

void ObjectTable::dispatch() {
  Core* c = core;

  auto body = [&]() {
    uint32_t obj = TABLE_BASE;
    for (int i = 0; i < SLOT_COUNT; i++, obj += SLOT_STRIDE) {
      if (c->mem_r8(obj + 0) == 0) continue;
      uint32_t idx = c->mem_r8(obj + 1);
      uint32_t fn  = c->mem_r32(HANDLER_TABLE + (idx << 2));
      c->r[4] = obj;
      rec_dispatch(c, fn);   // handler(obj) — stays substrate / honors its own override
    }
  };

  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("disp26c88verify") ? 1 : 0;
  if (!s_v) { body(); return; }

  static uint8_t* ram0 = (uint8_t*)malloc(0x200000);
  static uint8_t* ramN = (uint8_t*)malloc(0x200000);
  uint8_t spad0[0x400], spadN[0x400];
  uint32_t regs0[32]; memcpy(regs0, c->r, sizeof regs0);
  memcpy(ram0, c->ram, 0x200000); memcpy(spad0, c->scratch, 0x400);

  body();

  memcpy(ramN, c->ram, 0x200000); memcpy(spadN, c->scratch, 0x400);
  memcpy(c->ram, ram0, 0x200000); memcpy(c->scratch, spad0, 0x400); memcpy(c->r, regs0, sizeof regs0);
  rec_super_call(c, 0x80026C88u);

  uint32_t sp = regs0[29] & 0x1FFFFFu, flo = (sp >= 0x800) ? sp - 0x800 : 0;
  int ro = -1;
  for (uint32_t a = 0; a < 0x200000; a++) if (c->ram[a] != ramN[a] && !(a >= flo && a < sp)) { ro = (int)a; break; }
  int so = -1;
  for (uint32_t a = 0; a < 0x400; a++) if (c->scratch[a] != spadN[a]) { so = (int)a; break; }
  static long ng = 0, nb = 0;
  if (ro >= 0 || so >= 0) {
    if (nb++ < 40) fprintf(stderr, "[disp26c88verify] MISMATCH ram@%x spad@%x sp=%x\n", ro, so, sp);
  } else if (++ng % 50 == 0) fprintf(stderr, "[disp26c88verify] %ld matches\n", ng);
}
