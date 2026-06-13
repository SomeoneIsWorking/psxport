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
//   B0(0x18) ResetEntryInt B0(0x19) HookEntryInt B0(0x5B) ChangeClearPAD
//   C0(0x00) EnqueueTimerAndVblankIrqs  C0(0x01) EnqueueSyscallHandler
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
