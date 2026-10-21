# 0171: a block store was booked one byte at a time

Status: fixed. Owner: `runtime/psx/core/guest_memory.cpp` `Core::mem_write`.

## Observed
Spyro 1 from boot at real speed with the player's settings (psx-spyro issue 0196, `scratch/judder-pass3/hand0`,
`presentseq`, perf at 2 kHz): the field where boot hands over to the frame loop (fence 437) presented 14.7 ms
late (load 13.0). The main thread spent 53 samples (26.5 ms) in that 30 ms field; 13 of them were store
bookkeeping: `writeGuestMemory<uint8_t>`, `PacketPoolExtent::readFrom`/`contains`, `OtAttr::trackStoreSlow`,
`EmissionScope::noteStore`.

## Cause
`Core::mem_write` (the WAD archive transfer and the stock CdRead landing) looped `mem_w8_unnotified` over the
block, so every byte ran the producer census, the packet-pool bound check, the emission binding and the
rollback test.

## Fix
`Core::mem_write` books a span once: the display-pass guard, the store watch (per byte, only while armed),
`WriteRollback::note` (any width now), `OtAttr::trackStore` over the span (`PacketPoolExtent::overlaps`), and
`EmissionScope::noteStore(GuestSpan)` binding every word it touches; then one copy and one executable-write
notification. A span that is not all host memory (devices, or one leaving its region) still stores per byte.
`writeGuestMemory` shares the same `Core::bookStore`.

## Proof
Same route, `scratch/judder-pass3/hand1` (load 17.7..22.9): fence 437 4.3 ms late; 30 main-thread samples in
the field, one in store bookkeeping (`memmove` of the block). The rest of that field is Lightrec's first
translation of the loaded code (about 12 samples) and the image's SHA-256 (2..3). With the host this loaded the
lateness figures are no pacing evidence; the sample counts are.
