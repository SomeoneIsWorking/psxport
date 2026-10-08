// gpu_native_dma.cpp — DMA2's words into GpuState's parser: the ordering-table walk, host-memory
// packet replay and block mode, each word stamped with its guest address (native depth keys on it).
// The guest-visible device gets the same words in gpu_native_api.cpp.
#include "cfg.h" // cfg_str — the opt-in scene-dump frame number
#include "core.h"
#include "game.h"
#include "gpu_native_internal.h" // GpuState, guestAddressOf, gpu_scene_log
#include "ordering_table.h"      // the guest DrawOTag chain, walked by its own cursor

#include <lucent/log.h>

// DMA channel 2 (GPU): walk an ordering-table linked list from `madr`, feeding each node's
// GP0 words to the parser. Header word: bits[24..31]=word count, bits[0..23]=next node addr
// (0xFFFFFF = end).
void GpuState::gpu_dma2_linked_list(Core *core, uint32_t madr) {
  {
    static int sd = -2;
    if (sd == -2) {
      const char *e = cfg_str("PSXPORT_SCENEDUMP");
      sd = e ? atoi(e) : -1;
    }
    if (sd >= 0 && s_frame == sd) {
      gpu_scene_log(core, madr);
    }
  }
  s_dma2++;
  s_ot_madr = madr & 0x1FFFFC;
  // The projections behind this table's packets are complete: pair them with what it draws.
  core->rsub.projectionProvenance.sealForDraw();
  using psx::gpu::OrderingTableCursor;
  // PSXPORT_DEBUG=ot (diagnostic only — the driver no longer reads the OT): on a chain that fails to
  // terminate within an OT's worth of nodes (cyclic = malformed), dump its first 40 nodes once for diagnosis.
  // (Empty OTs are ~0x800 link-only nodes that DO terminate at the sentinel; a true cycle never terminates.)
  // GUARD KEPT: a 4096-step guest-memory chain walk to decide whether the OT terminates, then a
  // 40-node dump — all of it non-logging work, and none of it may run on an ordinary run.
  //
  // The probe walks with its OWN cursor rather than reading headers inline, so "does this chain
  // terminate" is answered by the same end-of-chain rule the real walk uses. A probe that re-derived the
  // rule could disagree with the walk it is diagnosing — and then it would report a malformed table the
  // walk handles fine, or miss one it does not.
  if (lucent::channel_on("ot")) {
    static int dumped = 0;
    OrderingTableCursor probe(*core, madr);
    const bool bounded = [&probe] {
      // A SEPARATE, SMALLER cap than the walk's own: this probe answers "does the chain terminate at
      // all", and 4096 nodes is already far past any real table, so a chain still running here is
      // cyclic. Stated here rather than reused from the walk so the two cannot drift into one budget.
      constexpr int kProbeNodeBudget = 4096;
      while (probe.nodesEntered() < kProbeNodeBudget) {
        if (!probe.advance()) {
          return !probe.truncated();
        }
      }
      return false;
    }();
    if (!bounded && !dumped++) {
      lucent::debug(
          "ot", "[otdbg] MALFORMED OT from madr=0x{:08X}:", psx::gpu::guestAddressOf(psx::gpu::mainRamOffsetOf(madr)));
      OrderingTableCursor dumpWalk(*core, madr);
      for (int k = 0; k < 40; k++) {
        const psx::gpu::OtNode &node = dumpWalk.node();
        lucent::debug("ot",
                      "  [{:2}] @0x{:08X} hdr=0x{:08X} (n={}) -> 0x{:08X}",
                      k,
                      node.guestAddress(),
                      node.headerWord(),
                      node.gp0WordCount(),
                      node.nextGuestAddress());
        if (!dumpWalk.advance()) {
          break;
        }
      }
    }
  }
  // Enumerate this DrawOTag's prims in OT LINK order (the guest draw order), feeding each prim's GP0 words
  // to gpu_gp0() which (a) APPLIES the GPU state commands (E1 texpage, E2 texwindow, …) and (b) classifies
  // drawables into the engine render queue (RQ_BACKGROUND/WORLD/HUD). This is NOT "honoring the PSX
  // visibility order": the engine still OWNS what ends up on top — 3D world prims carry real per-vertex
  // depth (RQ_OM_DEPTH → the depth buffer decides occlusion, order-independent). What link order DOES give
  // us is the only correct enumeration for replaying guest GP0: GP0 state commands are ORDER-DEPENDENT and
  // must be applied in DRAW order, because each 2D sprite/poly binds the texpage/texwindow set by the E1/E2
  // node that PRECEDES it in the OT. (later-172 replaced this with a LINEAR packet-pool scan on the premise
  // that "memory order ≡ draw order, the engine re-sorts anyway." That premise is FALSE for 2D: a 2D OT
  // links its nodes in REVERSE allocation order, so the linear scan decoupled every E1 DR_TPAGE from its
  // sprite — the title/menu's two full-screen background sprites then sampled a STALE texpage and rendered
  // BLACK. The 3D field was unaffected only because its prims carry their texpage inline and the depth
  // buffer owns order. Owning 2D order from engine-side SCENE data — instead of replaying guest packets at
  // all — is the remaining M4 work; until then the guest draw order is the correct enumeration to replay.)
  OrderingTableCursor walk(*core, madr);
  for (;;) {
    const psx::gpu::OtNode &node = walk.node();
    s_cur_node = node.guestAddress();
    for (unsigned i = 0; i < node.gp0WordCount(); i++) {
      const uint32_t wordAddress = node.gp0WordRamOffset(i);
      s_gp0_src = wordAddress; // guest addr of this word (Phase-1 attach)
      gpu_gp0(core, core->mem_r32(wordAddress));
    }
    if (!walk.advance()) {
      break;
    }
  }
  s_gp0_src = 0; // non-OT gpu_gp0 callers (direct GP0 / FMV / block) carry no packet address
  // PSXPORT_DEBUG=pool: per-DrawOTag OT node count + the packet-pool high-water (write ptr 0x800BF544),
  // to inspect the widescreen fixed-buffer-overflow hypothesis (later-124). node count = OT entries the
  // walk traversed. (Pool write ptr is the field overlay's global; meaningless on non-field overlays.)
  // GUARD KEPT: `mx` is a high-water mark the line reports, so the read-and-update is real state work
  // and must stay tied to the same condition the line is (leaving it unguarded would change what the
  // reported high-water means).
  if (lucent::channel_on("pool")) {
    static int mx = 0;
    // The walk this replaced counted with a loop guard, so its count was `guard + 1` — one MORE than
    // the nodes it had actually read whenever the cap truncated it. Reproduced exactly rather than
    // quietly corrected, because a diagnostic that changes its own numbers during a refactor is a
    // diagnostic whose history becomes unreadable. See docs/issues/0131-ot-walk-node-count-at-cap.md.
    const int nodes = walk.nodesEntered() + (walk.truncated() ? 1 : 0);
    // The field overlay's packet-pool write cursor. A TITLE address, read raw: it means nothing on
    // another title, and the two tests that seed it set it to the pool BASE rather than a packet index,
    // so a reader must not read it as "how many packets were allocated". See
    // docs/issues/0130-title-globals-in-the-framework.md.
    const uint32_t pool = core->mem_r32(0x800BF544u);
    if ((int)pool > mx) {
      mx = (int)pool;
    }
    lucent::debug("pool",
                  "f{} madr=0x{:08X} nodes={} pool=0x{:08X} hi=0x{:08X}",
                  s_frame,
                  s_ot_madr | psx::gpu::kKseg0Base,
                  nodes,
                  pool,
                  (uint32_t)mx);
  }
  if (walk.truncated()) {
    static int warned = 0;
    if (!warned++) {
      lucent::warn("gpu",
                   "WARN: OT walk hit {}-node cap (madr=0x{:08X}) — malformed/cyclic ordering table",
                   psx::gpu::kOtNodeLimit,
                   s_ot_madr | psx::gpu::kKseg0Base);
    }
  }
  // FLUSH. The walk above ENUMERATES the guest's prims and QUEUES them; something must then drain the
  // queue, or it accumulates across frames until RenderQueue's fail-fast fires ("render queue full
  // (65536 items) — refusing to drop prims"). A guest-driven DrawOTag is a complete draw-list
  // submission, so its end is exactly the right boundary.
  //
  // This was missing for the GUEST-DRIVEN path only. rq_flush lived solely in Engine::drawOTag, which
  // the title FrameDriver calls — so a port whose native frame owner drained the
  // queue every frame, while a port still running the game's OWN main() on the substrate (Phase 0,
  // where DrawOTag reaches the GPU through DMA2 rather than through the hook) never did. The same
  // omission is already described in native_boot.cpp's drawOTag comment as a past bug that rendered
  // the whole front-end black; this is that bug's other half, on the path that has no native hook.
  //
  // A walk over an EMPTY ordering table (the link-only chain ClearOTagR produces) queues nothing, and
  // flush -> emitQueue already returns immediately on an empty queue, so no guard is needed here.
  core->game->rq.flush(core);
}

