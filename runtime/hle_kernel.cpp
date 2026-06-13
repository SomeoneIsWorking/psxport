// psxport HLE BIOS — stateful kernel services (heap + events + kernel-table
// installers). Split out from hle_syscalls.cpp because, unlike the libc
// string/mem routines there, these own persistent native state (the heap
// arena, the event-control-block table).
//
// Function numbers are the standard nocash psx-spx BIOS table indices.
// Identifications used here (table(fnum) -> name):
//   A0(0x33) malloc   A0(0x34) free      A0(0x37) calloc  A0(0x38) realloc
//   A0(0x39) InitHeap A0(0x72) _96_remove A0(0xA2) EnqueueCdIntr
//   B0(0x00) alloc_kernel_memory
//   B0(0x07) DeliverEvent  B0(0x08) OpenEvent   B0(0x09) CloseEvent
//   B0(0x0A) WaitEvent     B0(0x0B) TestEvent   B0(0x0C) EnableEvent
//   B0(0x0D) DisableEvent
//   B0(0x15) Krom2RawAdd (ret discarded by Tomba2's int-init)
//   B0(0x18) ResetEntryInt B0(0x19) HookEntryInt B0(0x35) FileWrite
//   B0(0x57) GetB0Table    B0(0x5B) ChangeClearPAD
//   A0(0x44) FlushCache    A0(0x49) GPU_cw
//   C0(0x00) EnqueueTimerAndVblankIrqs  C0(0x01) EnqueueSyscallHandler
//   C0(0x02) SysEnqIntRP   C0(0x03) SysDeqIntRP
//   C0(0x07) InstallExceptionHandlers   C0(0x08) SysInitMemory
//   C0(0x0A) ChangeClearRCnt            C0(0x0C) InitDefInt
//   C0(0x12) InstallDevices             C0(0x1C) AdjustA0Table
//
// IMPORTANT SCOPE NOTE: the event services here are pure bookkeeping. Real PSX
// event delivery is IRQ-driven through the exception handler; that path is NOT
// wired in this stage (it would require touching beetle's CPU/exception path).
// So events here model a consistent fired/enabled state machine — enough for
// game code that OpenEvent()s, then DeliverEvent()/TestEvent()/WaitEvent()s
// against its own software-delivered events — but hardware IRQ delivery
// (vblank, cdrom done, etc.) does not auto-fire these. WaitEvent therefore does
// not block (there is no scheduler to block on); it returns immediately.

#include "hle_bios.h"
#include <cstdio>
#include <cstdlib>

// ---------------------------------------------------------------------------
// Heap allocator
// ---------------------------------------------------------------------------
// A0(0x39) InitHeap(addr,size) hands us a game-owned region of main RAM to use
// as the malloc arena. The real BIOS uses a single first-fit free list with a
// 1-word size header per block; we do the same, kept entirely in native state
// (block bookkeeping lives in this struct, NOT in PSX RAM, so we never collide
// with whatever the game stores in the arena). Addresses we hand back are real
// PSX virtual addresses inside [base, base+size).
//
// Games typically InitHeap once at startup then malloc; a simple first-fit
// free list with coalescing is correct and adequate.

struct HeapBlock {
	uint32_t addr;   // PSX virtual address of the block's usable memory
	uint32_t size;   // usable bytes
	bool     used;
};

static const int HEAP_MAX_BLOCKS = 4096;
static HeapBlock s_blocks[HEAP_MAX_BLOCKS];
static int       s_block_count = 0;
static uint32_t  s_heap_base = 0;
static uint32_t  s_heap_size = 0;
static bool      s_heap_inited = false;

static void heap_init(uint32_t addr, uint32_t size) {
	// PSX InitHeap stores the heap in the given KUSEG/KSEG0 region. Keep the
	// raw virtual base (callers compare returned pointers to it).
	s_heap_base = addr;
	s_heap_size = size;
	s_block_count = 1;
	s_blocks[0].addr = addr;
	s_blocks[0].size = size;
	s_blocks[0].used = false;
	s_heap_inited = true;
}

