// legacy_game_config.h — deprecated guest-address/configuration bag for migrating consumers.
#pragma once
#include <stdint.h>

#ifdef __cplusplus

#include "platform_hle.h"

// GameConfig — deprecated compatibility storage for game-specific guest facts not yet migrated to
// typed runtime owners. A game fills one static instance; living legacy consumers read `c->cfg`.
// The first group below is adapter input only and must disappear after every consumer passes its own
// psx::game::Crt0Image. Do not add fields here.
struct GameConfig {
  // --- adapter-only Crt0Image projection; generic crt0 code no longer reads these ---
  uint32_t bssZeroLo, bssZeroHi;        // .bss clear range
  uint32_t stackTopBase, stackTopBase2; // guest stack top globals
  uint32_t heapBase;                    // heap start
  uint32_t heapSizePtr, heapBasePtr;    // heap globals written by crt0
  uint32_t gp;                          // global pointer
  uint32_t libcInit;                    // libc init entry
  uint32_t gameMain, crt0;              // game-main / crt0 entries

  // Guest MAIN .text range, masked to physical addresses. Read by nothing; titles still set it.
  uint32_t recMainLo, recMainHi;

  // --- per-parity packet pool ---
  uint32_t packetPoolBase, packetPoolStride;

  // --- stock libcd entries (StockLibcd) ---
  uint32_t cdCommand, cdSync;

  // STOCK Sony libcd CdGetSector(dest, words) — the sector-transfer routine. Appended at the END of
  // this group on purpose: GameConfig is initialised POSITIONALLY by every consumer, so inserting a
  // field mid-struct silently shifts every value after it.
  //
  // It carries no LBA: stock libcd positions the head with CdlSetloc and reads from wherever it was
  // left, so the handler consumes Cd::setloc_lba. Zero for a game that uses an engine loader instead.
  uint32_t cdGetSector;

  // Guest global holding stock libcd's READY callback pointer (what CdReadyCallback() writes). The
  // PC drives the read through it: a stock-libcd read is a per-sector callback loop, so invoking
  // this once per sector completes the transfer with no interrupt in the picture at all. Zero for a
  // game that does not use stock libcd.
  uint32_t cdReadyCbPtr;

  // Guest buffer holding stock libcd's LAST REQUESTED POSITION (what CdLastPos() returns), and the
  // last Setmode byte immediately after it: [+0..3] = the Setloc parameter, [+4] = mode.
  //
  // This is bookkeeping the REPLACED routine used to do. Overriding a library entry point means
  // inheriting the state it maintained: stock libcd records the Setloc parameter inside its
  // command-send routine, and its read path later seeds the expected-sector counter from it
  // (CdPosToInt(CdLastPos())). Skip the record and the counter is seeded from stale bytes, the
  // drive-position check fails, and every read is rejected — with nothing pointing at the override
  // as the cause. Zero for a game that does not use stock libcd.
  uint32_t cdLastPosBuf;

  // STOCK Sony libcd CdRead(sectors, buf, mode) and CdReadSync(mode, result). Overriding at THIS
  // level makes the PC perform the whole read: the guest's per-sector callback loop, drive-position
  // check, vblank timeout and retry path never run. Zero for a game that does not use stock libcd.
  uint32_t cdReadStock;
  uint32_t cdReadSync;

  // STOCK Sony libcd CdSearchFile(CdlFILE*, name) — resolved natively from the disc image's ISO9660
  // tree, so the guest's filesystem walk issues no drive activity at all.
  uint32_t cdSearchFile;

  // BASE of the guest's per-channel DMA-COMPLETION callback table — the table `DMACallback(ch, fn)`
  // writes, indexed by DMA channel (channel ch's entry is base + 4*ch). The port dispatches these
  // itself, standing in for the BIOS DMA interrupt handler, gated on the channel's DICR enable.
  //
  // EVERY channel, not just the CD's: a streaming reader relies on the channel-3 callback to promote
  // a ring slot from "DMA in flight" to "ready", and an FMV player relies on the channel-1 (MDEC-out)
  // callback to upload the decoded strip to VRAM. Announcing only one of them looks like a working
  // port right up until the other subsystem is used.
  //
  // Zero for a game whose table has not been reverse-engineered — then NO callback is dispatched,
  // which is the same behaviour as a guest that registered none, never a wrong one.
  uint32_t dmaCallbackTable;

