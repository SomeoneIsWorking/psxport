# Per-game GRAPHICS PRODUCER DB — orientation + design

**Status: A PLAN. Nothing here is implemented.** Written 2026-08-11, orientation pass only; no
framework file was edited and no `coord/claims/` lock was taken. Companion doc:
`docs/plans/render-path-tristate.md` (the three-way render toggle that makes this DB's central
comparison *observable*).

Every citation below is `file:line` in a psxport checkout at `d6b8e17d` (all three trees were on that
gitlink, clean, when this was written) or in a game repo at its 2026-08-11 HEAD.

---

## The ask (USER, 2026-08-11)

> *"I need a DB for each game to keep track of native graphics producers, framework should do this
> automatically, when the game renders an effect, if it's not in the DB then a DB entry gets created
> and we need to track if it has a native producer equivalent. Some info regarding could be, does it
> have native producer, is the effect RE'd yet etc. basically comparing GTE/OT against native
> producers"*

## It is populated by ORDINARY PLAY, and it is IN GIT (USER, 2026-08-11)

> *"this 'DB' I talked about should also be populated when I'm playing then I can tell you something
> like 'work on the DB entries'"* · *"And it should be in the git"*

That is a design constraint, not a nice-to-have, and it decides four things:

1. **The census is ON by default in a normal windowed run.** It is not a diagnostic you opt into — the
   user's own play session is the primary data source. So stage 2's cost measurement is not a
   formality: if the always-on feed cannot be made free enough, the fallback is a cheaper feed, NOT an
   opt-in flag that would leave ordinary play uncounted. And whatever the outcome, the run says which
   it was, because a DB that silently stopped being fed is worse than none.
2. **Ingest happens automatically at exit, and survives a kill.** Observations are appended to
   `scratch/producers/run-<stamp>.jsonl` as they are first seen (not buffered to the end), and
   `tools/producers.py ingest` runs from `run.sh`'s tail AND at the start of the next run — so a
   session that ended in a crash, a watchdog abort, or a `kill` still lands its rows.
3. **The tracked rows must not churn on every play session.** The volatile per-run numbers stay in
   `scratch/`; the tracked row carries only slow-moving facts — `first_seen`, `last_seen` (date),
   `runs`, `prims_max`, the union of `sub_signatures`, and the derived `has_native`. After a few
   sessions a play run's `git diff` is *new producers and nothing else*, which is what makes the diff
   readable as "here is what I found while playing".
4. **"Work on the DB entries" has to be answerable as a ranked queue.** `producers.py report --todo`
   orders rows by what is worth owning next: seen in the most sessions × most prims × no native
   producer, `re_status: unknown` first, and rows whose `has_native=1` but `native_reached=0` (we
   think we own it and it never ran) flagged separately — that combination is a lie in the DB and
   outranks new work.

## What the DB is FOR — the axis no existing registry has

Three registries already exist per repo and each answers exactly one question:

| registry | question | what it keys on |
|---|---|---|
| `docs/codemap.md` (`tools/codemap.py`) | what code EXISTS, and its status | files / subsystems |
| `docs/issues/` (`tools/catalog.py`) | has this been hit or ruled out before | symptoms |
| `docs/re-frontier.md` (`tools/re_frontier.py`) | which RE step is REAL vs a HACK | ordered steps |
| `overrides::install` table (`override_registry.h:63`) | which guest ADDRESSES we own | addresses |

None of them is keyed on **what the game actually draws**. `overrides::coverage`
(`override_registry.h:83`) comes closest and is still the wrong denominator: it reports "of the
addresses we OWN, how many did this run reach" — a fraction whose denominator is *our own ambition*.
The missing number is the opposite fraction: **of the picture-producing work the game DID this run,
how much does the PC produce natively?** That denominator is discovered by RUNNING, never by reading
our own source, and nothing in the workspace records it.