// First-fit allocate `size` bytes; returns PSX addr or 0 on failure.
static uint32_t heap_alloc(uint32_t size) {
	if (!s_heap_inited || size == 0) return 0;
	size = (size + 7u) & ~7u; // 8-byte align allocations
	for (int i = 0; i < s_block_count; i++) {
		if (s_blocks[i].used || s_blocks[i].size < size) continue;
		if (s_blocks[i].size > size && s_block_count < HEAP_MAX_BLOCKS) {
			// Split: shift the tail blocks up by one, insert remainder after i.
			for (int j = s_block_count; j > i + 1; j--) s_blocks[j] = s_blocks[j - 1];
			s_blocks[i + 1].addr = s_blocks[i].addr + size;
			s_blocks[i + 1].size = s_blocks[i].size - size;
			s_blocks[i + 1].used = false;
			s_blocks[i].size = size;
			s_block_count++;
		}
		s_blocks[i].used = true;
		return s_blocks[i].addr;
	}
	return 0; // out of memory
}

static void heap_coalesce() {
	for (int i = 0; i + 1 < s_block_count;) {
		if (!s_blocks[i].used && !s_blocks[i + 1].used) {
			s_blocks[i].size += s_blocks[i + 1].size;
			for (int j = i + 1; j + 1 < s_block_count; j++) s_blocks[j] = s_blocks[j + 1];
			s_block_count--;
		} else {
			i++;
		}
	}
}

static void heap_free(uint32_t addr) {
	if (addr == 0) return;
	for (int i = 0; i < s_block_count; i++) {
		if (s_blocks[i].addr == addr && s_blocks[i].used) {
			s_blocks[i].used = false;
			heap_coalesce();
			return;
		}
	}
	// Unknown pointer: ignore (matches BIOS leniency; no crash).
}

// realloc: allocate new, copy min(old,new) bytes, free old. We don't track the
// old usable size beyond the block table, so look it up there.
static uint32_t heap_block_size(uint32_t addr) {
	for (int i = 0; i < s_block_count; i++)
		if (s_blocks[i].addr == addr && s_blocks[i].used) return s_blocks[i].size;
	return 0;
}

// ---------------------------------------------------------------------------
// Event control blocks (bookkeeping only — see SCOPE NOTE at top)
// ---------------------------------------------------------------------------
// OpenBIOS reported "EvCB0x10" => 16 event slots. We model that count.
struct EvCB {
	bool     open;     // slot allocated by OpenEvent
	bool     enabled;  // EnableEvent/DisableEvent
	bool     fired;    // DeliverEvent set it; TestEvent reads/clears it
	uint32_t ev_class; // class id (matched by DeliverEvent)
	uint32_t spec;     // spec bits  (matched by DeliverEvent)
	uint32_t mode;     // 0x1000 = mark-as-busy (TestEvent), 0x2000 = call func
	uint32_t func;     // callback PSX addr (NOT invoked — no IRQ path yet)
};

static const int EVCB_MAX = 16;
static EvCB s_ev[EVCB_MAX];

// Game-registered interrupt entry point (B0:0x19 HookEntryInt). The native
// exception handler in hle_irq.cpp reads this and invokes it on a hardware IRQ.
static uint32_t s_int_handler = 0;
extern "C" uint32_t psxport_hle_int_handler(void) { return s_int_handler; }

// Native event delivery helper for the IRQ path (see hle_bios.h). Marks matching
// open+enabled EvCBs fired and collects mode-0x2000 callback func addresses.
extern "C" int psxport_hle_deliver_event_funcs(uint32_t ev_class, uint32_t spec,
                                               uint32_t* out_func, int max) {
	int n = 0;
	for (int i = 0; i < EVCB_MAX; i++) {
		if (s_ev[i].open && s_ev[i].enabled &&
		    s_ev[i].ev_class == ev_class && (s_ev[i].spec & spec)) {
			s_ev[i].fired = true;
			if ((s_ev[i].mode & 0x2000) && s_ev[i].func && n < max)
				out_func[n++] = s_ev[i].func;
		}
	}
	return n;
}

// Event id encoding matches the BIOS convention (0xF1000000 | index), so an id
// handed back to game code round-trips through our table lookup.
static const uint32_t EV_ID_BASE = 0xF1000000u;
static int ev_index(uint32_t id) {
	uint32_t idx = id - EV_ID_BASE;
	return (idx < (uint32_t)EVCB_MAX) ? (int)idx : -1;
}