  // --- pad driver (pad_input.cpp) ---
  uint32_t padSlot0Buf, padSlot1Buf, padDriverFn;
  uint32_t padSlotPtrTable; // (added P1.x) SIO driver per-slot buf-ptr table base (+slot*padSlotPtrStride)
  // Byte distance between consecutive slots' buffer pointers. A driver that keeps a flat pointer
  // array uses 4 (Tomba!2); one that stores the pointer INSIDE a per-port context record uses that
  // record's size (Spyro: libpad's 240-byte per-port context). 0 is read as 4 so a config that
  // predates this field keeps its old meaning rather than silently reading slot 1 from slot 0.
  uint32_t padSlotPtrStride;

  // --- platform HLE: the PSX hardware-sync primitives (sync_overrides.cpp) ---
  // These are the SCEI library entry points whose real bodies busy-spin on a hardware IRQ our no-IRQ
  // runtime never raises (libmdec/libcd/libgpu/libetc sync + the kernel task-switch funnel).
  // PlatformHle::initBuiltins() installs a native handler at each.
  //
  // They were hardcoded in sync_overrides.cpp until 2026-07-28 — the SAME defect the seed set had,
  // and wrong in the same way: the addresses are facts about ONE executable. For a different game
  // they miss every primitive it actually uses AND install handlers over unrelated functions that
  // happen to sit at those addresses, which is a wrong abort waiting to fire rather than a silent
  // no-op. (Found standing up a second consumer; Spider-Man has real code at Tomba!2's VSync
  // address.) Same remedy as recMainLo/recMainHi: the value travels with the game.
  //
  // ZERO MEANS "this game has no such primitive, or it has not been RE'd yet" — initBuiltins skips
  // it. A game that leaves one zero and needs it will hang in the real spin loop, which is the
  // honest signal that its RE is outstanding.
  struct PlatformHleCfg {
    // The address windows register_() will accept. Everything outside them is refused, which is what
    // keeps GAME/engine logic out of this table (game logic is owned top-down through the override
    // registry instead). Each slot is one exact half-open range; an unused one is left {0,0}. If NO
    // window is configured, register_() refuses everything and says so — a game must state its own
    // memory map. Direct and adapter storage share the one framework capacity constant.
    uint32_t windowLo[kPlatformHleWindowCapacity], windowHi[kPlatformHleWindowCapacity];

    // Read by nothing; titles still set it.
    uint32_t codeScanLo, codeScanHi;

    uint32_t decDctInSync, decDctOutSync;    // libmdec DecDCTinSync / DecDCToutSync
    uint32_t cdReadSync, cdDataSync;         // libcd CdReadSync / CdDataSync
    uint32_t cdInitHandshake;                // libcd low-level CdInit controller-ready handshake
    uint32_t gpuTimeoutArm, gpuTimeoutCheck; // libgpu GPU-DMA-completion timeout (arm / check)
    uint32_t gpuTimeoutDeadlineVar;          // guest global the arm writes its far-future deadline to
    uint32_t gpuTimeoutFlagVar;              // guest global the arm clears

    // libgte SetGeomOffset / SetGeomScreen — the two leaves through which the game STATES its camera
    // projection (screen centre OFX/OFY, projection-plane distance H). Owned natively so the port
    // RECORDS the projection where the game sets it instead of reading CR24/25/26 back out of the GTE
    // at draw time. See proj_params.h for the implementation and why it is oracle-safe.
    //
    // Zero for a game whose libgte entry points have not been located — then nothing is registered and
    // the guest bodies run, which leaves ProjParams unset and makes the native camera's
    // requireGeom abort rather than draw with an invented projection. That is the intended behaviour
    // for an un-RE'd port, not a gap to paper over.
    uint32_t setGeomOffset, setGeomScreen;

    // Measured libetc VSync entry; the legacy session binds it as its StockVSync (a fatal trap).
    uint32_t vsyncTrap;

