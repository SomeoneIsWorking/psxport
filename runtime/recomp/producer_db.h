// producer_db.h — the graphics-producer DB's RUN LIFECYCLE, callable by a port that owns its frame loop.
//
// WHY THIS HEADER EXISTS. The four lifecycle calls (load the claim set, print the run-end report, write the
// per-run JSONL, append the earned claims) lived INSIDE `game_main` in native_boot.cpp. That is the
// framework's own frame loop — and a port that owns its loop never reaches it. MEASURED 2026-08-12:
// `spyro` never calls `native_boot_run` at all, and `spider1` calls it but its `bootInit` dispatches the
// guest main, which never returns. So in both ports the DB emitted NOTHING — no report line, no
// `scratch/producers/` directory, no claims file — while the census itself was armed and being fed. The
// claim "the framework does this automatically" was true only for the one consumer whose boot spine the
// lifecycle happened to be welded to.
//
// THE SILENCE WAS THE WORST PART. Nothing was wrong at run time; there was simply no output, which is
// indistinguishable from "this game draws nothing the DB can see". `ProducerCensus` designs its negatives
// carefully (wasFed / primsSeen / spanMiss / unscopedNative / overflow all exist so an empty table cannot
// read as a clean one) — and all of that care was unreachable. A designed negative that is never PRINTED
// is not a negative, it is an absence.
//
// A PORT WITH ITS OWN FRAME LOOP MUST CALL BOTH:
//     producer_db_begin(c);    // once, BEFORE the first frame — loads the accumulated claim set
//     ... the port's own frame loop ...
//     producer_db_finish(c);   // once, after the last frame — report + JSONL + append claims
// `native_boot.cpp` calls them for a port that uses the framework's loop, so nothing changes for that case.
// Calling `finish` twice is harmless (it re-reports); never calling it is the failure this header exists to
// make nameable.
#pragma once
class Core;

// Loads the accumulated claim set (PSXPORT_PRODUCERS_DB, default <PRODUCERS_DIR>/claims.txt). Safe to call
// when the census will not be fed — loadClaims reports its own refusal when the file is absent.
void producer_db_begin(Core* c);

// Run-end: the claim-resolution accounting, the census report, the per-run JSONL, and the append-only claim
// file. This is the ONLY place the DB becomes durable, so a port that does not call it produces no DB.
void producer_db_finish(Core* c);