// See gpu_replay_guest_packet. Identical to the OT walk's inner loop except that the words arrive in
// host memory, so `s_gp0_src` is stamped with the guest-shaped address the word WOULD have had. That
// is not a fiction the submit path can notice: it reads the stamp to attribute the packet, and the
// address it is given is the one a guest packet of this stream really occupied.
void GpuState::replayGuestPacket(Core *core, uint32_t nodeAddress, const uint32_t *words, unsigned count) {
  s_cur_node = nodeAddress;
  for (unsigned i = 0; i < count; i++) {
    // Word 0 is the node's first GP0 word, and it sits one header word in — the same address the OT
    // walk stamps, so a replayed word is attributed exactly where that word would have been.
    s_gp0_src = nodeAddress + psx::gpu::kOtHeaderBytes + psx::gpu::kOtHeaderBytes * i;
    gpu_gp0(core, words[i]);
  }
  s_gp0_src = 0;
}
// DMA channel 2 block mode, RAM to GPU: `count` words from `madr`, each stamped with its guest
// address the way the linked-list walk stamps them (native depth keys its lookups on it).
void GpuState::gpu_dma2_block(Core *core, uint32_t madr, int count) {
  s_dma2++;
  uint32_t addr = madr & 0x1FFFFC;
  s_dma_src = addr;
  for (int i = 0; i < count; i++) {
    s_gp0_src = addr;
    gpu_gp0(core, core->mem_r32(addr));
    addr += 4;
  }
  s_gp0_src = 0; // leave no stale address for a later non-packet GP0 caller to inherit
}
