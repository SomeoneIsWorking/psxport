# 0177: a poly-line packet ended its bucket walk and decoded to no primitive

Status: fixed (2026-10-23). Owner: `runtime/psx/present/gp0_primitive_decode.*`, `runtime/psx/present/packet_chains.cpp`.

## Observed
Tomba's A08 node strip FUN_80117E84 links a semi-transparent 3-point poly-line per window (`line_strip.h`). Walking
its OT bucket, the chain stopped at the first poly-line packet: `PacketChainWalker::walk` returned on the packet and
none of the segments reached the sink. A producer that keys the node had no drawn primitive to place, and the
record's poly-line packets stayed unkeyed.

## Cause
`decodePacketPrimitive` reported a poly-line (GP0 0x48/0x58) as `std::nullopt`, which the walker reads as "not a
primitive": it stops the chain and reports the packet. So a packet the GPU draws as several line segments decoded to
nothing.

## Proper fix
Decode a poly-line as the segments the GPU draws: the first point to each later point through the terminator word,
each a line primitive carrying the place of its own position word (`decodePlacedLineEnds`, `polyLinePoint`,
`lineSegment`, `decodePolyLine` in `gp0_primitive_decode.h`). `PacketChainWalker::walk` emits each segment through the
sink under the current draw mode; an unterminated packet still reports the packet.

## Companion
`EmissionScope::amend` and `PacketAmendScope` (`emission_scope.h`, `emit_memory.h`) let a producer rewrite an
already-written packet in place under the packet's own key (`keyFor`): a rewrite of the colour word would otherwise
unbind the command word it shares with, and the record would be shown whole.
