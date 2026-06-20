// PC-native engine LEVEL / STAGE LOADING (the engine system that reads a stage off the disc and brings
// the world into being). Per the CLAUDE.md boundary this is ENGINE → reimplemented PC-native; the bytes
// it loads (stage overlay code + level data) and the per-entity AI/physics that later run on it stay PSX.
//
// Entry chain from main: task0 = the stage sequencer FUN_800499e8 (resolves \BIN\START.BIN → its disc
// LBA/size into the stage table DAT_800be1e0/e4) → FUN_80052078(stage) (restart task at the stage) →
// FUN_800450bc(task, stage) = THIS loader. RE: scratch/decomp + tools/disas.py (later-162).
#include "core.h"
#include "cfg.h"
#include <stdint.h>

void rec_dispatch(Core*, uint32_t);
void rec_super_call(Core*, uint32_t);

// FUN_800450bc — load a stage's overlay off the disc and set the task's stage entry point.
//   a0 (param_1) = the task's entry-pointer slot (2 words: [0]=entry fn, [1]=task stack/handle)
//   a1 (param_2) = stage index;  stage 3 = the resident default (no load, entry = *0x800a3ed8)
// Per-stage (LBA,size) pairs live at 0x800be1e0/0x800be1e4 (stride 8); the overlay loads to 0x80106228;
// the per-stage entry table is at 0x800a3ecc. The PSX yields a frame after the load to wait for the async
// CD (FUN_80051f80(1)); our overlay loader (ov_cd_loadfile, the 0x8001db8c override) is SYNCHRONOUS — the
// data is present when it returns — so the yield is DROPPED. That also removes the only coroutine yield in
// this function, which is what makes a native reimplementation safe (no longjmp out of this C frame).
static void eng_load_stage(Core* c) {
  uint32_t param_1 = c->r[4];
  int32_t  stage   = (int32_t)c->r[5];
  uint32_t entry;
  if (stage == 3) {
    entry = c->mem_r32(0x800A3ED8u);                                   // resident default entry
  } else {
    uint32_t lba  = c->mem_r32(0x800BE1E0u + (uint32_t)stage * 8u);    // stage's disc LBA
    uint32_t size = c->mem_r32(0x800BE1E4u + (uint32_t)stage * 8u);    // stage's byte size
    c->r[4] = 0x80106228u; c->r[5] = lba; c->r[6] = size;
    rec_dispatch(c, 0x8001DB8Cu);                                     // synchronous overlay load (CD platform)
    entry = c->mem_r32(0x800A3ECCu + (uint32_t)stage * 4u);            // the stage's entry point
  }
  c->mem_w32(param_1 + 0, entry);                                      // *param_1 = stage entry fn
  rec_dispatch(c, 0x80080930u);                                        // task stack/handle alloc
  c->mem_w32(param_1 + 4, c->r[2]);                                    // param_1[1] = its result
  c->r[2] = (int32_t)stage;                                            // (recomp returns param_2; harmless)
}

void ov_load_stage(Core* c) {
  if (cfg_on("PSXPORT_LOADSTAGE_RECOMP")) { rec_super_call(c, 0x800450BCu); return; }   // A/B oracle
  eng_load_stage(c);
}