The concrete cost of not having it, measured today: spyro has exactly **one** native producer
(`spyro/docs/info/claims/167-*.md`, stage 13's front-end sprite layer) and **no way to state what
fraction of a frame that is**. Tomba2Engine has 68 `.cpp` files under `game/render/` and likewise no
per-effect ledger — `docs/gfx-debug.md` lists ~8 hand-written per-producer debug channels
(`beamfx`, `plumefx`, `heads0`, `cullpush`, …), each invented ad hoc for one effect, each with its
own hand-maintained denominator line. **That per-effect table is the DB, currently living as prose in
a doc and as `if` statements in eight producers.**

## What already exists to build on — the feed is 90% present

The attribution machinery this DB needs was built for a different purpose (bug #45's batching
question) and is already wired end to end:

| piece | where | what it gives us |
|---|---|---|
| otattr shadow stack | `interp_diag.h:69-75` | the innermost INDIRECTLY-dispatched guest fn — pushed/popped unconditionally in `rec_dispatch`, **not** channel-gated, so it costs nothing to read |
| packet-pool store spans | `ot_attr.h:75-99` | `[lo,hi) -> {emitter fn, caller fn, node}` for every guest store into the packet pool, coalesced. `lookupStore(addr)` is the answer-lookup |
| per-word GP0 source address | `gpu_native_internal.h:132`, stamped at `gpu_native.cpp:2091` (OT walk) and `:2150` (block DMA) | the guest address each GP0 word came from — **the join key** between a rasterized prim and the span table |
| per-prim word addresses | `gpu_native.cpp:1332` (`s_fifo_addr[]`) | at prim completion, `s_fifo_addr[0]` is the packet header's guest address |
| the attribution DENOMINATOR, already counted | `gpu_native.cpp:1331` (`s_gp0_addressed` / `s_gp0_anon`) | how many GP0 words could not be attributed at all (direct register writes, FMV/block uploads). Already exists, already counted, for exactly the reason this doc's honesty section demands |
| GTE RTPS/RTPT per-(fn,node) counts | `ot_attr.h:93` (`trackGte`) | the "GTE side" of the comparison, aggregated per producer per frame |
| render-walk object scope | `render_diag.h` (`beginObject`/`currentNode`) | which live entity node was being rendered |
| native-side submission chokepoints | `render_queue.h:213` `emitItem`, `:218` `emitOrQueue`, `:204` `push2dQuad`, `:231` `drawWorldQuad` | every native producer's prims funnel through here |
| the RAII declaration pattern to copy | `render_queue.h:189` (`Space2dScope`) | precedent for a producer scope: restores the PREVIOUS value, never resets to a default |

**The one thing genuinely missing is the join and the ledger**, plus a native-side identity
declaration. There is no new instrumentation to invent.

**Blocker to design around, stated plainly:** the span table's feed
(`OtAttr::trackStore`, `ot_attr.h:87`) is gated on the `otattr` lucent channel, and that gate is
inline *because it runs on every guest store* — the out-of-line version measured **2.54% of total
CPU doing nothing** (`ot_attr.h:83-85`). A census that must be on for every run cannot simply flip
that gate on. See stage 2.

---

## The identity decision: what IS "an effect"

**Primary key: the guest SUBMITTER FN address** — `InterpDiag::otattrTop()` at the moment the packet
was written, i.e. exactly what `OtAttr::Span::fn` already records.

Why this key and not the alternatives:

- **not a curated effect NAME.** A name cannot be minted by the framework at run time, so a
  name-keyed DB cannot satisfy "if it's not in the DB then a DB entry gets created". Names are a
  CURATED field on a mechanically-keyed row.
- **not the prim's material signature** (op / texpage / CLUT / bbox class). Two unrelated effects
  share a texpage constantly, and one effect legitimately spans several; the signature is a
  *sub-facet* worth recording inside a row, never the row's identity.
- **not the entity node** (`RenderDiag::currentNode`). That is an INSTANCE, not a producer — 45 nodes
  of one effect are one row, and the node is a per-observation detail.
- **the key must live in the SAME space as `overrides::install`'s key (a guest address)**, because
  that is what makes "does it have a native producer" a *mechanical* answer rather than a curated
  claim. A row whose fn is in the override table with a `native` handler HAS a native producer, and
  no human has to remember to tick a box.

**Sub-rows, where one fn is honestly several effects:** an entry may carry observed sub-signatures
(`{op class, texpage, clut, mode, semi}` tuples with counts). Splitting a row into two curated
effects is a hand edit that records the discriminator it split on. The `plumefx` producer is the
known case — one guest fn (`FUN_8002BC9C`) whose subtype byte `0x14/0x15` reaches a *different*
emitter family with no port (`Tomba2Engine/docs/gfx-debug.md`). A single row for that fn would report
"native producer: yes" over a branch that has none. **That is the failure mode the sub-signature
field exists to prevent, and it is real, not hypothetical.**

**Native-side identity:** a native producer declares itself with an RAII scope modelled on
`Space2dScope` (`render_queue.h:189`) —

```cpp
{ RenderQueue::ProducerScope p(rq, kProducerTitleMenuSprites);   // interned id + guest fn it ports
  ...pushes... }
```

The declaration carries **the guest fn it is a port OF**, which is what lets the native leg's prims
be counted against the same row as the guest leg's. A native producer that declares no guest fn is
legal (a PC-only enhancement) and is recorded as such — it just never participates in the compare.

---

## Design the NEGATIVE first (global CLAUDE.md rule, and this instrument has four silences)

A per-effect coverage report that can print "0 unported producers" is worthless unless each zero is
distinguishable from "I never looked". Every report line carries its denominator:

1. **`gp0Anon`** — GP0 words with no source address (`gpu_native.cpp:1331`). These prims are
   *permanently unattributable*, not "produced by nobody". Already counted; the report prints it.
2. **`spanMiss`** — prims whose `s_fifo_addr[0]` fell in no recorded span. Means the span feed was
   OFF or overflowed (`OtAttr::spanOverflow`, `ot_attr.h:98`), NOT that the prim has no producer.
3. **`nativeLegNotRun`** — the census ran with the render path in a mode where native producers are
   structurally silent. `PSXPORT_GATE=1` runs the `gen` body for **all 482** registered addresses
   (measured 2026-08-06, `Tomba2Engine/docs/gfx-debug.md`), so a native-leg census under GATE
   measures zero by construction. The report must name the render path and the gate state on every
   line. This is instrument I040's failure, one level up.
4. **`reachedThisRun`** — the DB describes the frames this run PLAYED. A producer absent from a run
   is `not-reached`, never `absent`. Same disease `overrides::coverage` was written to cure
   (`override_registry.h:75-82`).

**And a `--selftest` that MUST produce a positive**, wired into ctest: feed a synthetic packet-pool
store from a known fn, walk a one-node OT over it, assert the census attributes exactly that prim to
exactly that fn — and assert the four counters above are zero on that case. A census tool that has
never been shown to fire is the same bug the DB exists to catch.

---

## Schema — one row per producer, OBSERVED and CURATED strictly separated

The split is the whole point: a run may rewrite every observed field and must never touch a curated
one.

```
key        guest submitter fn address (e.g. 0x8002BC9C) — or `native:<id>` for a PC-only producer

--- OBSERVED (machine-owned; overwritten by ingest, never hand-edited) ---
first_seen        run stamp + frame of first observation
runs              how many ingested runs saw it
prims_guest       prims attributed to it on the guest/GTE leg (last run, and max)
prims_native      prims its native ProducerScope pushed (last run, and max)
gte_calls         RTPS/RTPT count attributed to it (ot_attr.h:93)
sub_signatures    [{op, texpage, clut, mode, semi, count}] — the row-splitting evidence
nodes_seen        distinct entity nodes observed under it (count, not a list)
has_native        MECHANICAL: is `key` in the override table with a native handler != gen
native_reached    MECHANICAL: did that native handler actually RUN (overrides' per-address counts)
layers            RQ layer histogram (background/world/overlay/hud)

--- CURATED (human/agent-owned; ingest preserves verbatim) ---
name              what the effect IS ("radial plume", "title logo sprites")
re_status         unknown | decompiled | re-verified | ported | ported-partial
re_evidence       the claim / issue / decomp that justifies re_status
producer_file     game/render/fx_plume.cpp — the native producer, if any
partial_because   REQUIRED when re_status is ported-partial: the branch that is NOT ported
compare_status    never-compared | pixel-diffed | byte-exact | diverges
notes / links     [[claim]] / issue ids
```

`has_native` and `native_reached` are **derived, not stored curation** — that is what stops the DB
from rotting into a wish list. `re_status` is the field that cannot be derived and must therefore be
falsifiable: `re-verified` with no `re_evidence` is a lint error (`producers.py check`).

## Storage and ownership — where the file lives, who writes it

**Per game repo, in-repo, greppable** (workspace CLAUDE.md: the data must reach subagents):

```
<game>/docs/producers/<key>.md     one file per producer, YAML frontmatter (schema above) + body
<game>/docs/producers/README.md    the schema + how to read a negative
<game>/tools/producers.py          ingest / search / report / check / annotate
<game>/scratch/producers/run-*.jsonl   per-run OBSERVATION log (machine, never tracked)
```

One file per row, frontmatter + prose body — deliberately the shape `docs/issues/` and
`docs/info/claims/` already use, so it is greppable, and so two concurrent agents' ingests touch
disjoint files instead of fighting over one JSON.

**Who writes it, and why the runtime does NOT write tracked files.** The C++ runtime appends its
observations to `scratch/producers/run-<stamp>.jsonl` as producers are first seen, and at exit prints
ONE lucent line naming the ones with no DB row. `tools/producers.py ingest` creates/updates the
tracked rows, and is called from `run.sh` (tail AND next start, so a killed session is not lost) and
from `tools/gate.sh` / `tools/precommit_gate.sh`. Automatic in the sense the ask requires, without the
runtime owning a YAML writer or mutating a git tree from the frame path — where a crash or watchdog
abort would leave half-written frontmatter.

`ingest` is idempotent: a run that saw no new producer and no changed slow-moving field leaves every
tracked file **byte-identical**, so `git status` clean stays meaningful and a play session's diff is
exactly what that session discovered.

---

## Staged plan — each stage starts with a RED test in `psxport/tests/`

Framework work needs `mkdir coord/claims/producer-census` first (`docs/workspace/PROTOCOL.md`); the per-game
tool and `docs/producers/` are game-side and need no framework claim.

1. **`ProducerCensus`, hermetic, no game.** A per-Core value class (goes on `RenderSubstrate`,
   `render_substrate.h`, next to `otAttr`): `note(key, leg, prims, gte)`, plus the four denominators.
   RED test: build a census, note two producers on both legs, assert the table and the counters.
   No render path touched yet — this stage is pure data structure and it is where the negative-case
   assertions land.
2. **Guest-leg feed, and the hot-path gate — the stage the ask lives or dies on.** At prim completion in `gpu_gp0` (`gpu_native.cpp`
   ~935/~989), join `s_fifo_addr[0]` → `OtAttr::lookupStore` → census. Requires the span feed on
   without the `otattr` channel: widen `OtAttr::trackStore`'s inline gate to
   `g_otattr_channel || censusArmed` (one extra relaxed load — same shape as the existing gate,
   `ot_attr.h:87`), and **measure it** against the 2.54% the out-of-line version cost. If the census
   cannot be free enough to leave on in ORDINARY PLAY (the ask's primary data source), the answer is
   a cheaper feed — sample the span table per frame rather than per store, or attribute at the OT walk
   only — never an opt-in flag that leaves play uncounted. Whichever it is, the run NAMES it, because a
   DB that quietly stopped being fed reads exactly like a game that quietly stopped drawing anything
   new. RED test: a synthetic pool store + one-node OT walk attributes one prim to one fn (this is
   also the `--selftest`).
3. **Native-leg feed.** `RenderQueue::ProducerScope` + the interned producer-id table; count pushes
   in `emitOrQueue`/`push2dQuad`/`drawWorldQuad`. RED test: pushes inside a scope are attributed,
   pushes outside are counted as `unscoped` (a real number, not silence).
4. **Derived `has_native` / `native_reached`.** Read the override table + its per-address hit counts;
   this is the mechanical half of the compare. RED test: an installed native address reports
   `has_native=1, native_reached=0` until dispatched, `1/1` after.
5. **`scratch/producers/run-*.jsonl` writer + the exit summary line.** RED test: a run with the
   census armed writes a parseable file whose totals match the census counters.
6. **`tools/producers.py`** (`ingest`/`search`/`report`/`report --todo`/`check`), then wire `ingest`
   into `run.sh` (tail + next start) and the gates. `report --todo` is what answers "work on the DB
   entries" — the ranked queue described at the top. `check` lints: `re-verified` without evidence, `ported-partial` without
   `partial_because`, a row whose `producer_file` does not exist, a row `has_native=1` whose
   `re_status` is still `unknown`. Ship it in **one** game first — Tomba2Engine, because it has 68
   render files and the most rows to be wrong about — then copy to spyro/spider1.
7. **Per-producer A/B, the compare column.** With `docs/plans/render-path-tristate.md` landed, a row's
   `compare_status` becomes measurable: same binary, same frames, native leg vs GTE leg. The
   single-producer A/B recipe already used for spyro C167 (one early return in the producer's emit,
   same 24 presents, distinct-colour count) is the per-row instrument.

## Open questions (do not guess these — they change the schema)

1. **Is a guest fn the effect granularity you want, or do you want the DB rowed by NAMED EFFECT with
   fn as an attribute?** Plan above assumes fn-keyed with curated names; the plume case is the
   argument for it.
2. **Should a row track pixel-area rather than prim count?** Prim count is free; screen-area coverage
   is the number that actually says "how much of the frame do we own", and needs a bbox accumulator.
3. **One DB per game, or a shared row space for producers that are engine-common across games?**
   Assumed strictly per-game (the ask says "for each game"); guest addresses are per-game anyway.