// ---------------------------------------------------------------------------
// Native HLE BIOS work area (laid down in otherwise-unused PSX RAM)
// ---------------------------------------------------------------------------
// The real BIOS keeps its jump tables and a per-process "kernel control" struct
// in low RAM and exposes the table base via GetB0Table()/GetC0Table(). Tomba2's
// interrupt-system init (0x80017D54) does:
//     P = GetB0Table();  Q = *(P + 0x16C);
//     handlerA = Q + 0x884;  handlerB = Q + 0x894;   // stored, then `jr`'d to
//     clear 11 words at Q+0x594.. ; clear 9 words at Q+0x62C..
// i.e. it derives BIOS-internal pad/callback handler entry points from a struct
// reachable through the B0 table, and JUMPS to them on Enable/DisablePad. Under
// a pure-HLE BIOS there is no ROM holding those handlers, so we publish a
// self-consistent native work area: a B0-table-shaped page P whose [+0x16C] slot
// points at a control struct Q, with a `jr $ra; nop` no-op stub physically at
// Q+0x884 and Q+0x894 (the addresses the game jumps to). Pad input is serviced
// natively in our runtime, so these BIOS pad handlers are correctly no-ops.
//
// Region 0x8000E000.. is BIOS-reserved and unused by the EXE (loads at
// 0x80010000) — verified zero in fresh + mid-intro RAM dumps.
static const uint32_t HLE_WORK_BASE = 0x8000E000u; // control struct Q
static const uint32_t HLE_B0TABLE   = 0x8000F000u; // fake B0 jump table P
static const uint32_t HLE_STUB_A    = HLE_WORK_BASE + 0x884u; // jr $ra; nop
static const uint32_t HLE_STUB_B    = HLE_WORK_BASE + 0x894u;
static bool s_work_inited = false;

static void work_area_init(uint8_t* ram) {
	if (s_work_inited) return;
	s_work_inited = true;
	// `jr $ra; nop` = 0x03E00008, 0x00000000 (MIPS, LE).
	hle_w32(ram, HLE_STUB_A + 0, 0x03E00008u); hle_w32(ram, HLE_STUB_A + 4, 0);
	hle_w32(ram, HLE_STUB_B + 0, 0x03E00008u); hle_w32(ram, HLE_STUB_B + 4, 0);
	// B0 table P: slot at +0x16C points at the control struct Q. (The game only
	// ever reads this one slot; the rest of the table page stays zero.)
	hle_w32(ram, HLE_B0TABLE + 0x16Cu, HLE_WORK_BASE);
}

// ---------------------------------------------------------------------------
// OpenBIOS kernel thread model — __globals + Process[] + TCB (Thread)[] array
// ---------------------------------------------------------------------------
// The real OpenBIOS kernel keeps a per-process current-thread pointer and a TCB
// array, both reachable through __globals @ 0x80000100 (globals.h). Tomba!2's
// front-end StrPlayer subsystem is a 2-thread coroutine system that drives the
// whole intro/title/attract sequence via OpenThread/ChangeThread + the exception
// (syscall) protocol. With __globals==0 the game's kernel-thread path read base
// 0 and `jr`'d to garbage (the f1284 AdEL). Populating the tables *only* helps if
// the entire thread model works (the verified all-or-nothing finding in
// docs/tomba2-hle-irq.md), which is what this + the TCB-based exception protocol
// in hle_irq.cpp implement.
//
// Layout (mirrors src/mips/openbios/kernel/{globals.h,threads.c} +
// common/kernel/threads.h, all verified against title.bin in docs/tomba2-threads.md):
//   __globals @ 0x80000100:
//     +0x08 processes ptr   -> Process array (1 process; process[0].thread = cur)
//     +0x0C processBlockSize = 1 * sizeof(Process) = 4
//     +0x10 threads ptr     -> TCB array
//     +0x14 threadBlockSize = 4 * sizeof(Thread) = 0x300
//   struct Process { Thread* thread; }           (4 bytes)
//   struct Thread (0xC0): +0x00 flags (0x1000 FREE, 0x4000 USED), +0x04 flags2,
//     +0x08 GPR[0..31] (r0..ra), +0x88 returnPC, +0x8C hi, +0x90 lo, +0x94 SR,
//     +0x98 Cause, +0x9C unknown[9].
// Region 0x8000C000.. is BIOS-reserved, below the EXE (0x80010000) and the HLE
// work area (0x8000E000) — verified zero in fresh + mid-intro RAM dumps. We lay
// the Process array at HLE_GLOB_PROC and the 4-slot TCB array at HLE_TCB_BASE.
const uint32_t HLE_GLOBALS    = 0x80000100u;        // __globals base
const uint32_t HLE_GLOB_PROC  = 0x8000C000u;        // Process array
const uint32_t HLE_TCB_BASE   = 0x8000C100u;        // TCB (Thread) array
const uint32_t HLE_TCB_STRIDE = 0xC0u;              // sizeof(Thread)
const int      HLE_TCB_COUNT  = 4;                  // 4 slots (title.bin verified)

