# 0187 — An SPU DMA over 64K words was cut at the staging buffer

Status: fixed (2026-10-28). Owner: `runtime/psx/core/mem.cpp` (the DMA4 CHCR write).

## Observed
Spyro 1's oracle route (`psx-spyro/tools/oracle_compare.py --frame-step 1`): at `settled_play` the product's SPU RAM
0x41010..0x6FFFF was all zero and the console's held the level sample bank. The bank goes up in DMA4 blocks of more
than 0x10000 words at SPU address 0x1010.

## Cause and fix
The DMA4 handler clamped the block to `dmaBuffer_`'s 0x10000 words and dropped the rest. A transfer now streams
through the buffer in buffer-sized pieces, both directions; the executable-write notification covers the whole
block.

The DMA0/DMA1 (MDEC in/out) starts carried the same 0x10000-word clamp with no buffer behind it: they pump through
`mdec_dma_pump` and never touch `dmaBuffer_`; the clamp is deleted. DMA3 (CD-ROM) clamped too while computing the
chain-end MADR from the whole count; a block now streams through the buffer (the FIFO, then controller zeros) and a
chain reads each node into the buffer's start. No DMA start in `mem.cpp` clamps a block now.

## Shown
`psx-spyro/scratch/player-bugs-4/probe/spu_ram.py`: after the fix the two cores' SPU RAM differs only in
0x000..0x7FF, the capture buffers the SPU rewrites every sample.
