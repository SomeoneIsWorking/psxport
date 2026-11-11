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
- **Embedded ZeroTier.** Online setup is automatic: netplay's UDP goes through the optional Lucent ZeroTier
  component (libzt, built by the xmen2 agent) when it lands, never through hand-rolled NAT traversal. It joins a
  controller-less ad-hoc network with no account or install; a player's code is their 10-hex-digit node ID, and
  ZeroTier's public roots relay when no direct path exists (ZeroTier's infrastructure, not ours). Ad-hoc networks
  are public and IPv6 unicast only, so the session authenticates its peers itself. License: libzt's `LICENSE.txt`
  is BSL 1.1 with Change Date 2026-01-01 to Apache 2.0, and its pinned ZeroTierOne (`ext/ZeroTierOne` `c53c6bd9c3`,
  1.4.4) is BSL 1.1 with Change Date 2025-01-01 to Apache 2.0, so both are Apache 2.0 now; lwIP is BSD. Apache 2.0
  ships beside MIT with its NOTICE and license text in the distribution. A newer revision can carry a later Change
  Date, so the pin stays at a revision whose Change Date has passed. Mega Man X4's LAN co-op uses the same path.
- **Rollback, in Lucent.** The user, 2026-11-11: "Lf2 would probably need a new rollback netcode ... and anything
  that doesn't have its own multiplayer ofc also gets rollback netcode". So the rollback engine is title-neutral and
  lives in Lucent as one implementation: input exchange, prediction (the last pad received), rewind and resimulate,
  an input-delay setting, per-frame state-hash desync detection, and spectating if cheap. A runtime supplies four
  hooks: save state, load state, advance one frame, set the pads. psxport implements them over whole-machine
  savestates (S023) and the pad input phase, re-stepping with audio and presentation suppressed; the guest's logic
  rate is untouched (30 Hz titles stay 30 Hz). psxport is the first consumer and builds the Lucent component when
  this plan reaches phase 4; LF2 is the second. Prior art first: GGPO (MIT since 2019) and its maintained forks,
  checked against the four hooks; a fork that fits is adopted, pinned by revision. Its datagrams go through the
  Lucent ZeroTier component's sockets, never its own UDP.
- **Engine API (provisional until the GGPO check).** `lucent::rollback::Session`, built from a config of slot count,
  input bytes per slot (PSX pad: 2; LF2: 1), input delay and maximum rollback frames, over a runtime interface:

  ```cpp
  namespace lucent::rollback {
  class Runtime {
  public:
    virtual ~Runtime() = default;
    virtual void save(int slot) = 0;                              // keep this frame's state in ring slot `slot`
    virtual void load(int slot) = 0;                              // restore ring slot `slot`
    virtual void setInputs(std::span<const std::uint8_t> all) = 0; // slots x input bytes, slot-major
    virtual void advance(bool resimulating) = 0;                  // one logic frame; no audio/present while resimulating
    [[nodiscard]] virtual std::uint64_t hash() const = 0;         // state hash for desync detection
  };
  }
  ```

  The runtime owns its state storage (psxport whole-machine states, LF2 dirty-page snapshots); the engine only names
  ring slots in `[0, maxRollback]`. LF2 confirmed the interface (8 slots x 1 byte, `advance` = one guest frame).
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
4. Rollback: the Lucent engine (GGPO fork or our own after the prior-art check) with psxport's four hooks;
   desync reports. Tell the lf2 agent the component's name when it exists.
5. Session setup in the in-window UI (host, join by address, LAN list).

The first title is Crash Bash, 4 players, one pad per machine. Mega Man X4's LAN co-op (its `docs/plans/`) uses
this owner rather than its own transport.
