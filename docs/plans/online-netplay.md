# Online netplay

The user, 2026-11-11: "how can we integrate zerotier support for my multiplayer games ... OpenSU, Little Fighter,
X-Men, whatever else that is multiplayer capable, like Crash Bash and others".

The PSX titles with more than one pad (Crash Bash, CTR, Tekken 3 and the others) are local multiplayer only: the
guest has no network code to point at ZeroTier. They get input netplay, written once in psxport for every title:
each player's pad on their own machine, pads exchanged per guest frame, every peer running the same machine.

## What psxport must guarantee

- **Deterministic stepping.** The same boot image, settings that reach the guest, and pad sequence give the same
  guest state on every peer, frame for frame. CD reads are instant and synchronous by design, so disc timing does
  not vary; host wall clock, frame pacing, fps60 replays, ires and the sink never reach guest state (the fps60
  on/off leak proof already holds this for presentation).
- **Complete savestates.** Whole-machine save and load (S023) restores everything the next frame reads, cheap
  enough to take every frame; a state's hash compares two peers.
- **Input at the pad boundary.** The guest reads each port's pad through the one input owner
  (`runtime/psx/input/`, the `.pad` input phase); netplay supplies the pads for a frame there, the same seam
  `pad_record_replay` uses.
- **Desync detection.** Peers exchange a per-frame state hash; the first mismatching frame is reported with both
  states saved.

## Design

- **Transport.** UDP over any IP path: LAN, a typed address, a ZeroTier or other virtual-network address. No
  matchmaking, lobby or relay service exists or is assumed; one peer hosts, the others join by address, and LAN
  peers are found by broadcast. Interface enumeration (which address is the ZeroTier one) comes from the Lucent
  owner the xmen2 agent is adding.
- **Rollback.** Each peer runs ahead on predicted remote pads (the last one received), saves a state per frame and,
  when a late pad differs, loads the state of that frame and re-steps to the present with audio and presentation
  suppressed. The guest's logic rate is untouched (30 Hz titles stay 30 Hz).
- **Lockstep fallback.** A title whose frame cannot be re-stepped cheaply or deterministically enough waits for
  every peer's pad before each frame, with a fixed input delay.
- **Session.** The host sends the image identity, settings that reach the guest and a start state; a peer with a
  different image is refused by name.

## Phases

1. Determinism proof: two processes, one `.pad`, identical per-frame state hashes over a Crash Bash 4-player match;
   every divergence traced to its owner and fixed.
2. Per-frame savestate cost measured in load-independent units (instructions, bytes) and cut until one frame's
   save plus one re-stepped frame fit beside a normal frame.
3. Pad exchange and lockstep over loopback, then LAN, then a ZeroTier address.
4. Rollback with prediction; desync reports.
5. Session setup in the in-window UI (host, join by address, LAN list).

The first title is Crash Bash, 4 players, one pad per machine. Mega Man X4's LAN co-op (its `docs/plans/`) uses
this owner rather than its own transport.
