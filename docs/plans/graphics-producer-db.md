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
> **STAGE 2 STATUS 2026-08-11 — the gate cost is SETTLED and CHEAP; the feed is BLOCKED on span-table
> capacity.** Measured, not estimated:
>
> * **The hot-path gate is affordable, so the census stays on in ordinary play.** Widening
>   `OtAttr::trackStore` to `g_otattr_channel || g_producer_census_armed` costs **+0.26%** (Tomba!2, 300
>   frames headless, three runs each: 77.7/77.6/77.7 s closed vs 77.9/77.9/77.7 s armed) against the
>   **2.54%** the out-of-line version once cost. The work is real and not optimised away: the same run
>   records **10,188 spans, 0 overflow**, printed at run end so "no cost" can never be read as "no work".
>   With the guest-leg join added the total is **+0.8%**. So the plan's fear — that the census could not
>   be free enough to leave on during play — does NOT hold, and no opt-in flag is needed.
> * **The join is wired** at all three prim-completion sites in `gpu_gp0` (polygon, rect/sprite, line)
>   via `GpuState::censusGuestPrim`, joining `s_fifo_addr[0]` -> `OtAttr::lookupStore` -> the census.
> * **THE BLOCKER: the span table saturates.** On the `gte` render path (the only path where guest GP0
>   prims execute at all — on the native path pc_render does not walk the OT, so the guest leg correctly
>   records NOTHING), a 300-frame run reports `spans recorded 65536 (overflow 2681415)`. The table is
>   sized for a diagnostic burst, not a session. Consequences, both counted rather than guessed:
>   `span-miss 113,586` and `span-no-fn 107,837` out of 442,846 prims seen — attributed **0**.
> * **A span with `fn == 0` is NOT a row.** Wiring it as one produced a single bogus row keyed
>   `0x00000000` holding 107,837 prims, which is worse than being uncounted because it reads as a
>   producer. It is now its own blindness reason (`WHY_SPAN_NO_FN`) so the report distinguishes "no span
>   covered this address" from "a span did, and it does not know the author".
> * **UPDATE, same day — the saturation is FIXED and it was MASKING the real blocker.** The table was
>   reset off `gpu.s_frame`, which counts PRESENTS: measured, 60 logic frames advanced it to 3, so the
>   reset almost never fired. It is now driven per LOGIC frame from the frame loop
>   (`OtAttr::beginLogicFrame`). Overflow went **2,681,415 -> 0** and span-miss **113,586 -> 26**, so
>   lookups now find their span essentially always. And with that noise gone the real problem is plain:
>   **`span-no-fn` is now 221,397 of 221,423** — the spans are found and their emitter fn is ZERO.
> * **THE ACTUAL BLOCKER, and it is architectural rather than a capacity bug.** `OtAttr`'s identity comes
>   from `InterpDiag::otattrTop()`, which by design pushes ONLY around indirect (`jalr`/`r2`) dispatch in
>   `rec_dispatch` — "direct recompiler-emitted `func_XXXX(c)` calls do NOT push" (interp_diag.h). This
>   port's frame loop is NATIVE and calls guest bodies directly, so for the pool stores that build a
>   frame there is frequently NOTHING indirectly-dispatched on the stack and the shadow stack is empty.
>   The guest leg therefore cannot get a producer identity from that source on this execution model, and
>   no amount of table tuning changes it.
> * **Do not "fix" this by keying rows on 0, or by falling back to the last non-zero fn.** The first
>   invents a producer (already measured: it produced one bogus row holding 107,837 prims); the second
>   attributes work to whoever happened to be dispatched most recently, which is worse because it looks
>   plausible. `WHY_SPAN_NO_FN` exists to keep this visible until it is genuinely solved.
> * **Candidate directions, none measured yet, listed so the next session starts from evidence:**
>   (a) attribute at the OT WALK instead of at the store — the walk visits each packet with the OT entry
>   in hand, which is a different and possibly better identity source; (b) use the span's `node` field
>   (already recorded) and read the node's render fn at `node+0x18`, which would land in the SAME key
>   space the native leg uses — but check first whether `node` is populated on this path, since
>   `RenderDiag::currentNode()` is set by the NATIVE walk and the gte leg does not run it;
>   (c) push the shadow stack on direct calls too, which is a substrate-wide cost that must be priced
>   before being considered.
> * **CANDIDATE (b) MEASURED AND MOSTLY DEAD, 2026-08-11.** The node IS there — 219,322 of 221,397
>   no-fn prims carried a render-walk node (99.06%), which looked decisive. But keying on `node+0x18`
>   only attributed **439** of them (0.2%): for the rest, +0x18 is not a main-RAM code address, i.e. those
>   nodes are not the type-0x20 render-fn shape at all. The 99.06% figure measured "is a node present",
>   which is NOT the same question as "does that node name a producer" — a reminder that a high number
>   against the wrong denominator is still the wrong answer. The route is KEPT because it is correct where
>   it fires and it lands in the native leg's key space, and both identity routes are counted separately
>   so a row's provenance is visible. It is not the guest leg's answer.
> * **CANDIDATE: the store's guest PC — TRIED AND REJECTED ON EVIDENCE, 2026-08-11.** Every recompiled
>   wrapper opens with `c->pc = <its own address>`, so unlike the shadow stack it IS populated for direct
>   calls. Wiring it attributed **221,397 of 221,397** prims: a guest leg that looked finished. The
>   addresses it named were `0x80080000`, `0x8008007C`, `0x8007FDB0`, `0x8007E620` — the SDK's own
>   packet-submit leaves — NOT SDK code, though this document called them that until 2026-08-12; they are
>   the game's own POLY_GT3/GT4 submitters, already native-owned, sitting inside the address band that
>   happens to hold the SCEI library. `c->pc` is the innermost guest fn ENTERED, so it names the LEAF
>   ROUTINE that performed the store, never the effect that requested it. **This is the single most
>   instructive negative in this plan: it produced 100% attribution and ~0% truth, and only reading the
>   actual row keys caught it.** A coverage number cannot validate an identity source.
> * **NOTE WHAT THAT IMPLIES ABOUT THE WHOLE APPROACH.** The pool stores are performed by SHARED SDK
>   routines on behalf of a caller, so no per-store observation can name the effect: the information is
>   not present at the store. Identity has to come from something that spans the call — a scope the
>   caller opens (which is what the NATIVE leg does and why it works), or the guest's own dispatch record.
> * **So the remaining candidate is (c) price pushing the shadow stack on direct calls** — the only option
>   left that restores caller context at store time. (a), attributing at the OT walk, was reconsidered and
>   is NOT promising after all: the OT entry yields the packet ADDRESS, which the join already has, and no
>   author. (a) is the more promising: the walk holds the OT entry, and the OT entry is what the
>   guest itself uses to find the packet, so identity there does not depend on the C call stack at all.
>
> * **NEXT, and do not skip to the join's downstream:** give the span table a per-frame reset or a ring
>   discipline so it stops saturating, THEN re-measure whether `span-no-fn` survives — it may be a
>   symptom of lookups landing in a saturated table rather than an independent defect. Do not tune the
>   attribution until the table stops dropping 2.68M spans, or you are tuning against noise.

