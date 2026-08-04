// dma_irq.h — DPCR (0x1F8010F0) and DICR (0x1F8010F4), the DMA controller's channel-enable and
// interrupt registers, as pure rules over a word. No Core, no Game, no I/O — so the semantics can
// be tested hermetically (tests/test_dma_irq_gate.cpp) instead of only through a running game.
//
// WHY THIS EXISTS. Neither register was modelled: reads returned 0 and writes fell through to the
// stray-I/O path. That is not a harmless gap, because DICR is where a guest says WHICH transfers it
// wants a completion interrupt for. With the register absent, every completion looks armed, so a
// port that turns "DMA finished" into "run the guest's DMA callback" runs that callback on transfers
// the guest deliberately left silent.
//
// Measured consequence (spider1 RE-07): Sony libstr's sector-arrival handler streams an STR frame
// as N sector DMAs on channel 3 and enables the channel-3 DMA interrupt for the LAST chunk ONLY —
// its DMA starter sets or clears bit 16+ch in DICR per transfer. The frame-completion callback
// therefore ran once per SECTOR instead of once per FRAME, marking ring slots ready before the frame
// was assembled and advancing the producer's frame index N times too often. The intro movies
// deadlocked with the producer sitting behind the consumer.
//
// Layout (nocash psx-spx, DMA Interrupt Register):
//   0-5    unknown, R/W
//   6-14   not used (always zero)
//   15     force IRQ — sets the master flag unconditionally, R/W
//   16-22  IRQ enable for DMA0..DMA6
//   23     IRQ master enable for DMA0..DMA6
//   24-30  IRQ flag for DMA0..DMA6 — set by hardware on completion, write 1 to acknowledge
//   31     IRQ master flag — READ ONLY, computed: force || (master_en && (enable & flag))
#pragma once
#include <cstdint>

// DPCR's power-on value. The BIOS leaves it here and Sony's libraries read-modify-write it, so a
// guest that ORs its channel's enable bit in must read back the other channels' bits, not zero.
static constexpr uint32_t DPCR_RESET = 0x07654321u;

static constexpr uint32_t DICR_FORCE       = 0x00008000u;  // bit 15
static constexpr uint32_t DICR_ENABLE_MASK = 0x007F0000u;  // bits 16-22, one per channel
static constexpr uint32_t DICR_MASTER_EN   = 0x00800000u;  // bit 23
static constexpr uint32_t DICR_FLAG_MASK   = 0x7F000000u;  // bits 24-30, one per channel
static constexpr uint32_t DICR_MASTER_FLAG = 0x80000000u;  // bit 31, read-only
// Everything a store may set directly. The flag bits are excluded because a write to them
// ACKNOWLEDGES rather than sets, and bit 31 because it is read-only.
static constexpr uint32_t DICR_RW_MASK     = 0x00FF803Fu;

// The byte lanes a `bytes`-wide access at `addr` covers, as a mask over the 32-bit register.
// Sub-word access is not a corner case here: libstr's DMA starter read-modify-writes the BYTE at
// DICR+2 (the enable bits plus the master-enable bit) on every single transfer.
inline uint32_t dma_lane_mask(uint32_t addr, uint32_t bytes) {
  if (bytes >= 4) return 0xFFFFFFFFu;
  const uint32_t shift = (addr & 3u) * 8u;
  const uint32_t width = bytes * 8u;
  return ((width >= 32u) ? 0xFFFFFFFFu : ((1u << width) - 1u)) << shift;
}

// A stored value positioned into the lanes it is written through.
inline uint32_t dma_lane_value(uint32_t addr, uint32_t v, uint32_t bytes) {
  return (bytes >= 4) ? v : (v << ((addr & 3u) * 8u));
}

// What a read of DICR returns: the stored word plus the computed master flag.
inline uint32_t dma_dicr_read(uint32_t dicr) {
  const bool armed_and_flagged =
      (dicr & DICR_MASTER_EN) != 0 && (dicr & ((dicr & DICR_ENABLE_MASK) << 8)) != 0;
  const uint32_t out = dicr & ~DICR_MASTER_FLAG;
  return ((dicr & DICR_FORCE) || armed_and_flagged) ? (out | DICR_MASTER_FLAG) : out;
}

