// native_diff.h — verify a NATIVE reimplementation against the recompiled body it replaces, per call.
//
// THE PROBLEM THIS SOLVES. A port only becomes a PC port when native code REPLACES guest bodies. But
// a replacement you cannot check is a guess, and this project's own rule is to verify on real data
// rather than trust that green means correct. psxport's answer is the SBS harness — two Cores run in
// lockstep, one all-substrate, one native, compared each frame. That harness is whole-run, needs a
// native frame loop, and its stepper hardcodes the first consumer's addresses, so a second port
// cannot use it without a framework generalisation. The result is a chicken-and-egg: you cannot own
// a function until the harness exists, and the harness is a large piece of work.
//
// A PER-CALL differential needs none of that, and for validating ONE replacement it is stronger:
// instead of asking "did a whole run diverge", it asks "does this function, from THIS exact input
// state, produce exactly what the recompiled body produces" — and answers it on every call.
//
//   1. snapshot guest RAM + scratchpad + the register file + the COP2/GTE register file
//   2. run the NATIVE body; capture the resulting RAM + registers
//   3. restore the snapshot; run the RECOMPILED body; capture again
//   4. compare. Any difference is reported with the address, both values, and the register.
//
// The native result is the one left in place, so a diff run behaves like a native run — it does not
// silently fall back to the substrate, which would make a broken replacement look fine.
//
// Cost is real (two 2 MB copies plus two executions per verified call), so it is OFF unless asked
// for, and bounded: PSXPORT_NDIFF=<n> verifies the first n calls of each site and then runs native
// only. That is usually enough — a replacement that matches its first calls and then diverges is
// almost always state-dependent, which shows up as a diff in those first calls anyway.
//
//   PSXPORT_NDIFF=8            verify the first 8 calls of every ndiff_run site
//   PSXPORT_NDIFF_MAXDIFF=32   stop listing individual byte diffs past this many (per call)
//
// COP2/GTE IS COMPARED TOO, and that was not optional. A PS1 game's hot maths is geometry code whose
// entire effect can be in the GTE register file — none of which is guest RAM. Comparing only GPRs and
// memory would have reported "matches" while a native body left REG[0..63] or FLAGS different, which
// is worse than no check: it is a green light on an unverified replacement.
//
// WHAT IT STILL CANNOT TELL YOU. Equality here means equal for the inputs actually exercised. It does
// not prove equivalence for unexercised paths, and it cannot see divergence in state kept outside the
// Core and the GTE — the host-side GPU render queue, for example. Say so when citing it: "matched the
// substrate on N calls covering X" is a claim; "verified equivalent" is not.
#pragma once
#include <cstdint>
class Core;

// Run `native`, and under PSXPORT_NDIFF also run `body` from the identical pre-state and compare.
// `name` labels the site in the log (use the guest address). Returns true if a difference was found.
bool ndiff_run(Core *c, const char *name, void (*native)(Core *), void (*body)(Core *));

// Has any site reported a divergence this run? For a gate check — a port whose native bodies match
// should be able to assert zero, and a regression then fails loudly rather than drifting.
int ndiff_divergences();

// TRUE while ndiff_run is executing either of its two legs.
//
// WHY ANYONE ELSE NEEDS TO KNOW. The comparison's premise is that both legs start from the identical
// pre-state AND that nothing else runs during either of them. Anything that can inject guest
// execution asynchronously breaks that premise, and it breaks it ASYMMETRICALLY: the native leg is
// straight C with no gate in it, while the recompiled leg is emitted code that polls
// `Core::pending_work` at every function entry and loop back-edge. So an asynchronous injection can
// only ever land in the SUBSTRATE leg, and the difference it makes is reported as a divergence of the
// native body — which is the one thing this harness exists to be trusted about.
//
// MEASURED, on the run that produced this function: with the host frame clock armed
// (rec_host_turn_register), spyro's gate reported 2 divergences whose bytes were the vblank counter
// 0x800749E0, the pad-decoder counters 0x80075760/0x800758C8, the pad buffers at 0x800773Ex and the
// vblank handler's own stack — i.e. every byte a delivered display field writes, and nothing the
// native bodies touch.
//
// Callers DEFER rather than drop: leave the work armed and take it at the next gate, exactly as
// rec_host_turn already does for a guest critical section.
bool ndiff_in_progress();
