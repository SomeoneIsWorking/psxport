# The game-agnostic framework has 19 hardcoded Tomba! 2 guest addresses in it

**Measured 2026-08-13**, by grep over `runtime/psx/`, then read site by site to separate diagnostics from
behaviour. Found while looking for a single-step hook for the oracle differential, not by auditing for it.

## The contract this breaks

`psxport/CLAUDE.md`: the framework *"carries **no game code** — the framework `#include`s nothing from a
game; a game provides its typed runtime configuration and hooks and links `libpsxport`"*, and
`psxport_smoke` exists to keep that seam honest by linking the library against a stub and proving zero game
symbols resolve.

The `#include` half of that is true and tested by `psxport_smoke`, which cannot catch an address because a
hex literal is not a symbol.

**The address half is already ratcheted, and I did not know that when I started.**
`tests/test_no_game_address_literals.cpp` holds a per-file, per-address baseline with SHRINK-ONLY
semantics: it fails if a literal appears more often than the baseline says, AND it fails if a baseline row
has become stale, telling you to lower the count or delete the row. It reported this refactor precisely —
three rows fixed outright, four dispatcher counts down — and refused to pass until the baseline was
shrunk with the change. That is the guard working exactly as intended, and it means the framework's
game-address debt is *tracked*, not unnoticed. `pc_scheduler.cpp` alone carries 62 baseline rows.

## What is actually there

An earlier product CPU implementation held 11 diagnostic-only address probes inside its instruction loop:

| address(es) | what the comment says it is |
|---|---|
| `0x8007E9C8` | `FUN_8007E9C8(color)` screen-fade calls, `fadeshot` channel — writes PPMs |
| `0x800939A0` | libsnd voice keyon, `keyon` channel, marked *"oracle, temporary"* |
| `0x80074BF8`, `0x80074E48` | `sound_play_bgm` / `sound_stop_bgm`, `bgmreq` channel |
| `0x80026874`, `0x80052208`, `0x800522B0`, `0x80075834`, `0x800788CC` | one grouped trap |
| `0x800914D0`, `0x800909C0`, `0x80090BD0`, `0x80091460`, `0x80090210`, `0x80090560`, `0x8008E390` | further audio/seq probes |
| `0x8007E998` | `text` channel |

Those probes were removed with the obsolete product CPU implementation. Their lasting lesson is that
title-address diagnostics belong in the title, not in a framework instruction loop.

**`runtime/psx/pc_scheduler.cpp` — 8 addresses, and these are BEHAVIOUR**, not diagnostics:

```
entry_pc == 0x801062E4   // DEMO
entry_pc == 0x80109164   // SOP area-load
entry_pc == 0x8010637C   // GAME
entry_pc == 0x8010649C   // STAGE-0 START.BIN
entry_pc == 0x80044F58   // is_preload_body
entry_pc == 0x8004514C   // is_stage1_callback
entry_pc == 0x800452C0   // is_area_data_load
```

They classify guest task entry points and the scheduler acts on the classification. So **the framework
schedules Tomba! 2 differently from every other game, and for Spyro / Spider-Man / Vagrant Story / Toy
Story 2 / Mega Man X4 these branches are dead** — those games get whatever the fall-through does, silently.
A new port cannot express "this entry is my area-load callback" at all, because the seam has no field for
it.

**This is an UNFINISHED MIGRATION, not a design disagreement**, which is why it is cheap to finish.
`cmake/psxport.cmake:213` records the intent: *"as of P1.7c the framework owns
Fps60/RenderQueue/PcScheduler/VerifyHarness/FfSpan (their headers moved to runtime/psx/; the game
reaches into them only through the GameHooks seam)"*. The behaviour half of that seam DID move — the
scheduler calls `hooks->schedStageBody(...)` with a `SCHED_CORO_*` kind, so the game supplies the task
bodies. What never moved is the mapping from ENTRY PC to kind. The seam has the verb and is missing the
noun.

## Why this is not fixed in the same breath as recording it

The `pc_scheduler.cpp` addresses change scheduling behaviour of a **shipping, working port**. The USER's
standing instruction on exactly this: *"my main concern was that we don't dive too deep into technical work
and miss what is in front of us, the games themselves, because it happened before, you focused too much on
gates and in turn everything that was working became broken"*. A blind refactor of live scheduling
classification is that failure mode precisely.