> **THE WORLD LAYER IS NOW KEYED, AND THE GUEST CHAIN BEHIND IT DOES NOT RUN (2026-08-12).** The
> largest undeclared block was `Render::perObjFlush`, the PICTURE half of guest `FUN_8003CDD8`
> (~846k prims per 500-frame field replay). It is now scoped, keyed by the **per-mode emitter** the mode
> routes to — never by `FUN_8003CDD8`, which is a shared caller whose key would collapse all eleven
> emitters into one meaningless row. Result: attributed **69,276 -> 484,940** prims, world-undeclared
> **846,223 -> 430,559**, new dominant row `0x80146478` at 415,664. `Tomba2Engine` commit 9c94008.
>
> **CORRECTION (2026-08-12, same day): the earlier claim here — "the guest per-object dispatch chain
> does not execute on the leg where the census measures" — WAS WRONG, and the way it was wrong is the
> reusable lesson.** It rested on an instrumented NATIVE body logging 0 calls. But `PSXPORT_GATE=1` sets
> `psx_fallback`, and `overrides::runEntry` routes EVERY registered address to its `gen` body on that leg:
> `PSXPORT_DEBUG=ovhit` reports **482 registry entries, ZERO with native hits, 268 with oracle hits**.
> The chain runs — `FUN_8003CDD8` 1195 times, `FUN_8003F698` 9507, `FUN_80146478` 6245 — only the PC
> bodies are bypassed, which is what recomp_path MEANS.
>
> Worse, this was already recorded: `Tomba2Engine/docs/findings/render.md` has carried it since
> 2026-08-06, with the one-line check (`PSXPORT_DEBUG=ovhit`) that would have settled it in seconds. The
> registry was queried about the crash being triaged, never about the silence. The finding has been
> retitled so the phrasings anyone would actually type — "never runs", "never called", "0 calls", "never
> dispatched", "probe prints nothing" — reach it, and all four were verified to hit.
>
> **What this changes, and what it does not.** The recorder design still had to go, but for the routing
> reason rather than an idle guest: the record lived in a native body the measuring leg bypasses. The key
> resolved from data is unaffected and is in fact BETTER than described — it reads the same `MODE_*` state
> the executing gen body reads, so it names the emitter the guest ACTUALLY used, not a counterfactual.
> And the world layer DOES have a runtime guest side; `primsGuest` is 0 because the guest leg's
> attribution is structurally blind for packet-pool stores, not because nothing ran. A guest-side
> comparison is therefore possible in principle, blocked by that blindness alone.
>
> The key is resolved from guest DATA (the same `MODE_FORCE`/`MODE_BYTE`/`MODE_TABLE` reads
> `perModeDispatch` makes), naming the emitter the guest actually routed to. It exposes rather than hides
> a real asymmetry — the guest routes to one of
> eleven per-mode emitters while the native pass draws every geomblk as generic GT3/GT4, so a row keyed
> to a non-generic emitter quantifies the generic rebuild of a special-cased guest renderer. One input
> (`flag & 1`, which also forces generic) belongs to a function that never runs on this leg and so is
> unobservable in principle; it is passed as 0 and named at the call site.
>
> **NATIVE-LEG ATTRIBUTION IS NOW 94.2%** (1,170,025 of 1,241,704 prims, 500-frame field replay,
> `Tomba2Engine` 94f6c8a). Two more producers keyed after perObjFlush, and each taught the same lesson:
>
> * **`backdropRender`** -> keyed on the **resident drawer**, resolved per area from the guest's bg-state
>   jump table (`backdropTilemapDrawer` already decoded it and merely did not report it). Background
>   undeclared 323,163 -> 27. NOT a hardcoded literal, and the run proved why: two distinct drawers
>   appeared in one replay — `0x80115598` (seaside field, 299,200 prims) and `0x8010C26C` (SOP narration,
>   23,936, frames 63..96). A literal would have credited the narration's prims to the seaside drawer.
> * **`fieldEntityRender`** (scene table: grass, props) -> `0x80109FE0`. World undeclared 430,559 ->
>   68,610. The address was confirmed with `codemap.py --addr`, not read off the file's own banner —
>   a banner address in this subsystem was already wrong once (`"perObjFlush/func_80051464"` names
>   `NodeXform::propagateAxis`).
>
> **The generalisable rule from all three: the key is RESOLVED, never written down.** Two of the three
> guest addresses vary at runtime (per-mode emitter, per-area drawer), and both resolutions already
> existed in the code for other reasons. A literal would have been wrong for every case but the one it
> was sampled from, and wrong in the way a DB cannot detect — a plausible row naming a function that was
> not resident.
>
> **Still undeclared, NOT claimed as done:** world 68,610 + hud 2,939 + overlay 103 + background 27.
> Candidates are the unscoped emitting producers: `mesh_draw`, `fx_sprite`, `fx_vortex`,
> `fx_backdrop_plane` (world); `minimap`, `card_browser`, `hud_gauge_emitter`, `render_options`,
> `screen_fade`, `margin_render` (hud/overlay). Each needs its own guest address RE'd and verified.
> `mesh_quads` stays deliberately unscoped — the shared writer must inherit its caller's scope.

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

