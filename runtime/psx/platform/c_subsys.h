// C-linkage declarations for the leaf subsystems still compiled as C (the mdec Beetle adapter) plus the vendored Beetle
// MDEC clock pump. The OOP refactor made most of the runtime C++; these few stay C, so the C++ callers must see their
// symbols with C linkage. Include this instead of scattering local (C++-mangled) forward decls.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// mdec_beetle.c + vendored mednafen mdec.c
void mdec_init(void);
void mdec_write(uint32_t addr, uint32_t val);
uint32_t mdec_read(uint32_t addr);
int mdec_dma_in(const uint32_t *words, int count); // returns words actually fed (short = deferral)
int mdec_dma_out(uint32_t *buf, int count);
int mdec_dma_out_rest(uint32_t *buf, int count);
uint32_t mdec_dma_read_word(uint32_t *offs); // pop one OutFIFO word + its scatter displacement
void mdec_step(void);                        // run the decoder until it parks at a FIFO gate
bool mdec_dma_can_write(void);
bool mdec_dma_can_read(void);
void MDEC_Run(int32_t clocks);

#ifdef __cplusplus
}
#endif