// Thread struct field offsets (relative to a TCB base).
enum {
	TCB_FLAGS    = 0x00, TCB_FLAGS2 = 0x04,
	TCB_GPR      = 0x08,  // GPR[i] at TCB_GPR + i*4
	TCB_RETPC    = 0x88, TCB_HI = 0x8C, TCB_LO = 0x90, TCB_SR = 0x94, TCB_CAUSE = 0x98,
};
enum { TCB_FLAG_FREE = 0x1000u, TCB_FLAG_USED = 0x4000u };

static bool s_kernel_inited = false;

// Lay down __globals + the Process/TCB arrays. Called once before the game's
// kernel-thread path can run (from psxport_hle_kernel_tables_init at HLE boot).
extern "C" void psxport_hle_kernel_tables_init(uint8_t* ram) {
	if (s_kernel_inited) return;
	s_kernel_inited = true;
	// __globals (only the fields the game/kernel read; rest stay zero).
	hle_w32(ram, HLE_GLOBALS + 0x08, HLE_GLOB_PROC);                // processes
	hle_w32(ram, HLE_GLOBALS + 0x0C, 1u * 4u);                      // processBlockSize
	hle_w32(ram, HLE_GLOBALS + 0x10, HLE_TCB_BASE);                 // threads
	hle_w32(ram, HLE_GLOBALS + 0x14, (uint32_t)HLE_TCB_COUNT * HLE_TCB_STRIDE); // threadBlockSize
	// TCB array: all FREE except slot 0 (the boot/main thread) = USED.
	for (int i = 0; i < HLE_TCB_COUNT; i++)
		hle_w32(ram, HLE_TCB_BASE + (uint32_t)i * HLE_TCB_STRIDE + TCB_FLAGS, TCB_FLAG_FREE);
	hle_w32(ram, HLE_TCB_BASE + TCB_FLAGS, TCB_FLAG_USED);
	// process[0].thread = &threads[0] (current thread).
	hle_w32(ram, HLE_GLOB_PROC + 0, HLE_TCB_BASE);
}

// Current thread TCB address = *(*(__globals.processes)) = *( *(0x80000108) ).
extern "C" uint32_t psxport_hle_current_tcb(uint8_t* ram) {
	const uint32_t proc = hle_r32(ram, HLE_GLOBALS + 0x08);
	if (!proc) return 0;
	return hle_r32(ram, proc); // process[0].thread
}

// Set the current thread (process[0].thread = tcb). Used by ChangeThread / the
// a0=3 syscall.
extern "C" void psxport_hle_set_current_tcb(uint8_t* ram, uint32_t tcb) {
	const uint32_t proc = hle_r32(ram, HLE_GLOBALS + 0x08);
	if (proc) hle_w32(ram, proc, tcb);
}

// getFreeTCBslot (threads.c): first slot whose flags == FREE, else -1.
static int free_tcb_slot(uint8_t* ram) {
	for (int i = 0; i < HLE_TCB_COUNT; i++)
		if (hle_r32(ram, HLE_TCB_BASE + (uint32_t)i * HLE_TCB_STRIDE + TCB_FLAGS) == TCB_FLAG_FREE)
			return i;
	return -1;
}