## DECIDED (USER, 2026-08-11): the row IS the guest submitter fn

Asked directly, with the alternatives (fn + material signature; named-effect rows with fns as
attributes) on the table. **One row per guest submitter fn, auto-created on first sight; `name` and
`re_status` curated on it; `has_native` / `native_reached` DERIVED from the override table.** Exactly
the schema above — so nothing in this plan changes, and the question is closed rather than left for a
later session to re-open. A fn that is honestly two effects is split by HAND, and the split records
its discriminator (the `sub_signatures` field is the evidence for when that is needed).

## IMPLEMENTED 2026-08-12: the guest leg has identity AND the two legs now JOIN

Three things landed today. Recorded here because this file is the design of record and the earlier
sections above describe a state that no longer holds.

**1. The guest leg's identity (psxport `38cec620`).** The attribution shadow stack is now maintained in
EVERY guest function's recompiler-emitted wrapper, so it covers direct `jal` calls, not just indirect
dispatch — that was the whole defect: the packet-pool stores are performed by shared SDK-adjacent
routines reached by direct `jal`, so the stack was empty for exactly the stores that mattered.
`span-no-fn` 274,089 -> **0**. Priced: 3,071,077 wrapper calls over a 1200-frame replay, +0.0% user CPU
for the push; the real cost is ~4.5% `.text` (the wrapper loses its tail call because it must return to
pop). The `if (g_otattr_channel)` gate on `recordFnStat` is LOAD-BEARING (+24% pc_render / +87%
psx_render without it).