    // Measured libgpu DrawSync entry. The host GPU consumes GP0/DMA work synchronously, so the
    // framework can complete this hardware wait without entering the guest's VSync-based body.
    uint32_t drawSync;
  } hle;

  // --- adapter-only rendering policy --------------------------------------------------------------
  // Does the guest's UPLOADED VRAM stay visible under the submitted primitives?
  //
  // The renderer clears the colour target to black before drawing, on the principle that "the PC
  // renderer shows ONLY what a native producer submitted". That is right for a port whose native
  // renderer owns the frame — anything left over from the guest's own drawing would be stale.
  //
  // It is WRONG for a port still running the guest's drawing code, because on real hardware an upload
  // into the display area IS visible. A game whose logo screens, FMV stills or menus are uploads with
  // no primitives renders them BLACK: the upload lands in VRAM and the clear discards it before
  // anything is drawn on top. That is exactly what Spyro's SCE/Universal screens do.
  //
  // ZERO KEEPS THE EXISTING BEHAVIOUR (clear to black), so a consumer that does not set it is
  // unaffected — and this field is APPENDED at the end of the struct because GameConfig is
  // initialised positionally by some consumers. Set to 1 while the guest still owns drawing.
  // LegacyGameRuntimeAdapter projects this into GameRuntime::guestVramIsPicture(). The renderer no
  // longer reads the bag. Migrated runtimes override that policy directly and may vary it per frame.
  uint32_t preserveVramBackdrop;

  // --- memory card (memcard.cpp) ----------------------------------------------------------------
  // The consuming game's memory-card env key and default backing-file path. The resolver used to
  // hardcode the FIRST consumer's key (PSXPORT_TOMBA2_CARD) and filename (scratch/saves/tomba2.mcr), so a second
  // consumer's card silently landed in the reference game's file, or nowhere. NULL keeps the old behaviour for a
  // consumer that has not set them. Appended at the end because GameConfig is initialised positionally.
  const char *cardEnvVar;
  const char *cardDefaultPath;

  // --- frame pacing (frame_pacer.cpp) -----------------------------------------------------------
  // The pacer sleeps a whole-frame interval per call so a live run plays at the game's intended
  // speed instead of spinning. `quota` is the number of DISPLAY FIELDS one pacing call represents,
  // and the interval is that many fields at the game's real field rate (FramePacer::fieldRateMilliHz,
  // decoded from GP1(0x08) bit 3) divided by `parts`.
  //
  // IT IS NOT "WINDOWED PACING". It used to be — the pacer early-returned when there was no window,
  // which made every headless timing number a statement about a program the user never runs.
  // Headless and windowed are one program (headless = no window surface, no audio device, nothing
  // else); `PSXPORT_NOPACE` is the ONE switch for "run as fast as the host can".
  //
  // ZERO IS LOUD, ONCE, AND PACES AT ONE FIELD. It used to fall through to reading the scratchpad
  // byte 0x1F800235 — the FIRST consumer's (Tomba!2) engine field, "vblanks per displayed frame,
  // =2 => 30fps" — which is ordinary working memory in any other game (Spyro's geometry renderer
  // writes vertex data over it), so a second consumer slept on garbage and its run crawled. That
  // fallback is DELETED: a port that has not derived its cadence must be visibly unconfigured, not
  // quietly mistimed. A new game MUST set this field.
  //
  // Semantics of the value are by CALLING CADENCE, not the game's display rate: a native-loop port
  // that calls FramePacer::paceFrame once per logic frame sets the game's fields-per-frame (2 => 30fps on a
  // 60-field display). A port that still runs the guest's own frame loop and paces once per field
  // sets 1. The LENGTH of a field is not this field's business — that comes from the game's video
  // standard (FramePacer::fieldRateMilliHz). Appended at the end because GameConfig is initialised
  // positionally.
  uint32_t paceQuota;

