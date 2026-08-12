// producer_db.cpp — the graphics-producer DB's run lifecycle. See producer_db.h for WHY this is a
// separate, port-callable entry point rather than inline in native_boot's frame loop: a port that owns its
// own loop (spyro, spider1) never reached the inline version, so the DB silently emitted nothing at all.
#include "producer_db.h"
#include "core.h"
#include "game.h"
#include "override_registry.h"   // overrides::query — per-row ownership for the JSONL
#include "config_vars.h"         // cv_producers_dir / cv_producers_db
#include "ot_attr.h"
#include <lucent/log.h>
#include <stdio.h>
#include <string>
#include <time.h>
#include <sys/stat.h>

void producer_db_begin(Core* c) {
  // Load the accumulated claim set BEFORE the first frame, so the guest leg can resolve from frame 0.
  // This is what makes the producer DB a COMPARISON rather than two disjoint row sets: no single leg runs
  // both halves (pc_render never GP0-executes the guest packets; psx_render never runs a native producer),
  // so the addresses natives key are earned on one leg and consumed on the other.
  const std::string db = psx::config::cv_producers_db.get();
  char def[512];
  if (db.empty()) snprintf(def, sizeof def, "%s/claims.txt", psx::config::cv_producers_dir.get().c_str());
  c->rsub.census.loadClaims(db.empty() ? def : db.c_str());
}

void producer_db_finish(Core* c) {
  lucent::info("producers",
    "run-end: claim resolution (SPANS, not prims — a span is a coalesced run of packet-pool stores, and "
    "many spans back one prim, so these are NOT the row counts below and must not be read as a per-prim "
    "join rate) — {} span(s) joined to a native row, {} with no claimed frame in the top {} "
    "(no native producer), {} unresolvable because the claim set was still EMPTY (pure psx_render leg, or "
    "before the first native producer ran); {} claim(s) in the set; {} found AT the search limit{}",
    c->rsub.otAttr.claimResolved(), c->rsub.otAttr.claimUnresolved(), OtAttr::CLAIM_SEARCH_DEPTH,
    c->rsub.census.claimResolveTooEarly(), c->rsub.census.claimCount(), c->rsub.otAttr.claimAtLimit(),
    c->rsub.otAttr.claimAtLimit() ? " — WIDEN CLAIM_SEARCH_DEPTH: a claim at the limit means deeper ones are being missed" : "");
  c->rsub.census.report("run-end");
  // `PSXPORT_DEBUG=otchain` — the chain report, tested against THE CLAIM SET: every guest-keyed row a
  // NATIVE producer has claimed (primsNative > 0). That set is the right one because the open question is
  // whether the two legs can be joined, and a row only joins if a native producer keyed it. Rows with a
  // guest key but no native prims are exactly the effects with NO native producer — they are not claims,
  // and counting them as such would make the report say "joined" about a row that has nothing to join to.
  if (g_otchain_channel) {
    // THE SAME SET THE RESOLVER USES, not a re-derivation. This block used to build its own list from
    // this run's census rows (guest-keyed with primsNative > 0), which is a DIFFERENT set from the
    // persisted one resolveClaimedFrame consults — so on a guest leg the report printed "0 of 29 shapes
    // claimed" while the resolver was joining 266,760 spans against 10 loaded claims. It refused loudly
    // instead of inventing a number, which is the only reason the split was findable; a report and the
    // mechanism it reports on must still read from ONE source.
    static constexpr int CLAIM_REPORT_CAP = 512;
    uint32_t claims[CLAIM_REPORT_CAP];
    int nclaims = 0, skipped = 0;
    for (int i = 0; i < c->rsub.census.claimCount(); i++) {
      if (nclaims < CLAIM_REPORT_CAP) claims[nclaims++] = c->rsub.census.claimAt(i); else skipped++;
    }
    if (skipped)
      lucent::warn("otchain", "claim set TRUNCATED for the report: {} claim(s) past CLAIM_REPORT_CAP={} "
                              "— an UNCLAIMED verdict below may be this truncation, not a real miss",
                   skipped, CLAIM_REPORT_CAP);
    c->rsub.otAttr.reportChains(claims, nclaims);
  }
  // Persist the OBSERVED half so the DB survives the run and can reach git through the game's
  // tools/producers.py ingest (USER: populated by playing, and tracked). Path is a knob so a harness can
  // separate its runs; the default lands in the gitignored scratch/ tree, never /tmp.
  {
    const std::string dirs = psx::config::cv_producers_dir.get();
    const char* dir = dirs.c_str();
    char stamp[32];
    { const time_t t = time(nullptr); struct tm tmv{};
#ifdef _WIN32
      localtime_s(&tmv, &t);
#else
      localtime_r(&t, &tmv);
#endif
      strftime(stamp, sizeof stamp, "%Y-%m-%dT%H:%M:%S", &tmv); }
    char path[512];
    snprintf(path, sizeof path, "%s/run-%s.jsonl", dir, stamp);
    // Create the directory rather than failing on a fresh clone; the writer REPORTS a failed open.
#ifdef _WIN32
    _mkdir(dir);
#else
    mkdir(dir, 0755);
#endif
    // Pass the override registry's own query so each row carries whether its guest address is
    // override-installed and whether that native ever ran. Without it the DB's ownership fields are
    // written as "unavailable" rather than a misleading false.
    c->rsub.census.writeJsonl(path, stamp, &overrides::query);
    // Persist the claim set so the NEXT run can join legs this one structurally cannot (see
    // ProducerCensus::loadClaims): no single leg runs both halves, so the join is cross-run by nature.
    char cpath[512];
    snprintf(cpath, sizeof cpath, "%s/claims.txt", dir);
    c->rsub.census.appendClaims(cpath);
  }
}
