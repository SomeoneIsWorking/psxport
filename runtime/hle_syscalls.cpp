// psxport HLE BIOS — stateless A0-table syscalls (libc string/mem + rand).
//
// These are the pure, side-effect-free standard-library routines the PSX BIOS
// exposes on the A0 table. They operate entirely on PSX main RAM through `ram`;
// stateful services (heap 0x33-0x39, events, threads, file I/O) are owned by
// the core and are NOT handled here. Function numbers below are the standard,
// stable A0 table indices.

#include "hle_bios.h"

#include <cstdio>
#include <cstdlib>

// PSX BIOS LCG. rand()/srand() share this seed; the exact recurrence and the
// >>16 & 0x7FFF extraction matter for determinism (must match real hardware).
static uint32_t s_rand_seed = 1;

// ---- A0(0x51) LoadExec pending-exec state -----------------------------------
// loadAndExec transfers control to the loaded EXE (never returns to its caller),
// so the A0(0x51) handler records the new entry context here and HleSyscallHook
// redirects the live CPU. (See hle_bios.h.)
static bool     s_exec_pending = false;
static uint32_t s_exec_pc = 0, s_exec_gp = 0, s_exec_sp = 0;

extern "C" int psxport_hle_take_pending_exec(uint32_t* out_pc, uint32_t* out_gp,
                                             uint32_t* out_sp) {
	if (!s_exec_pending) return 0;
	s_exec_pending = false;
	if (out_pc) *out_pc = s_exec_pc;
	if (out_gp) *out_gp = s_exec_gp;
	if (out_sp) *out_sp = s_exec_sp;
	return 1;
}

// Translate a PSX virtual address to a host pointer into the 2 MiB RAM. All the
// byte-wise loops below stay within RAM, so plain pointer arithmetic is safe.
static inline uint8_t* hle_ptr(uint8_t* ram, uint32_t a) { return ram + (a & 0x1FFFFF); }

