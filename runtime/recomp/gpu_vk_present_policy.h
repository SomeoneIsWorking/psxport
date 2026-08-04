#pragma once
#include <stdint.h>

// ---- "Does this present have anything new to show?" ------------------------------------------------
//
// A present is paced by the display field clock, but a guest need not produce a new picture every
// field. Hardware handles that by simply re-scanning the SAME framebuffer: a field in which the guest
// did nothing shows the previous image again. The renderer reproduces that by re-showing the last
// composite instead of rebuilding one (gpu: afca817d).
//
// The trap this header exists to close: "the guest did nothing" was read as "the geometry batch is
// empty", and that is only true for a port whose NATIVE producer owns the whole picture. A port still
// running the guest's own drawing has a SECOND way to produce a new picture — writing the framebuffer
// directly, via a CPU->VRAM upload, a fill, or a VRAM->VRAM copy, submitting zero primitives. Those
// screens (logo stills, loading screens, fades, pre-rendered art) are not "nothing new"; they are the
// whole frame. Treating them as nothing new never builds a composite at all, so they show black.
//
// That is issue 0029 one level up. 0029 was the same assumption inside render_geom ("total == 0 means
// clear to black"), fixed by GameConfig::preserveVramBackdrop. The empty-batch early-out then landed
// ABOVE upload_vram, so the preserve control could not be reached and the screens went black again.
//
// So the predicate is deliberately about CHANGE, not about who owns rendering: rebuild when EITHER
// source of a new picture fired. It carries the reason rather than a bool, so a caller (and a log)
// can say WHICH input made it decide, instead of a bare "skipped".
enum PresentRebuild {
  // Neither the geometry batch nor guest VRAM changed since the composite was built. Re-show it.
  PRESENT_REUSE_LAST = 0,
  // The guest submitted primitives this frame.
  PRESENT_REBUILD_GEOM,
  // The guest wrote the framebuffer directly (GP0 0xA0 upload / fill / VRAM->VRAM copy / native
  // load_image) since the composite was built, and submitted no primitives. THE UPLOAD-ONLY SCREEN.
  PRESENT_REBUILD_VRAM,
};

// guestVramIsPicture: GameConfig::preserveVramBackdrop — the port's own statement about whether the
//   guest's VRAM is part of the picture. It has to be consulted here, and it is NOT a convenience
//   gate to shrink the blast radius; the arm below is only MEANINGFUL when it is set. If a port's
//   native producer owns the frame, render_geom clears an empty batch to black, so "rebuild because
//   the guest wrote VRAM" would composite black over a good frame — strictly worse than re-showing
//   it. A guest VRAM write is new PICTURE content exactly when guest VRAM is the picture. This is the
//   same switch render_geom consults for its clear, so the two decisions cannot drift apart.
// vramWrites: a monotonically increasing count of guest CPU->VRAM write operations (the gpu_vk_dirty()
//   chokepoint). vramWritesAtLastBuild: its value when the composite currently on screen was built.
//   Compared with != rather than > so wraparound cannot wedge the decision into "never rebuild".
static inline PresentRebuild present_rebuild_decision(bool batchEmpty,
                                                      bool guestVramIsPicture,
                                                      uint32_t vramWrites,
                                                      uint32_t vramWritesAtLastBuild) {
  if (!batchEmpty) return PRESENT_REBUILD_GEOM;
  if (guestVramIsPicture && vramWrites != vramWritesAtLastBuild) return PRESENT_REBUILD_VRAM;
  return PRESENT_REUSE_LAST;
}
