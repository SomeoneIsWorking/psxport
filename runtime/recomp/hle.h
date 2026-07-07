// hle.h — class Hle — BIOS HLE subsystem (event control blocks, native first-fit heap, IRQ /
// work-area flags), owned by Game (`c->game->hle`, back-pointer wired in Game()). Implemented in
// hle.cpp. The deliverEvent method promotes the former free function hle_deliver_event (called by
// timing/native_boot/memcard/asset for VBlank + memcard + sound-DMA event delivery) so callers do
// c->game->hle.deliverEvent(class, spec) — no Core* arg on the surface.
#pragma once
#include <cstdint>
class Game;

struct HleEvCB { int open, enabled, fired; uint32_t ev_class, spec, mode, func; };  // was EvCB
struct HleHeapBlock { uint32_t addr, size; int used; };                             // was HeapBlock
class Hle {
public:
  Game* game = nullptr;
  HleEvCB     ev[16]      = {};   // was s_ev[EVCB_MAX]
  HleHeapBlock blk[4096]  = {};   // was s_blk[HEAP_MAX_BLOCKS]
  int      nblk       = 0;        // was s_nblk
  uint32_t heap_base  = 0;        // was s_heap_base
  uint32_t heap_size  = 0;        // was s_heap_size
  int      heap_ok    = 0;        // was s_heap_ok
  int      work_ok    = 0;        // was s_work_ok
  uint32_t int_handler = 0;       // was s_int_handler (B0:0x19 HookEntryInt)
  int      irq_enabled = 1;       // was s_irq_enabled

  // deliverEvent(evClass, spec): mark every open+enabled event slot whose class matches evClass
  //   and whose spec masks against `spec` as fired. Called by the frame VBlank tick, memcard
  //   completion, and sound-DMA completion so guest waits (TestEvent/WaitEvent) advance.
  void deliverEvent(uint32_t evClass, uint32_t spec);

  // ---- BIOS-side helpers -------------------------------------------------------
  // heap: A0:0x33-0x39 native first-fit arena (bookkeeping outside PSX RAM).
  void     heapInit(uint32_t addr, uint32_t size);
  uint32_t heapAlloc(uint32_t size);
  void     heapFree (uint32_t addr);
  uint32_t heapBlockSize(uint32_t addr) const;
  // work area (B0:0x56/0x57 GetC0Table/GetB0Table): publish a self-consistent native page.
  void     workAreaInit();
  // events: index-lookup for B0:0x08/0x09/0x0A/0x0B/0x0C/0x0D
  int      eventIndex(uint32_t id) const;
  // BIOS-call dispatch (A0/B0/C0). Returns true if handled (Core V0 set).
  bool     dispatchBios(char table, uint32_t fn);

private:
  void     heapCoalesce();   // internal free-side merge pass; only touches this instance's blk[]
};
