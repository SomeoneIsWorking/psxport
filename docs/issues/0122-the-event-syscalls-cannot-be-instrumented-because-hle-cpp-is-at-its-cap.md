---
id: 122
title: The BIOS event syscalls cannot be instrumented because hle.cpp is at its line cap
status: open
symptom: adding one lucent::debug line to the TestEvent case takes runtime/psx/hle.cpp from 784 to 789 lines and fails cpp_style; the file's shrink-only legacy cap is correct and the event syscalls have never been extracted into an owner
tags: architecture,hle,diagnostics,structure
created: 2026-09-19
---

## What happened

Spyro issue 0123 needed to know which event spec a card poll loop was waiting on. `OpenEvent` is
already traced on the `ev` channel; `TestEvent` is not, and it is `TestEvent` that answers the
question -- a title opens several specs and the order it tests them decides which one wins (see the
Spider-Man note above `Memcard::deliverComplete`).

The probe is five lines:

```cpp
    case 0x0B: {
      int i = eventIndex(a0); // TestEvent (read+clear)
      const bool fired = i >= 0 && s_ev[i].fired != 0;
      lucent::debug("ev", "TestEvent handle=0x{:08X} -> {}", a0, fired ? "FIRED" : "not fired");
      if (fired) {
        s_ev[i].fired = 0;
        c->r[V0] = 1;
      } else {
        c->r[V0] = 0;
      }
      return true;
    }
```

It works -- it produced the 61,689-line measurement that named 0123's cause -- and it cannot be
landed, because `hle.cpp` is at 784/784 and the cap is shrink-only. Raising the cap to hold a
diagnostic would be exactly the thing the cap exists to stop.

## What the cap is actually asking for

The event syscalls are a cohesive responsibility that has never been given an owner. In `hle.cpp`
they are cases 0x07..0x0D of one large syscall switch, operating on `Hle::ev[16]`, `Hle::ev_depth`,
`Hle::eventIndex` and `Hle::deliverEvent`, with the array declared as a public data member of a
class whose members are all public.

A `runtime/psx/hle_events.*` owner would hold the control blocks, index lookup, delivery, and the
0x07..0x0D handling behind a narrow interface, and would take roughly 120 lines out of `hle.cpp` --
well clear of the cap, with room for the instrument the next investigator will want.

## Why it was not done in the same change

`deliverEvent` is not a pure data operation: an `EvMdINTR` event is delivered by CALLING the guest
handler through `psx::cpu::dispatchGuest0`, with `ev_depth` guarding re-entry. That makes this
extraction a change to guest-callback dispatch, which is the riskiest path in the runtime, and it
deserves its own gated pass rather than riding along with a log line. The external surface is small
and already known: five `deliverEvent` call sites (`mem.cpp` x1, `memcard.cpp` x4).

## Until then

Reproduce the measurement by applying the five lines above locally, building, and running with
`PSXPORT_DEBUG=ev`. Do not raise the cap.