  // --- crt0 stack-top bias (crt0_boot.h) --------------------------------------------------------
  // The guest crt0's OWN adjustment of the stack-top word before it becomes sp:
  //
  //     v0 = mem[stackTopBase] + stackBias.value ;  sp = fp = v0 | 0x80000000
  //
  // and, because the biased word also feeds the heap-size subtraction, `heapsz = (v0 - mem[
  // stackTopBase2]) - (heapBase & 0x1FFFFFFF)` moves with it too.
  //
  // WHY THIS IS A DECLARATION AND NOT JUST A NUMBER. The framework used to hardcode `- 8`, which is
  // the stock PSY-Q crt0's `addi v0,v0,-8`. MEASURED over all six of this workspace's executables with
  // `tools/crt0_extract` (which calls the same `crt0_scan` the boot-time audit uses, so the extractor
  // and the gate cannot drift): FOUR bias by -8 (Tomba!2, Spyro, Spider-Man, Vagrant Story) and TWO do
  // not bias at all — Mega Man X4 AND Toy Story 2, whose measured value is 0. So zero cannot mean
  // "unset" for this field the way it does for every address in GameConfig — 0 is a real answer —
  // and a plain `int32_t` defaulting to 0 would silently move sp 8 bytes on the four ports that DO
  // bias, while defaulting to -8 would silently mis-boot every future port that does not. Both
  // silent. Hence: `declared` must be set to 1 by the consumer, and `crt0_plan` REFUSES a boot where
  // it is 0, naming the field. Loud is recoverable; either default is not.
  //
  // Appended at the END of the struct, like paceQuota above, because GameConfig is
  // initialised POSITIONALLY by some consumers (spider1) — a field inserted into the crt0 group at
  // the top would shift every value after it there, silently.
  struct Crt0StackBias {
    uint32_t declared; // 1 = this game has stated its bias (even if the bias is 0). 0 = crt0 refuses.
    int32_t value;     // the measured adjustment, e.g. -8 for the stock PSY-Q crt0, 0 for X4.
  } stackBias;

  // --- THE GUEST'S OWN DISPLAY HEIGHT, in scanlines (0 = not declared) --------------------------
  // How many lines the game really scans out. Normally the GPU knows: GP1(0x07) programs the vertical
  // display range and gpu_gp1 decodes it. But a port that HLEs libgpu's display setup may leave that
  // register never written — measured on Tomba!2, 2026-08-19: 16,061 GP1 writes in a session and not
  // one of them is 05, 07 or 08 — and then `s_disp_h` is the framework's 240-line DEFAULT, which is a
  // guess wearing the costume of a measurement.
  //
  // The cost of that guess is visible: Tomba!2 is a 320x224 game (its cutscene letterbox rects sit at
  // rows 0..11 and 212..223, flush to the bottom of a 224-line screen — see the RE note in the
  // consumer's game/render/cine_bars.cpp), so presenting 240 lines put 16 rows of framebuffer BELOW the
  // bottom bar on screen, rows a console never scans out. The USER reported it as "a bogus segment
  // below the cutscene black bars".
  //
  // Applies to the GUEST-SOURCED render paths only (gte / psx), which are the ones claiming to show
  // what the console showed. A native renderer owns its own frame and may present more (USER,
  // 2026-08-19, on exactly this: "PC is fine, oracle isn't"). Unset leaves every path on the decoded
  // GP1 value, which is the correct behaviour for a game whose GPU registers are really programmed.
  // Appended at the end: GameConfig is initialised POSITIONALLY by some consumers.
  uint16_t guestDisplayHeight;

  // --- runtime-allocated packet pools ----------------------------------------------------------
  // Optional alternative to packetPoolBase/packetPoolStride for engines whose two parity pools are
  // allocated from the guest heap. Each value is the guest address of a u32 global holding the live
  // inclusive base or exclusive end. OtAttr reads the pointed-to values and invalidates its cache when
  // the guest rewrites any descriptor. Leave all four zero for a fixed-pool game.
  //
  // Appended because positional GameConfig consumers must retain their existing field mapping.
  uint32_t packetPoolBasePtrs[2];
  uint32_t packetPoolEndPtrs[2];

  // The game's directory name under the OS user-data root (HostIdentity::userDataName): where its
  // B-key bug reports are saved. One path component; null saves under the framework's own name.
  // Appended because positional GameConfig consumers must retain their existing field mapping.
  const char *userDataName;
};

#endif // __cplusplus