// OpenThread(pc, sp, gp) -> handle (0xFF0000NN), -1 if full. Mirrors threads.c
// openThread: claim a FREE slot, mark USED, seed returnPC/sp/fp/gp. The thread
// is *not* made current — ChangeThread does that. Other regs default 0; the
// thread runs with the seeded entry context, exactly as the real kernel.
static uint32_t open_thread(uint8_t* ram, uint32_t pc, uint32_t sp, uint32_t gp) {
	int slot = free_tcb_slot(ram);
	if (slot < 0) return 0xFFFFFFFFu;
	const uint32_t tcb = HLE_TCB_BASE + (uint32_t)slot * HLE_TCB_STRIDE;
	// Zero the register frame first (fresh thread starts clean).
	for (uint32_t o = TCB_GPR; o <= TCB_CAUSE; o += 4) hle_w32(ram, tcb + o, 0);
	hle_w32(ram, tcb + TCB_FLAGS, TCB_FLAG_USED);
	hle_w32(ram, tcb + TCB_FLAGS2, 0x1000u);
	hle_w32(ram, tcb + TCB_GPR + HLE_R_SP * 4, sp);  // sp = r29
	hle_w32(ram, tcb + TCB_GPR + 30 * 4, sp);        // fp = r30
	hle_w32(ram, tcb + TCB_GPR + HLE_R_GP * 4, gp);  // gp = r28
	hle_w32(ram, tcb + TCB_RETPC, pc);
	// A fresh thread must resume with interrupts enabled (the kernel hands new
	// threads a sane SR). Seed SR = current thread's SR so it inherits the live
	// interrupt-enable/KU state rather than resuming with IRQs masked.
	const uint32_t cur = psxport_hle_current_tcb(ram);
	if (cur) hle_w32(ram, tcb + TCB_SR, hle_r32(ram, cur + TCB_SR));
	return 0xFF000000u | (uint32_t)slot;
}

// CloseThread(handle) -> 1. Mirrors threads.c closeThread: mark slot FREE.
static uint32_t close_thread(uint8_t* ram, uint32_t handle) {
	const uint32_t slot = handle & 0xFFFFu;
	if (slot < (uint32_t)HLE_TCB_COUNT)
		hle_w32(ram, HLE_TCB_BASE + slot * HLE_TCB_STRIDE + TCB_FLAGS, TCB_FLAG_FREE);
	return 1;
}

// Resolve a ChangeThread handle (0xFF0000NN) to its TCB address.
extern "C" uint32_t psxport_hle_tcb_from_handle(uint32_t handle) {
	const uint32_t slot = handle & 0xFFFFu;
	if (slot >= (uint32_t)HLE_TCB_COUNT) return 0;
	return HLE_TCB_BASE + slot * HLE_TCB_STRIDE;
}

// ---------------------------------------------------------------------------
// Interrupt-handler priority chain (C0:0x02 SysEnqIntRP / 0x03 SysDeqIntRP)
// ---------------------------------------------------------------------------
// SysEnqIntRP(priority, elem) links `elem` (a PSX struct {next, handler,
// verifier, ...}) into the kernel's interrupt-handler chain for that priority
// slot; SysDeqIntRP unlinks it. The real BIOS root handler walks these chains
// calling each verifier/handler. Tomba2's own root dispatcher (0x800182D8,
// invoked by hle_irq.cpp) reads I_STAT directly and does NOT walk this chain, so
// for this game the chain is bookkeeping — but we maintain it faithfully (real
// singly-linked list per priority, head kept in our native work area) rather
// than no-op'ing, so any consumer that does walk it sees a consistent list.
static const int HLE_NUM_PRIORITY = 4; // BIOS priorities 0..3
static uint32_t s_int_chain_head[HLE_NUM_PRIORITY] = {0,0,0,0};

static void sys_enq_int_rp(uint8_t* ram, uint32_t pri, uint32_t elem) {
	if (pri >= (uint32_t)HLE_NUM_PRIORITY || elem == 0) return;
	// Push onto the head: elem->next = head; head = elem. (elem->next is word[0].)
	hle_w32(ram, elem + 0, s_int_chain_head[pri]);
	s_int_chain_head[pri] = elem;
}

static void sys_deq_int_rp(uint8_t* ram, uint32_t pri, uint32_t elem) {
	if (pri >= (uint32_t)HLE_NUM_PRIORITY || elem == 0) return;
	uint32_t cur = s_int_chain_head[pri];
	if (cur == elem) { s_int_chain_head[pri] = hle_r32(ram, elem + 0); return; }
	while (cur) {
		uint32_t next = hle_r32(ram, cur + 0);
		if (next == elem) { hle_w32(ram, cur + 0, hle_r32(ram, elem + 0)); return; }
		cur = next;
	}
}

