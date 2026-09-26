// store_observe — a PRODUCT surface for the dynarec store observer.
//
// WHY THIS EXISTS. `LightrecExecutor::configureStoreObserver` has been implemented and hermetically
// tested (tests/test_dynarec_contract.cpp) and is the only instrument that reports a translated
// GUEST store's guest PC together with the full register file. It also had no way to be armed from a
// product run: `PSXPORT_CW` sees host-side stores only. So the one question it exists to answer — which
// instruction wrote this guest word — was unaskable outside a unit test, and a live divergence
// investigation in a real title was blocked on it (Spyro 1, docs/issues/0133: the product executed a
// store to `g_Spyro + 0x88` that the reference did not, and nothing could name the instruction).
//
// The observer itself stays where it belongs, in the executor. This owns only the ARMING and the
// REPORTING: which guest addresses to watch, and what to say about what was seen. A title that wants
// richer handling calls `configureStoreObserver` itself with its own callback; this is the
// configuration-driven path, so an investigation needs no product edit.
//
// COST. Disarmed by default, and disarmed means the observer's own test in the executor does not fire,
// so an ordinary run pays one string compare per configuration and nothing per instruction.
#pragma once

class Core;

// Arm the store observer on this Core from configuration, and log what it sees. Idempotent, and safe
// to call from a product's setup: it is the whole product surface.
//
// `PSXPORT_STORE_OBSERVE` is a comma- or space-separated list of guest addresses in hex, e.g.
// `0x80078AE0,0x80076B80`, up to `kMaxObservedStoreTargets` of them. Empty or unset disarms, which is
// the default. An unparsable or over-long list is REFUSED with the offending text named, rather than
// silently watching a prefix of it — a diagnostic that quietly watched less than it was told to is the
// failure mode this whole area has already produced once.
void store_observe_configure(Core &core);

// Log what the observer saw, with the executor's own counters beside it. `StoreObservation` does not
// carry the address that was written, so this per-target table is how several watched words are told
// apart: it reports each one's store counts and the guest PC of its last one. Silent when the
// observer was never armed — the arming line already said so, and an unarmed observer's silence is not
// evidence of anything.
void store_observe_report(Core &core);