**2. The JOIN (psxport `90604e18`).** Identity alone did not produce a comparison: the guest leg keyed
at the innermost EMITTER frame while a native row is keyed at the HANDLER/PASS frame, so only **2 of 25**
keys coincided and every guest row read `native 0`. A guest prim now resolves outward along its call
chain to the first frame a native producer keys a row at (`OtAttr::resolveClaimedFrame`), bounded at
`CLAIM_SEARCH_DEPTH = 8` — measured, not guessed: `PSXPORT_DEBUG=otchain` found every claim within 3
frames of the top, and a claim found AT the limit is counted and warned about. When no frame in the
window is claimed the prim keeps its emitter key, which IS the DB's answer for that effect: it has no
native producer.

**3. Row lifetimes (psxport `63c5f537`).** Both feed sites stamped rows with `GpuState::s_frame`, which
counts PRESENTS, so every guest row read `frames 1 (f3..f3)`. One shared definition now
(`census_frame.h` -> `Timing::logicFrame`). This was the SAME root cause already fixed one layer down for
the span-table reset — fixing the reset while leaving the row stamp on the same counter left the identical
defect in the field a human reads.

### THE STRUCTURAL FACT THE JOIN EXPOSED — the comparison is CROSS-RUN

**No single leg runs both halves.** Under pc_render the guest packets are never GP0-executed, so
`primsGuest` is structurally 0; under psx_render no native producer runs, so nothing can be claimed. The
claim set is therefore earned on one run and consumed by another, persisted APPEND-ONLY to
`<PSXPORT_PRODUCERS_DIR>/claims.txt` and loaded before frame 0.

**The rejected alternative is a trap worth naming:** reload the newest run JSONL. That DESTROYS the set —
a psx_render run's rows legitimately carry `prims_native: 0`, so loading them would report "nothing has a
native producer" about a game where nine things do. Append-only means a claim earned by a native producer
actually drawing is never un-earned by a later leg that skips it.

### The instrument caught ITSELF, which is the reusable part

The FIRST `otchain` run reported "0 of 29 shapes claimed" — on a pure psx_render leg, where the claim set
could ONLY be empty. Because the report prints its claim count and warns explicitly when it is zero, that
0 was legible as "this run cannot answer the question" rather than "the fix does not work". Reading it as
a finding would have killed the correct design. Every negative in this instrument carries its denominator
and its blind spots for exactly this reason.

### Measured end state (Tomba!2, 300-frame `house-on-the-point`, two legs)

* leg 1 (pc_render) earns 9 claims; leg 2 (psx_render) joins **217,533** spans, **145,027** with no
  claimed frame, **1** unresolvable (claim set still empty), **0** at the search limit.
* folded through `Tomba2Engine/tools/producers.py`: **8 rows carry BOTH legs** (GTE prims vs native prims
  on one key), **27 rows are guest-only** — the ranked "no native producer" list, topped by `0x8003DF04`
  at 394,944 prims.
* SBS-full byte-exact with all of this on the guest hot path (both documented gate legs, 0 `sbs-div`).

### Still open, and none of it is papered over

* ~10% of guest prims key at the game's own POLY_GT3/GT4 SUBMIT LEAVES (`0x80080000`, `0x8008007C`,
  `0x8007FDB0`) — MEASURED 2026-08-12 and the cause is settled: NO frame anywhere on those chains is a
  claim, out to the root at depth 28, so widening the search window would change nothing. The row is
  honest that the join failed and is named one granularity level too deep; the fix is a `ProducerScope` on
  the per-mode guest emitter the command routes to (Tomba!2: `0x800803DC`, the generic GT3/GT4 case),
  never on the shared dispatcher (which would shadow all eleven per-mode emitters into one row) and never
  on a leaf. **"No native producer" here does NOT mean "not ported"** — native code for this picture
  already exists; what is missing is a claim on THIS route. Reading it the other way sends someone to
  re-port code that is already there. Because no
  frame in their 8-frame window is claimed — `Tomba2Engine` kanban #88, with the two candidate causes and
  the experiment that distinguishes them.
* `otattrFrameFromTop` REFUSES above `OTATTR_CAP` rather than guessing, so a chain deeper than 256 guest
  frames is reported blind (1 store in the measured run).
* Whether the census is structurally live in `spyro` / `spider1` depends on their `GameConfig`
  `packetPoolBase`/`Stride` being RE'd; where those are 0 the guest leg is BLIND and says so, and an empty
  table there means "not measured", never "the guest submitted nothing".

## Open questions (do not guess these — they change the schema)

1. **Should a row track pixel-area rather than prim count?** Prim count is free; screen-area coverage
   is the number that actually says "how much of the frame do we own", and needs a bbox accumulator.
2. **One DB per game, or a shared row space for producers that are engine-common across games?**
   Assumed strictly per-game (the ask says "for each game"); guest addresses are per-game anyway.