// Apply a store of `bytes` at `addr` to the stored DICR word. Plain R/W bits take the written value;
// a flag bit written as 1 is acknowledged (cleared); bit 31 is never stored. Both effects are
// restricted to the lanes actually written, which is why the ack cannot be expressed as a whole-word
// rule: a byte write to the enable lane must leave the flags in the top lane alone.
inline uint32_t dma_dicr_store(uint32_t cur, uint32_t addr, uint32_t v, uint32_t bytes) {
  const uint32_t lane = dma_lane_mask(addr, bytes);
  const uint32_t val = dma_lane_value(addr, v, bytes) & lane;
  const uint32_t rw = DICR_RW_MASK & lane;
  const uint32_t ack = DICR_FLAG_MASK & lane & val;
  return ((cur & ~rw) | (val & rw)) & ~ack;
}

// Would a completion on `ch` raise the DMA interrupt? This is the gate a completion signal must pass:
// the guest's per-channel enable AND the master enable, exactly as hardware ANDs them.
inline bool dma_irq_armed(uint32_t dicr, int ch) {
  return (dicr & DICR_MASTER_EN) != 0 && (dicr & (1u << (16 + ch))) != 0;
}

// Hardware's action when a transfer on `ch` finishes: set that channel's flag, but only if armed.
inline uint32_t dma_dicr_complete(uint32_t dicr, int ch) {
  return dma_irq_armed(dicr, ch) ? (dicr | (1u << (24 + ch))) : dicr;
}

// Acknowledge channel `ch`'s DICR flag (mem.cpp owns the register state).
//
// The port dispatches the guest's DMA-completion callback DIRECTLY, standing in for the BIOS DMA
// interrupt handler that would normally run first. Acknowledging the flag is part of what that
// handler does, so the port does it at the same point. Without this the flag latches on the first
// completion and DICR bit 31 reads asserted for the rest of the run, which is a lie about the
// hardware to any guest that looks.
void dma_irq_ack(int ch);

// The pending-completion set, owned by mem.cpp (which is where transfers finish) and drained by
// Hle::irqPoll (which is where guest code may safely be re-entered).
bool dma_done_owed(int ch);
void dma_done_taken(int ch);
bool dma_done_any();      // is ANY channel still owed? the deferred-work gate must not clear while so

// ─────────────────────────────────────────────────────────────────────────────────────────────────
// WHO gets called when a transfer finishes, and on WHICH channels.
//
// The port dispatches the guest's own DMA-completion callback in place of the BIOS DMA handler that
// would run on hardware. The BIOS keeps ONE CALLBACK PER CHANNEL, in a table the guest's
// `DMACallback(ch, fn)` writes; `GameConfig::dmaCallbackTable` is that table's base, so channel
// `ch`'s entry is `base + 4*ch`. A game that has not RE'd its table leaves the base 0 and gets no
// dispatch at all — the same behaviour as a game that registered no callback, never a wrong one.
inline uint32_t dma_callback_slot(uint32_t table, int ch) {
  return table ? table + 4u * (uint32_t)ch : 0u;
}

// The set of channels that have completed a transfer whose interrupt the guest ARMED, and therefore
// owe it the callback it registered.
//
// A SET, not a flag, and per CHANNEL, not per subsystem. Both were wrong before and both were wrong
// in a way that reads as working: the runtime tracked one boolean, set only by the CD channel, so a
// guest that armed channel 1 (MDEC-out) and channel 4 (SPU) — which Spider-Man does, DICR 0x009A0000
// — was simply never told. Its FMV player's MDEC-out callback, the routine that uploads a decoded
// strip to VRAM, was never called once in a whole run. A set is needed as well as a channel, because
// the MDEC pump drains DMA0 and DMA1 inside ONE guest store and both finish before the runtime
// reaches its next safe dispatch point.
//
// Dispatch is DEFERRED, not done at the completion: the completion happens inside a guest store,
// with native code mid-mutation. The runtime takes it at a function-entry boundary (Hle::irqPoll).
struct DmaDone {
  uint32_t mask = 0;
  // Record a completion on `ch`, applying the hardware gate. True if the guest is now owed a call.
  bool complete(uint32_t& dicr, int ch) {
    if (!dma_irq_armed(dicr, ch)) return false;
    dicr = dma_dicr_complete(dicr, ch);           // hardware sets the channel's flag
    mask |= 1u << ch;
    return true;
  }
  bool owed(int ch) const { return (mask & (1u << ch)) != 0; }
  void taken(int ch) { mask &= ~(1u << ch); }
};
