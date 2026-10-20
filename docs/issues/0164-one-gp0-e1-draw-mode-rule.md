# 0164: three copies of the GP0(E1) draw-mode rule disagreed on what it applies to

Status: fixed in psxport and Crash Bash; Spyro's copy moves onto it in B4

## Observed
`present/host_ordering_table.cpp` applied the last E1 texpage to every primitive but a polygon, Spyro's
`guest_memory::DrawMode` to every untextured primitive, Crash Bash's `decodePacket` likewise. An untextured
semi-transparent polygon took E1's blend mode in Spyro and not in Toy Story 2 or Tomba; a textured sprite
the reverse.

## Cause
No owner for the rule, so each packet walker wrote its own.

## Fix
`psx::present::DrawModeTracker` (`present/gp0_primitive_decode.*`) takes GP0(E1) packets and applies
the texpage to everything except a textured polygon, which carries its own: the hardware rule.
`PacketChainWalker` (`present/packet_chains.*`), the one packet-chain walk, uses it. Toy Story 2's and
Tomba's host-table renders now give an untextured polygon the E1 blend mode. Crash Bash's `decodePacket`
applies the mode words leading a packet through the tracker, so a textured sprite after one takes its page.
