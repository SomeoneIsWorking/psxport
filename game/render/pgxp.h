// class Pgxp — per-Core PGXP-lite subpixel cache (vertex-smoothing / PSX-wobble fix).
//
// PSX projected vertices to INTEGER screen coords; Beetle's GTE additionally computes SUBPIXEL-precise
// projected x/y/z and hands them to `PGXP_pushSXYZ2f` keyed by the packed integer SXY. We cache the
// precise coords keyed by that packed int; the GPU tee (gpu_native.c) reads them back by each vertex's
// integer (sx,sy) and rasterizes with FLOAT positions instead of the integer-snapped ones. Value-keyed
// PGXP-lite: on a key collision we fall back to integer coords, so a wrong match can only cost smoothing.
//
// PROPER OOP: one instance per Core, embedded on Render (`c->mRender->pgxp`) — was a file-scope cache
// in gte_beetle.cpp. `PGXP_pushSXYZ2f` is a Beetle-C ABI callback that lacks a Core* argument, so we
// keep a `sCurrent` bound pointer (parallels ProjPrim), set from `bind()` alongside gte_bind.
#pragma once
#include <cstdint>
class Core;

class Pgxp {
public:
  static constexpr int kBits = 15;
  static constexpr int kSize = 1 << kBits;
  static constexpr int kMask = kSize - 1;

  void bind(Core* c);                            // set the currently-bound cache to this instance
  static Pgxp* current() { return sCurrent; }    // Beetle callback consults this (no Core* in scope)

  void push(float x, float y, float z, uint32_t sxyPacked);
  bool lookup(int sx, int sy, float* px, float* py, float* pz) const;
  bool lookupView(int sx, int sy, float* vx, float* vy, float* vz) const;
  void frameReset();

private:
  struct Ent { uint32_t key; float x, y, z; float vx, vy, vz; uint8_t valid; };
  Ent mSlots[kSize];

  static Pgxp* sCurrent;

  static uint32_t keyOf(int sx, int sy) {
    return ((uint32_t)(sy & 0x7FF) << 11) | (uint32_t)(sx & 0x7FF);
  }
  static uint32_t slotOf(uint32_t key) {
    uint32_t h = key * 2654435761u;              // Knuth multiplicative hash
    return (h >> (32 - kBits)) & kMask;
  }
};
