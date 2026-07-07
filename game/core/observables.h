// game/core/observables.h — the OBSERVABLE-STATE positive list (USER 2026-07-08).
//
// pc_skip legitimately diverges from the faithful/substrate path in transient scratch, stack
// bytes and cadence counters — but everything the game/player OBSERVES must match: the SFX/audio
// tables, the loaded module pointers, consumable handshake flags, and the SPU sample banks. This
// list is shared by the two comparers built on it:
//   * VerifyHarness::skipCheck (SV_CHECK) — fork-level TDD: run the skip leg AND the substrate
//     oracle arc inline from the same pre-state, compare these regions, abort on diff.
//   * Sbs checkObservables (PSXPORT_SBS_MODE=skip) — whole-run drift detector with settled-
//     divergence semantics.
// GROW this list as observable-output bugs surface — it is the OPPOSITE of an allowlist: a
// positive list of what MUST match. Never add an exemption to hide a diff.
#pragma once
#include <stdint.h>

struct ObsRegion { const char* label; uint32_t lo, hi; };

// Guest-RAM regions. (SPU RAM 512 KB and the per-area fx-table deref are handled specially by
// each comparer — the former lives host-side, the latter chases a pointer.)
static const ObsRegion kObsRegions[] = {
  { "AUDIO fx_table",       0x800A4D18u, 0x800A4EF8u },   // id 0..111 SFX entries (stride 8)
  { "AUDIO fx_area_ptrs",   0x800A4EF8u, 0x800A4F80u },   // per-area SFX table pointers
  { "AUDIO seq_slots",      0x800BE3B8u, 0x800BE3F8u },   // libsnd seq slot table (FUN_8007566C)
  { "AUDIO global_scale",   0x800FB165u, 0x800FB166u },
  { "libcd file-table",     0x800BE0F0u, 0x800BE110u },   // sector bases the loaders consume
  { "module ptr table",     0x800ECF58u, 0x800ECFD8u },   // relocation targets (area data modules)
  { "SWDATA blob base",     0x1F80022Cu, 0x1F800230u },   // scratchpad: *0x1F80022C (VAB chain input)
  { "done/ready flags",     0x1F80019Bu, 0x1F80019Cu },   // spawn-and-wait completion
  { "load-state byte",      0x1F800206u, 0x1F800207u },
};
static constexpr int kNObsRegions = (int)(sizeof(kObsRegions) / sizeof(kObsRegions[0]));