// ---------------------------------------------------------------------------
// Dispatcher for the stateful kernel functions. Returns 1 if handled (with
// gpr[V0] set), 0 if not ours. Called from psxport_hle_syscall().
// ---------------------------------------------------------------------------
extern "C" int psxport_hle_kernel(char table, uint32_t fnum, uint32_t* gpr, uint8_t* ram) {
	const uint32_t a0 = gpr[HLE_R_A0];
	const uint32_t a1 = gpr[HLE_R_A1];

	if (table == 'A') {
		switch (fnum) {
		case 0x33: // malloc(size) -> ptr (0 on failure)
			gpr[HLE_R_V0] = heap_alloc(a0);
			return 1;
		case 0x34: // free(buf) -> void
			heap_free(a0);
			gpr[HLE_R_V0] = 0;
			return 1;
		case 0x37: { // calloc(num, size) -> zeroed ptr
			uint32_t bytes = a0 * a1;
			uint32_t p = heap_alloc(bytes);
			if (p) for (uint32_t i = 0; i < bytes; i++) hle_w8(ram, p + i, 0);
			gpr[HLE_R_V0] = p;
			return 1;
		}
		case 0x38: { // realloc(old, new_size) -> ptr
			// BIOS semantics: realloc(0,n)==malloc(n); realloc(p,0)==free(p)->0.
			if (a0 == 0) { gpr[HLE_R_V0] = heap_alloc(a1); return 1; }
			if (a1 == 0) { heap_free(a0); gpr[HLE_R_V0] = 0; return 1; }
			uint32_t old_size = heap_block_size(a0);
			uint32_t np = heap_alloc(a1);
			if (np) {
				uint32_t n = (old_size < a1) ? old_size : a1;
				for (uint32_t i = 0; i < n; i++) hle_w8(ram, np + i, hle_r8(ram, a0 + i));
				heap_free(a0);
			}
			gpr[HLE_R_V0] = np;
			return 1;
		}
		case 0x39: // InitHeap(addr, size) -> void
			heap_init(a0, a1);
			gpr[HLE_R_V0] = 0;
			return 1;
		case 0x44: // FlushCache() -> void : flush the I-cache after self-modifying
			// code / overlay loads. The interpreter has no host code cache, so
			// there is nothing to invalidate; safe no-op.
			gpr[HLE_R_V0] = 0;
			return 1;
		case 0x49: // GPU_cw(gp0cmd) -> void : send one command word to GP0. The
			// game writes GP0/GP1 directly via MMIO for all its real drawing; this
			// BIOS helper is used only for incidental commands and its result is
			// not consumed. We have no generic GP0-write accessor in this stage, so
			// model it as a no-op (does not affect the crash path or rendering of
			// the game's own direct GPU writes). v0=0.
			gpr[HLE_R_V0] = 0;
			return 1;
		case 0x72: // _96_remove() -> void : CD-device teardown counterpart of
			// _96_init. No host device to tear down; benign no-op.
			gpr[HLE_R_V0] = 0;
			return 1;
		case 0xA2: // EnqueueCdIntr() -> void : installs the CD IRQ handler into
			// the kernel's interrupt chain. We service CD natively and have no
			// kernel IRQ chain, so there is nothing to enqueue. Safe no-op.
			gpr[HLE_R_V0] = 0;
			return 1;
		default:
			return 0;
		}
	}

	if (table == 'B') {
		switch (fnum) {
		case 0x00: // alloc_kernel_memory(size) -> ptr : kernel-pool allocator
			// used by the ROM (not the game heap). Back it with the same arena;
			// if the heap isn't inited yet this returns 0 (caller is ROM glue).
			gpr[HLE_R_V0] = heap_alloc(a0);
			return 1;

		case 0x07: { // DeliverEvent(class, spec) -> void : mark matching slots fired
			for (int i = 0; i < EVCB_MAX; i++) {
				if (s_ev[i].open && s_ev[i].enabled &&
				    s_ev[i].ev_class == a0 && (s_ev[i].spec & a1)) {
					s_ev[i].fired = true;
					// mode 0x2000 would invoke func via the IRQ path; not wired
					// here (see SCOPE NOTE), so we only set the fired flag.
				}
			}
			gpr[HLE_R_V0] = 0;
			return 1;
		}
		case 0x08: { // OpenEvent(class, spec, mode, func) -> event id (-1 on full)
			const uint32_t mode = gpr[HLE_R_A2];
			const uint32_t func = gpr[HLE_R_A3];
			for (int i = 0; i < EVCB_MAX; i++) {
				if (!s_ev[i].open) {
					s_ev[i].open = true;
					s_ev[i].enabled = false; // opened disabled; EnableEvent arms it
					s_ev[i].fired = false;
					s_ev[i].ev_class = a0;
					s_ev[i].spec = a1;
					s_ev[i].mode = mode;
					s_ev[i].func = func;
					gpr[HLE_R_V0] = EV_ID_BASE + (uint32_t)i;
					return 1;
				}
			}
			gpr[HLE_R_V0] = 0xFFFFFFFFu; // table full
			return 1;
		}
		case 0x09: { // CloseEvent(event) -> 1/0
			int i = ev_index(a0);
			if (i >= 0) { s_ev[i].open = false; gpr[HLE_R_V0] = 1; }
			else gpr[HLE_R_V0] = 0;
			return 1;
		}
		case 0x0A: { // WaitEvent(event) -> 1/0 : real BIOS blocks until fired.
			// No scheduler/IRQ delivery here (SCOPE NOTE), so we cannot block;
			// report the event as ready and consume the fired flag so a
			// wait-then-test loop terminates instead of spinning forever.
			int i = ev_index(a0);
			if (i >= 0) { s_ev[i].fired = false; gpr[HLE_R_V0] = 1; }
			else gpr[HLE_R_V0] = 0;
			return 1;
		}
		case 0x0B: { // TestEvent(event) -> 1 if fired (and clear), else 0
			int i = ev_index(a0);
			if (i >= 0 && s_ev[i].fired) { s_ev[i].fired = false; gpr[HLE_R_V0] = 1; }
			else gpr[HLE_R_V0] = 0;
			return 1;
		}
		case 0x0C: { // EnableEvent(event) -> 1/0
			int i = ev_index(a0);
			if (i >= 0) { s_ev[i].enabled = true; gpr[HLE_R_V0] = 1; }
			else gpr[HLE_R_V0] = 0;
			return 1;
		}
		case 0x0D: { // DisableEvent(event) -> 1/0
			int i = ev_index(a0);
			if (i >= 0) { s_ev[i].enabled = false; gpr[HLE_R_V0] = 1; }
			else gpr[HLE_R_V0] = 0;
			return 1;
		}

		case 0x15: // Krom2RawAdd(shiftjis) -> kanji-ROM glyph ptr (-1 if invalid).
			// Tomba2 calls this from its interrupt-system init (0x800179E0) but
			// DISCARDS the return (the next instruction overwrites v0). There is no
			// kanji font ROM under HLE; return -1 (the BIOS "invalid code" result),
			// which is the safe sentinel for any caller that does inspect it.
			gpr[HLE_R_V0] = 0xFFFFFFFFu;
			return 1;

		case 0x35: { // FileWrite(fd, buf, len) -> bytes written. Tomba2 uses this
			// only as the BIOS write() backing TTY (fd 1/2) for debug strings. Emit
			// stdout/stderr to our TTY sink (stderr); any other fd has no host file,
			// so report the full count as written (success) without storing.
			const uint32_t fd = a0, buf = a1, len = gpr[HLE_R_A2];
			if (fd == 1 || fd == 2) {
				for (uint32_t i = 0; i < len; i++) fputc(hle_r8(ram, buf + i), stderr);
			}
			gpr[HLE_R_V0] = len;
			return 1;
		}

		case 0x57: { // GetB0Table() -> ptr to the BIOS B0 jump table. Tomba2 reads
			// table[+0x16C] to reach a kernel control struct and derives pad-handler
			// entry points (struct+0x884 / +0x894) it later `jr`s to. Publish our
			// native work area so those derived pointers land on real `jr $ra`
			// no-op stubs (pad input is serviced natively). Returning 0 here was the
			// f1284 crash: table[+0x16C] read garbage -> jr to a junk address.
			work_area_init(ram);
			gpr[HLE_R_V0] = HLE_B0TABLE;
			return 1;
		}

		case 0x17: // ReturnFromException() -> void. The real BIOS restores the
			// saved register frame and RFEs. In our HLE, the game's root IRQ
			// dispatcher calls this near its end, but it ALSO has a normal
			// epilogue + `jr $ra` that returns to our exception trampoline's
			// sentinel, where hle_irq.cpp performs the single RFE + register
			// restore. So here ReturnFromException is a benign return-to-caller:
			// let the dispatcher run its epilogue; the real RFE happens at the
			// sentinel. (Returning gpr[v0]=0 and falling through to ra.)
			gpr[HLE_R_V0] = 0;
			return 1;
		case 0x18: // ResetEntryInt() -> void : clears the kernel's interrupt
			// entry-point hook. No kernel IRQ chain here; benign no-op.
			gpr[HLE_R_V0] = 0;
			return 1;
		case 0x19: // HookEntryInt(cb) -> void : registers the game's interrupt
			// chain element with the kernel. Record cb; hle_irq.cpp's native
			// exception handler reads cb->word[0] (the BIOS-vectored entry) and
			// decodes the dispatcher it tail-`jal`s, then invokes that on each
			// hardware IRQ. (Tomba2 passes cb=0x80025724, word[0]=0x80018268,
			// which jal's the root dispatcher 0x800182D8.)
			s_int_handler = a0;
			gpr[HLE_R_V0] = 0;
			return 1;
		case 0x0E: { // OpenThread(pc, sp, gp) -> handle. Claims a FREE TCB slot,
			// seeds its entry context. Mirrors threads.c openThread. The new
			// thread is dormant until ChangeThread switches to it.
			psxport_hle_kernel_tables_init(ram);
			const uint32_t gp = gpr[HLE_R_A2];
			gpr[HLE_R_V0] = open_thread(ram, a0, a1, gp);
			return 1;
		}
		case 0x0F: // CloseThread(handle) -> 1. Marks the slot FREE.
			gpr[HLE_R_V0] = close_thread(ram, a0);
			return 1;
		case 0x10: { // ChangeThread(handle) -> (context switch; does not return
			// to caller). threads.c changeThread() does changeThreadSubFunction(
			// &threads[id]) which is syscall a0=3: save the caller's context into
			// the current TCB, set process[0].thread = &threads[id], restore the
			// target TCB and resume it. We can't redirect PC from here, so request
			// the switch; HleSyscallHook performs the TCB save/load + PC redirect
			// (Hle_Irq_ThreadSwitch). v0=1 is the value the *caller* thread sees
			// when something later switches back to it.
			psxport_hle_kernel_tables_init(ram);
			const uint32_t tcb = psxport_hle_tcb_from_handle(a0);
			if (tcb) { psxport_hle_request_thread_switch(tcb); gpr[HLE_R_V0] = 1; }
			else gpr[HLE_R_V0] = 0;
			return 1;
		}
		case 0x5B: // ChangeClearPAD(int) -> void : toggles whether the BIOS pad
			// handler auto-clears the pad buffer. Pad handling is not BIOS-driven
			// in our runtime; benign no-op.
			gpr[HLE_R_V0] = 0;
			return 1;
		default:
			return 0;
		}
	}

	if (table == 'C') {
		switch (fnum) {
		// All C0 entries the game/ROM hits here are kernel-table INSTALLERS:
		// they wire BIOS-internal handlers (timer/vblank IRQ, syscall handler,
		// exception handlers, device list, default-int handler) into the kernel
		// jump tables and IRQ chain. In this HLE runtime there is no BIOS jump
		// table or kernel IRQ chain to install into (we service those natively
		// and don't run the ROM's dispatch), so each is a benign success no-op:
		// returning 0 lets the boot sequence proceed past them.
		case 0x00: // EnqueueTimerAndVblankIrqs(priority)
		case 0x01: // EnqueueSyscallHandler(priority)
		case 0x07: // InstallExceptionHandlers()
		case 0x08: // SysInitMemory(addr, size)
		case 0x0C: // InitDefInt(priority)
		case 0x12: // InstallDevices(ttyflag)
		case 0x1C: // AdjustA0Table()
			gpr[HLE_R_V0] = 0;
			return 1;
		case 0x02: // SysEnqIntRP(priority, elem) -> elem. Link the interrupt-handler
			// element into our native priority chain (real singly-linked list kept
			// in PSX RAM via elem->next). Tomba2's root dispatcher reads I_STAT
			// directly and does not walk this chain, but we maintain it faithfully.
			work_area_init(ram);
			sys_enq_int_rp(ram, a0, a1);
			gpr[HLE_R_V0] = a1;
			return 1;
		case 0x03: // SysDeqIntRP(priority, elem) -> elem. Unlink it from the chain.
			sys_deq_int_rp(ram, a0, a1);
			gpr[HLE_R_V0] = a1;
			return 1;
		case 0x0A: // ChangeClearRCnt(t, flag) -> old flag. Root-counter (timer)
			// auto-clear toggle. No BIOS-driven root-counter handling here;
			// return 0 (previous flag) as a benign result.
			gpr[HLE_R_V0] = 0;
			return 1;
		default:
			return 0;
		}
	}

	return 0;
}
