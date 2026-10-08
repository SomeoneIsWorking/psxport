// store_observe — a PRODUCT surface for the dynarec store observer.
//
// WHY THIS EXISTS. `LightrecExecutor::configureStoreObserver` has been implemented and hermetically
// tested (tests/test_dynarec_contract.cpp) and is the only instrument that reports a translated
// GUEST store's guest PC together with the full register file. It also had no way to be armed from a
// product run: `PSXPORT_CW` sees host-side stores only, so no live divergence investigation could ask
// anything about a translated store at all (Spyro 1, docs/issues/0133).
//
// WHAT IT ACTUALLY ANSWERS, because the previous version of this comment claimed the opposite and the
// claim cost a real investigation. The armed list is matched against the PC OF each executed translated
// store — `target.guestPc != guestPc` in `LightrecExecutor::Impl::observeStore` — so the list is STORE
// PCs. Therefore:
//
//   * "did the store instruction at PC X run, how many times, and what did it write?"  YES. This is
//     the instrument's real strength, and it is how a suspected never-executed function is caught: its
//     first `sw $ra, off($sp)` in the prologue is enough.
//   * "which instruction wrote THIS GUEST WORD W?"  NO. The callback receives the store's PC and the
//     register file, not the store's resolved target address, so there is no lookup from a word back to
//     an instruction. Finding the writer of W is a SEARCH over candidate store PCs, not a query.
//
// The old comment said the instrument existed to answer the second question, and the old report line
// said "this address was not written in this run". On 2026-09-27 that was acted on: Spyro 1 armed
// `PSXPORT_STORE_OBSERVE=800700F4,...` — DATA addresses, while investigating why the moby list at
// 0x800700F4 was never filled — got MATCHED NONE, and recorded that the product "never executes a
// translated store to the list base" over 116M instructions. 0x800700F4 is not an instruction, so no
// store instruction can be AT it: MATCHED NONE was guaranteed before the game started, and a
// guaranteed answer was published as a measurement. Every line this module prints now names STORE PC.
//
// The observer itself stays where it belongs, in the executor. This owns only the ARMING and the
// REPORTING: which guest addresses to watch, and what to say about what was seen. A title that wants
// richer handling calls `configureStoreObserver` itself with its own callback; this is the
// configuration-driven path, so an investigation needs no product edit.
//
// COST. Disarmed by default, and disarmed means the observer's own test in the executor does not fire,
// so an ordinary run pays one string compare per configuration and nothing per instruction.
#pragma once

#include "lightrec_executor.h" // StoreObserverReport

class Core;

// Arm the store observer on this Core from configuration, and log what it sees. Idempotent, and safe
// to call from a product's setup: it is the whole product surface.
//
// `PSXPORT_STORE_OBSERVE` is a comma- or space-separated list of GUEST ADDRESSES OF STORE
// INSTRUCTIONS in hex, e.g. `0x80083884,0x8007DB3C`, up to `kMaxObservedStoreTargets` of them. Empty or
// unset disarms, which is the default. An unparsable or over-long list is REFUSED with the offending
// text named, rather than silently watching a prefix of it.
//
// *** THESE ARE STORE PCs, NOT DATA ADDRESSES, AND THE DIFFERENCE IS THE WHOLE USABILITY QUESTION. ***
// `LightrecExecutor::Impl::observeStore` matches `target.guestPc != guestPc` — the pc of the translated
// STORE — so the instrument answers "what does this store instruction write, and with what registers",
// NOT "which instruction wrote this word". Arming a DATA address therefore matches nothing, and the
// zero that follows is not evidence: measured 2026-09-27, arming `0x8007DB3C` and `0x8007DB54` (store
// PCs) produced 24,332 correctly-attributed callback lines, while arming `0x80078AE0` (a data address)
// produced `MATCHED NONE of the 9,366,306 executed JIT instruction(s)` on a word that demonstrably
// changes every frame. A per-target report row echoes the armed value in a column that reads like an
// address, which is what makes the mistake easy. **If you want to know which instruction wrote a word,
// you must already know the instruction** — use `PSXPORT_CW` for host-reaching stores, or read the store
// instruction out of the listing.
//
// *** WHO ARMS IT, AND WHY IT IS NOT OPTIONAL FOR A TITLE ***
// `psx::Machine::attachControlChannel` (machine.cpp) calls this beside `DbgServer::attach`, and
// `native_boot_run` reaches it through the same spine, so a product is armed whichever composition it
// boots with. It used to be reachable only through the framework's own boot line, so a title-owned
// spine was silently unarmed while the boot audit printed `PSXPORT_STORE_OBSERVE = ... [env]` — an
// audit line that says a knob is BOUND, not that anything reads it. Measured 2026-09-27 on Spyro 1:
// `nm -C` showed `store_observe_configure` linked into the product, the audit showed the variable set,
// and a run with `PSXPORT_STORE_OBSERVE=nothex` produced NOT ONE line — not even the
// `refusing PSXPORT_STORE_OBSERVE` error that parsing a bad token emits unconditionally. The only
// silent path is `requested.empty()`, so the reader never ran. That is the same class of defect as a
// title with no live endpoint at all: a diagnostic that cannot be reached is not a diagnostic.
void store_observe_configure(Core &core);

// Log what the observer saw, with the executor's own counters beside it. `StoreObservation` does not
// carry the address that was written, so this per-target table is how several watched words are told
// apart: it reports each one's store counts and the guest PC of its last one. Silent when the
// observer was never armed — the arming line already said so, and an unarmed observer's silence is not
// evidence of anything. Takes the report rather than the Core: `~LightrecExecutor` emits it, and asking
// the Core for its executor from inside that destructor returned a torn-down object (SIGSEGV in Spyro's
// recipe tests, 2026-10-01).
void store_observe_report(const psx::cpu::StoreObserverReport &report);

// A title-owned spine calls THIS once instead of remembering the pair, because forgetting the report is
// how a run ends with the observer armed and nothing said about what it saw — the same silence the
// configuration path is trusted not to have.
void store_observe_attach(Core &core);