So the order is: establish that the Tomba! 2 gate runs and is green FIRST, then move the addresses to the
seam, then re-gate.

**The baseline is MEASURED, so the refactor is now verifiable.** Built against the dev framework
(`-DPSXPORT_DIR=$PSX/psxport`) and gated 2026-08-13:

```
[gate:boot] exit=0 in 82.2s · 133 output line(s) · max frame counter seen = 428
[gate:boot] newgame prologue at frame 27; advanced 401 frame(s) past it (asked for 400)
[gate:boot] end state: stage=8010637C sm48=2
[gate:boot] env audit AT EXIT: 4 PSXPORT_* set -> 4 declared, 0 legacy, 0 UNKNOWN
[gate:boot] PASS
```

Note what the gate itself says it does NOT cover: *"This says NOTHING about pixels or SBS parity."* So it
catches a scheduler regression that changes which stage the run ends on, or that trips one of its 8 failure
patterns — it would not catch a change that keeps the stage identical while altering rendering. A
  representative title execution is the stronger check for the behavioural half.

## The fix, when it is done

- **The behavioural ones** become a `GameConfig` table of `(entry_pc, SCHED_CORO_* kind)` the game
  declares, in the same style as the crt0 boot group: named, justified by disassembly, and `0` when
  un-RE'd (zero is honest). `hasNativeHandlerForEntry` and the fiber stanza then look up the table
  instead of testing literals. Nothing needs inventing — `GameConfig` already holds guest-address
  literals and the `SCHED_CORO_*` kinds already exist; this finishes P1.7c by giving the seam the noun
  to go with its verb. Games that declare nothing get an empty table, which is exactly their behaviour
  today (dead branches), so no other port changes.
- **The diagnostic ones** do not belong in the framework at any address. The ones marked temporary should
  simply go. The rest belong behind a `GameHooks` per-instruction diagnostic callback the GAME installs, so
  a game-specific probe lives in the game repo, and the framework's hot loop tests one function pointer
  instead of eleven literals.
- **The ratchet already exists** — `tests/test_no_game_address_literals.cpp` — so the remaining work is
  to keep shrinking it, not to build it. Its 62 rows for `pc_scheduler.cpp` are the actual backlog, and
  the entry-PC work below removed 7 of them.

## What was DONE, and the honest limit on how well it is verified

The behavioural half is moved: `GameConfig` gained a `SchedEntry` table (`pc`, `nativeHandler`,
`hasFiberBody`, `fiberBody`), `pc_scheduler.cpp` looks up instead of testing literals, Tomba! 2 declares
the seven entries in its own `game_config.cpp`, and the ratchet baseline shrank by 3 rows plus 4 counts.
The transcription is verbatim — same addresses, same classification — and the previous `if/else` chain's
bare `else` (which silently made any unlisted entry `SCHED_CORO_AREALOAD_FAITHFUL`) is gone, because a
table has no fall-through to be wrong about.

**It is NOT verified by execution, and the reason is measured rather than assumed.** Three runs:

| run | result |
|---|---|
| `gate.py boot --frames 400` after the change | PASS, byte-identical numbers to the baseline (133 lines, stage `8010637C`, sm48 2) |
| the same gate with `schedEntryCount = 0` (temporary, reverted) | **also PASS**, identically |
| a 400-frame attract run (`gate.py run`, no `newgame`) | PASS, and no seam line either |

The second row is the important one: a green boot gate cannot tell a correct table from an empty one. So
the first row is not evidence. `PcScheduler::hasNativeHandlerForEntry` is the guard at
`scheduler.cpp:157` inside the SUBSTRATE fallback stanza — reached only for a task the native path did not
claim — and neither a boot nor an attract run reaches it.

That is now visible instead of invisible: the first consultation logs a one-shot line naming the entry PC,
whether it matched, and how many entries were declared, and it says outright that a log lacking the line
proves nothing about the table. It was tried in the destructor first and that did not fire at all — a
headless run leaves through `_exit`, so static teardown never happens. A diagnostic that only fires on a
clean shutdown does not fire.

**What would actually exercise it:** a run with un-ported tasks reaching the guest-execution path —
the SBS strict byte-compare leg. That is the check this refactor still owes.