int psxport_hle_syscall(char table, uint32_t fnum, uint32_t* gpr, uint8_t* ram) {
	// B0/C0 (and the stateful A0 heap/kernel functions, handled in the default
	// case below) are owned by the kernel module.
	if (table != 'A') return psxport_hle_kernel(table, fnum, gpr, ram);

	const uint32_t a0 = gpr[HLE_R_A0];
	const uint32_t a1 = gpr[HLE_R_A1];
	const uint32_t a2 = gpr[HLE_R_A2];

	switch (fnum) {
	case 0x15: { // strcat(dst, src) -> dst
		uint8_t* dst = hle_ptr(ram, a0);
		const uint8_t* src = hle_ptr(ram, a1);
		uint8_t* d = dst;
		while (*d) d++;
		while ((*d++ = *src++)) {}
		gpr[HLE_R_V0] = a0;
		return 1;
	}
	case 0x16: { // strncat(dst, src, n) -> dst
		uint8_t* dst = hle_ptr(ram, a0);
		const uint8_t* src = hle_ptr(ram, a1);
		uint8_t* d = dst;
		while (*d) d++;
		uint32_t n = a2;
		while (n-- && *src) *d++ = *src++;
		*d = 0; // strncat always NUL-terminates
		gpr[HLE_R_V0] = a0;
		return 1;
	}
	case 0x17: { // strcmp(s1, s2) -> int
		const uint8_t* p1 = hle_ptr(ram, a0);
		const uint8_t* p2 = hle_ptr(ram, a1);
		while (*p1 && *p1 == *p2) { p1++; p2++; }
		gpr[HLE_R_V0] = (uint32_t)((int)*p1 - (int)*p2);
		return 1;
	}
	case 0x18: { // strncmp(s1, s2, n) -> int
		const uint8_t* p1 = hle_ptr(ram, a0);
		const uint8_t* p2 = hle_ptr(ram, a1);
		uint32_t n = a2;
		int r = 0;
		while (n--) {
			if (*p1 != *p2 || !*p1) { r = (int)*p1 - (int)*p2; break; }
			p1++; p2++;
		}
		gpr[HLE_R_V0] = (uint32_t)r;
		return 1;
	}
	case 0x19: { // strcpy(dst, src) -> dst
		uint8_t* d = hle_ptr(ram, a0);
		const uint8_t* s = hle_ptr(ram, a1);
		while ((*d++ = *s++)) {}
		gpr[HLE_R_V0] = a0;
		return 1;
	}
	case 0x1a: { // strncpy(dst, src, n) -> dst
		uint8_t* d = hle_ptr(ram, a0);
		const uint8_t* s = hle_ptr(ram, a1);
		uint32_t n = a2;
		// Copy up to n chars; if src ends early, pad remainder with NUL.
		while (n) { uint8_t c = *s; *d++ = c; if (!c) break; s++; n--; }
		while (n) { *d++ = 0; n--; }
		gpr[HLE_R_V0] = a0;
		return 1;
	}
	case 0x1b: { // strlen(s) -> len
		const uint8_t* s = hle_ptr(ram, a0);
		uint32_t len = 0;
		while (s[len]) len++;
		gpr[HLE_R_V0] = len;
		return 1;
	}
	case 0x1c: // index(s, c) -> first occurrence ptr | 0
	case 0x1e: { // strchr (== index)
		uint32_t p = a0;
		uint8_t c = (uint8_t)a1;
		for (;;) {
			uint8_t ch = hle_r8(ram, p);
			if (ch == c) { gpr[HLE_R_V0] = p; return 1; } // c==0 matches the NUL terminator (libc semantics)
			if (!ch) { gpr[HLE_R_V0] = 0; return 1; }
			p++;
		}
	}
	case 0x1d: // rindex(s, c) -> last occurrence ptr | 0
	case 0x1f: { // strrchr (== rindex)
		uint32_t p = a0;
		uint8_t c = (uint8_t)a1;
		uint32_t found = 0;
		for (;;) {
			uint8_t ch = hle_r8(ram, p);
			if (ch == c) found = p;
			if (!ch) break;
			p++;
		}
		gpr[HLE_R_V0] = found;
		return 1;
	}
	case 0x25: { // toupper(c) -> c
		uint32_t c = a0;
		if (c >= 'a' && c <= 'z') c -= 0x20;
		gpr[HLE_R_V0] = c;
		return 1;
	}
	case 0x26: { // tolower(c) -> c
		uint32_t c = a0;
		if (c >= 'A' && c <= 'Z') c += 0x20;
		gpr[HLE_R_V0] = c;
		return 1;
	}
	case 0x27: { // bcopy(src, dst, n) -> void  [NOTE: a0=src, a1=dst, a2=n]
		// Like memmove (handle overlap), but no return value.
		uint8_t* src = hle_ptr(ram, a0);
		uint8_t* dst = hle_ptr(ram, a1);
		uint32_t n = a2;
		if (dst < src) { for (uint32_t i = 0; i < n; i++) dst[i] = src[i]; }
		else { for (uint32_t i = n; i-- > 0;) dst[i] = src[i]; }
		gpr[HLE_R_V0] = 0;
		return 1;
	}
	case 0x28: { // bzero(dst, n) -> void
		uint8_t* d = hle_ptr(ram, a0);
		for (uint32_t i = 0; i < a1; i++) d[i] = 0;
		gpr[HLE_R_V0] = 0;
		return 1;
	}
	case 0x29: { // bcmp(p1, p2, n) -> int (0 if equal)
		const uint8_t* p1 = hle_ptr(ram, a0);
		const uint8_t* p2 = hle_ptr(ram, a1);
		uint32_t n = a2;
		int r = 0;
		for (uint32_t i = 0; i < n; i++) if (p1[i] != p2[i]) { r = (int)p1[i] - (int)p2[i]; break; }
		gpr[HLE_R_V0] = (uint32_t)r;
		return 1;
	}
	case 0x2a: { // memcpy(dst, src, n) -> dst
		uint8_t* d = hle_ptr(ram, a0);
		const uint8_t* s = hle_ptr(ram, a1);
		for (uint32_t i = 0; i < a2; i++) d[i] = s[i];
		gpr[HLE_R_V0] = a0;
		return 1;
	}
	case 0x2b: { // memset(dst, c, n) -> dst
		uint8_t* d = hle_ptr(ram, a0);
		uint8_t c = (uint8_t)a1;
		for (uint32_t i = 0; i < a2; i++) d[i] = c;
		gpr[HLE_R_V0] = a0;
		return 1;
	}
	case 0x2c: { // memmove(dst, src, n) -> dst
		uint8_t* d = hle_ptr(ram, a0);
		uint8_t* s = hle_ptr(ram, a1);
		uint32_t n = a2;
		if (d < s) { for (uint32_t i = 0; i < n; i++) d[i] = s[i]; }
		else { for (uint32_t i = n; i-- > 0;) d[i] = s[i]; }
		gpr[HLE_R_V0] = a0;
		return 1;
	}
	case 0x2d: { // memcmp(s1, s2, n) -> int
		const uint8_t* p1 = hle_ptr(ram, a0);
		const uint8_t* p2 = hle_ptr(ram, a1);
		uint32_t n = a2;
		int r = 0;
		for (uint32_t i = 0; i < n; i++) if (p1[i] != p2[i]) { r = (int)p1[i] - (int)p2[i]; break; }
		gpr[HLE_R_V0] = (uint32_t)r;
		return 1;
	}
	case 0x2e: { // memchr(s, c, n) -> ptr | 0
		uint32_t p = a0;
		uint8_t c = (uint8_t)a1;
		for (uint32_t i = 0; i < a2; i++) {
			if (hle_r8(ram, p + i) == c) { gpr[HLE_R_V0] = p + i; return 1; }
		}
		gpr[HLE_R_V0] = 0;
		return 1;
	}
	case 0x2f: { // rand() -> int (0..0x7FFF)
		s_rand_seed = s_rand_seed * 0x41C64E6D + 0x3039;
		gpr[HLE_R_V0] = (s_rand_seed >> 16) & 0x7FFF;
		return 1;
	}
	case 0x30: { // srand(seed) -> void
		s_rand_seed = a0;
		gpr[HLE_R_V0] = 0;
		return 1;
	}

	case 0x51: { // loadAndExec(filename, stackStart, stackSize) -> (does not
		// return; transfers control to the loaded EXE). OpenBIOS loadAndExec
		// (kernel/psxexe.c) -> exec (kernel/psxexec.s): load the EXE, then set
		// stack_start/size from the args, clear BSS, set $gp=header.gp,
		// $sp=stackStart+stackSize, jump to header.pc with $a0=1,$a1=NULL.
		// We load it here and stash the entry context; HleSyscallHook installs
		// the regs + redirects PC (the call must not return to its caller).
		char fname[64];
		uint32_t p = a0;
		uint32_t i = 0;
		for (; i < sizeof(fname) - 1; i++) {
			uint8_t c = hle_r8(ram, p + i);
			if (!c) break;
			fname[i] = (char)c;
		}
		fname[i] = '\0';

		uint32_t pc = 0, gp = 0, exe_stack_top = 0;
		if (!psxport_hle_load_exe(ram, fname, &pc, &gp, &exe_stack_top)) {
			fprintf(stderr, "[hle] LoadExec FAILED to load '%s'\n", fname);
			gpr[HLE_R_V0] = 0; // exec returns 1 on success; 0 = failure
			return 1;          // handled (returns to caller, which handles failure)
		}
		// exec(): if the EXE header carries no stack (stack_start==0 in the
		// header), use the caller-supplied stackStart+stackSize (a1+a2). The
		// game passes a1=0x80200000, a2=0 here (verified). loadAndExec overrides
		// header.stack_start/size with the args BEFORE exec, so the args win when
		// non-zero; fall back to the EXE's own stack top otherwise.
		const uint32_t arg_stack = a1; // stackStart
		const uint32_t arg_size  = a2; // stackSize
		uint32_t sp;
		if (arg_stack) sp = arg_stack + arg_size;
		else if (exe_stack_top) sp = exe_stack_top;
		else sp = 0x801FFFF0u;
		s_exec_pending = true;
		s_exec_pc = pc; s_exec_gp = gp; s_exec_sp = sp;
		fprintf(stderr, "[hle] LoadExec '%s' -> pc=%08X gp=%08X sp=%08X\n",
		        fname, pc, gp, sp);
		gpr[HLE_R_V0] = 1; // exec returns 1 (success) — though it won't return here
		return 1;
	}

	// Heap services (malloc/free/calloc/realloc/InitHeap, 0x33-0x39) and the
	// A0 kernel-table functions (_96_remove 0x72, EnqueueCdIntr 0xA2) are
	// stateful: route them to the kernel module (returns 0 if it can't handle
	// it either, so genuinely-unknown calls still get logged upstream).
	default:
		return psxport_hle_kernel('A', fnum, gpr, ram);
	}
}
