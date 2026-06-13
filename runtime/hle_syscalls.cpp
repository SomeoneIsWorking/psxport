// psxport HLE BIOS — stateless A0-table syscalls (libc string/mem + rand).
//
// These are the pure, side-effect-free standard-library routines the PSX BIOS
// exposes on the A0 table. They operate entirely on PSX main RAM through `ram`;
// stateful services (heap 0x33-0x39, events, threads, file I/O) are owned by
// the core and are NOT handled here. Function numbers below are the standard,
// stable A0 table indices.

#include "hle_bios.h"

// PSX BIOS LCG. rand()/srand() share this seed; the exact recurrence and the
// >>16 & 0x7FFF extraction matter for determinism (must match real hardware).
static uint32_t s_rand_seed = 1;

// Translate a PSX virtual address to a host pointer into the 2 MiB RAM. All the
// byte-wise loops below stay within RAM, so plain pointer arithmetic is safe.
static inline uint8_t* hle_ptr(uint8_t* ram, uint32_t a) { return ram + (a & 0x1FFFFF); }

int psxport_hle_syscall(char table, uint32_t fnum, uint32_t* gpr, uint8_t* ram) {
	if (table != 'A') return 0;

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

	// Heap services (malloc/free/InitHeap, 0x33-0x39) are owned by the core:
	// report unhandled so it can route them.
	default:
		return 0;
	}
}
