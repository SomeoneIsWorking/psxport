// render_noise.h — THE ONE definition of "guest RAM the render path legitimately differs in",
// derived from GameConfig instead of hardcoded.
//
// WHY THIS FILE EXISTS. The same predicate was written out THREE times with Tomba!2's literal
// addresses baked in — ot_attr.cpp (pool_range, fixed 2026-08-11), dualcore.cpp::isRenderRegion and
// selftest.cpp::od_is_render — inside game-agnostic framework code. Two consequences, both measured:
//   * on a game whose pool is elsewhere (spyro, spider1) the mask covers ORDINARY ENGINE DATA, so a
//     compare harness throws away real divergences and still prints "NO DIVERGENCE";
//   * because the copies were independent, fixing one left two lying.
// So: one header, one arithmetic, every consumer derives from GameConfig::{packetPool*, otRegion*,
// poolPtr*, dwellCounter}.
//
// THE HONEST ZERO (the whole point). A game that has not RE'd its pool leaves those fields 0 (spyro,
// spider1 — an honest zero with a TODO, per their own rules). A mask that cannot be derived is then
// EMPTY — it masks NOTHING — and says so once, loudly. It must never fall back to another game's
// window: an inherited mask does not merely drop ranges, it makes the harness's headline verdict a
// statement about a region it blinded itself to. Empty-and-noisy is recoverable; silently-wrong is not.
#pragma once
#include <stdint.h>
#include <stdio.h>    // snprintf (describe())
#include <stddef.h>   // size_t
#include "game_iface.h"
#include <lucent/log.h>

// The render-noise windows of ONE game, all derived from GameConfig. `hi` is EXCLUSIVE everywhere.
//
// ARITHMETIC, checked against the literals it replaces (Tomba!2, game/core/game_config.cpp):
//   pool  [packetPoolBase, packetPoolBase + 2*packetPoolStride)  = [0x800BFE68, 0x800E7E68)  both parities
//   env   [pool.hi, ot.lo)                                       = [0x800E7E68, 0x800E80A8)  draw env + dwell
//   ot    [otRegionBase, otRegionBase + 2*otRegionStride)         = [0x800E80A8, 0x800EC188)  both parities
//   ptrs  [min(poolPtrLast,poolPtrCur) - 4, max(...) + 8)         = [0x800BF4F0, 0x800BF54C)
// The old single literal window was [0x800BFE68, 0x800EA200): identical at the bottom, and 0x1F88
// bytes SHORT at the top — 0x800EA200 lands 0xE8 into OT parity 1 (0x800EA118 + 0x2070), so the old
// "OT (x2 pages)" comment was wrong even for Tomba!2 and parity 1 was 99% unmasked. The derived form
// covers both OT pages, i.e. it masks 0x1F88 MORE bytes than the literal did, all of them inside the
// ordering table — which is render output by definition. Every other bound reproduces exactly.
//
// The ptrs window's -4/+8 slop is INHERITED from the literal, not derived: it is the bookkeeping words
// adjacent to the pointer pair. (The literal's "+ dwell" annotation was wrong — dwellCounter is
// 0x800E809C, which lands in the `env` window, not here.)
struct RenderNoiseMask {
  uint32_t poolLo = 0, poolHi = 0;
  uint32_t otLo   = 0, otHi   = 0;
  uint32_t envLo  = 0, envHi  = 0;   // empty unless the OT sits just above the pool (see ENV_SLACK)
  uint32_t ptrLo  = 0, ptrHi  = 0;
  bool known = false;                // false => covers() is EMPTY, and something already said so

  // How far above the packet pool the OT may sit for the words BETWEEN them to count as render noise
  // (draw env / dwell counter). Tomba!2: 0x240. A game whose OT is nowhere near its pool gets no env
  // window rather than a masked megabyte.
  static constexpr uint32_t ENV_SLACK = 0x1000u;

  bool covers(uint32_t a) const {
    if (!known) return false;
    if (a >= poolLo && a < poolHi) return true;
    if (a >= otLo   && a < otHi)   return true;
    if (a >= envLo  && a < envHi)  return true;
    if (a >= ptrLo  && a < ptrHi)  return true;
    return false;
  }

  uint32_t bytes() const {
    return (poolHi - poolLo) + (otHi - otLo) + (envHi - envLo) + (ptrHi - ptrLo);
  }

  // Build the mask for `cfg`. `who` names the harness in the blind-mask warning, so the log says
  // which run's verdict is affected. Warns AT MOST ONCE per process (an inline function's local
  // static is one object across every TU), because these are per-byte-hot call sites.
  static RenderNoiseMask from(const GameConfig* cfg, const char* who) {
    RenderNoiseMask m;
    const bool havePool = cfg && cfg->packetPoolBase && cfg->packetPoolStride;
    const bool haveOt   = cfg && cfg->otRegionBase   && cfg->otRegionStride;
    const bool havePtrs = cfg && cfg->poolPtrCur     && cfg->poolPtrLast;
    if (!havePool && !haveOt && !havePtrs) {
      static bool warned = false;
      if (!warned) {
        warned = true;
        lucent::warn("render-noise",
                     "{}: the render-region mask is EMPTY — GameConfig::packetPool*/otRegion*/poolPtr* "
                     "are all 0 for this game, so RENDER-PATH NOISE WILL BE REPORTED AS GAMEPLAY "
                     "DIVERGENCE. It is NOT falling back to another game's window (that would silently "
                     "delete real divergences from the verdict). RE the packet pool / ordering table and "
                     "fill those fields.", who);
      }
      return m;
    }
    if (havePool) { m.poolLo = cfg->packetPoolBase; m.poolHi = cfg->packetPoolBase + 2u * cfg->packetPoolStride; }
    if (haveOt)   { m.otLo   = cfg->otRegionBase;   m.otHi   = cfg->otRegionBase   + 2u * cfg->otRegionStride;   }
    if (havePtrs) {
      const uint32_t lo = cfg->poolPtrLast < cfg->poolPtrCur ? cfg->poolPtrLast : cfg->poolPtrCur;
      const uint32_t hi = cfg->poolPtrLast < cfg->poolPtrCur ? cfg->poolPtrCur  : cfg->poolPtrLast;
      m.ptrLo = lo - 4u; m.ptrHi = hi + 8u;
    }
    if (havePool && haveOt && m.otLo >= m.poolHi && m.otLo - m.poolHi <= ENV_SLACK) {
      m.envLo = m.poolHi; m.envHi = m.otLo;
    }
    m.known = true;
    // A PARTIAL mask is still a blind spot, and the caller's verdict depends on which part is missing —
    // so name it rather than letting a half-derived mask read as a complete one.
    if (!havePool || !haveOt || !havePtrs) {
      static bool warnedPartial = false;
      if (!warnedPartial) {
        warnedPartial = true;
        lucent::warn("render-noise",
                     "{}: the render-region mask is PARTIAL — packet pool {}, ordering table {}, pool "
                     "pointers {}. The unset parts mask NOTHING, so divergence reported there may be "
                     "render noise rather than a gameplay bug.", who,
                     havePool ? "ok" : "MISSING", haveOt ? "ok" : "MISSING", havePtrs ? "ok" : "MISSING");
      }
    }
    return m;
  }

  // One line describing what is actually masked — for a harness report header, which must never print
  // another game's addresses as if they were this run's.
  // Returns a caller-owned string built into `buf`.
  const char* describe(char* buf, size_t n) const {
    if (!known) { snprintf(buf, n, "(no render region masked — GameConfig packet-pool/OT fields unset)"); return buf; }
    snprintf(buf, n, "pool 0x%08X..0x%08X  OT 0x%08X..0x%08X  env 0x%08X..0x%08X  ptrs 0x%08X..0x%08X (%u B)",
             poolLo, poolHi, otLo, otHi, envLo, envHi, ptrLo, ptrHi, bytes());
    return buf;
  }
};
