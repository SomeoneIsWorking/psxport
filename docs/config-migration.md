# The `PSXPORT_*` → CVar migration map

**Status: a MAP, not a migration.** This document is the complete inventory of every configuration
knob the framework has, what reads it, and the order in which the layered-CVar core should take them
over. No code changes with this file. The CVar core itself is a sibling change under the same
`config-cvar` claim (`runtime/recomp/config_var.h`, `config.h`, `config.cpp`); this document tells that
change what it has to cover and in what order.

Measured 2026-08-06 against `spider1/external/psxport` @ `9890eaa8`, plus read-only scans of
`spider1/game`, `spyro/game`, `Tomba2Engine/game` and their `tools/`.

---

## The four headline numbers, with their denominators

Scan denominator: **281 files / 74 622 lines** under the psxport checkout (excluding `build*/`,
`vendor/`, `scratch/`, `generated/`), plus the three games' `game/` and `tools/` trees.

| | count | what it means |
|---|---|---|
| **N — knobs** | **280** | distinct `PSXPORT_*` names that are, or are documented as, an environment knob |
| **M — read by NOTHING** | **74** | no read site in the framework *or* in any of the three game repos |
| **K — documented but dead** | **36** | `docs/config.md` presents it as a live knob; nothing reads it |
| **J — undocumented but live** | **112** | has a real read site; `docs/config.md` never mentions it |

Of the 280: **171 are read by the framework**, **35 only by a game repo**, **74 by nothing**.
Of the 171 framework-live knobs, **83 are undocumented** and 88 are documented.
Beyond the 36 in K, a further **29** dead names sit in `docs/config.md`'s "old env flag → channel"
table, where being dead is *correct and intentional* — but that table is a tombstone table, which the
workspace's own no-tombstones rule says to delete rather than maintain.

**`PSXPORT_FPS60` is not an isolated case.** The brief named it as the one documented-but-dead knob.
It is one of **eighteen** in a single doc paragraph. `docs/config.md:541-548` presents
`VK`, `SW_GPU`, `VK_NODEPTH`, `VK_TRITEST`, `FPS60`, `FPS60_GATE`, `FPS60_SYNTH`, `IRES`, `WIDE`,
`GPU_WINDOW`, `NATIVE_DEPTH`, `SSAO` (+`SSAO_STRENGTH`/`_RADIUS`/`_BIAS`/`_RANGE`/`_VIZ`), `LIGHT`
(+`LIGHT_DIR`/`_AMBIENT`/`_DIFFUSE`), `UI`, `ATTACH`, `CULL`, `TRANSPLANT` as live env vars. **None of
them is read by anything in any of the four trees.** They are the video settings, and they moved to
`psxport_settings.ini` + the F1 overlay — the doc says so itself twenty lines earlier
(`config.md:33-36`, "Visual settings … are the F1 overlay + `psxport_settings.ini` …, not env") and
then lists them as env vars anyway. Anyone who A/B'd a render change with `PSXPORT_FPS60=1` or
`PSXPORT_IRES=3` set measured the same program twice.

**How "dead" was confirmed, because a grep count is text and not code.** Every name in M was checked
with a *call-site* regex, not a token grep: `cfg_on|cfg_int|cfg_str|getenv|SDL_getenv|std::getenv|
lucent::config::flag|number|text|present` immediately followed by `("<NAME>"`, plus
`os.environ`/`os.getenv` for Python and `${NAME}` in `*.sh`, run over all three game trees and all
three psxport submodules. Doc code-blocks (`cfg_on("PSXPORT_FOO")` at `config.md:86`) and comments
that quote a deleted call (`native_boot.cpp:629`) were rejected by name — the six rejected hits are
listed in the method section below rather than silently dropped.

---

## Corrections to the brief. These were MEASURED; treat them as replacing what you were told.

The brief's starting numbers came from a token grep. Four of them do not survive a call-site check.

1. **"201 distinct `PSXPORT_*` env vars"** — 240 distinct `PSXPORT_*` *tokens* appear in the psxport
   checkout. 24 of them are not env vars at all: 7 include guards (`PSXPORT_CFG_H`…), 4 compile-time
   macros, 6 CMake variables (`PSXPORT_ROOT` alone accounts for 83 occurrences), 2 doc placeholders
   (`PSXPORT_FOO`, `PSXPORT_YOUR_FLAG`) and 12 prose tokens. **171 have a real framework read site.**

2. **"`PSXPORT_SDL` (15 sites)" is not an env var.** It is a compile-time macro set by
   `cmake/psxport.cmake:173` (`target_compile_definitions … PSXPORT_SDL`) and consumed by `#ifdef`
   in `pad_input.cpp`, `spu_audio.{h,cpp}`, `gpu_native.cpp`, `native_fmv.cpp`, `native_stub.cpp`.
   It has no place in a CVar migration. Setting it in the environment does nothing.

3. **The `PSXPORT_DEBUG` landmine is ALREADY DEFUSED in this tree, and the remaining risk is a
   different one.** The brief says the migration is dangerous because "~700 legacy `cfg_log*` sites
   fire during boot". Measured: **`runtime/` and `common/` contain ZERO `cfg_logf`/`cfg_logi`/
   `cfg_logw`/`cfg_loge`/`cfg_dbg` call sites** — the only occurrences are the declarations in
   `cfg.h`/`cfg.cpp` themselves and docstrings in `tools/logsig.py`. `bootstrap_once()` no longer
   loads anything; it only bumps a generation counter. The channel name is baked in at build time
   (`cmake/psxport.cmake:198-199`), which is why order no longer matters.
   The legacy sites that DO remain are entirely game-side: **Tomba2Engine `game/` 262, spider1
   `game/` 57, spyro `game/` 0.** So the risk is not "retiring cfg turns the channels off"; it is
   **"routing `PSXPORT_DEBUG` through a CVar makes lucent stop seeing it"** — see the dedicated
   section below.

4. **This checkout predates the `pace-parity` landing.** `PROTOCOL.md` records
   `gpu_native.cpp:1542`'s `if (!gpu_has_window() || cfg_on("PSXPORT_NOPACE")) return;` as history,
   fixed by psxport `80e3d203`. In this tree that line is still present verbatim, `gpu_has_window()`
   still exists (`gpu_native.cpp:1536`, `gpu_vk.cpp:146`), and `80e3d203` is not a valid object
   (`git cat-file -t` fails). `pace_plan.h` and `video_plan.h` do not exist here. The `PSXPORT_NOPACE`
   row below therefore describes THIS tree; whoever merges must re-check it against the landed shape.

---

## The mechanisms that exist today: there are FIVE, not three

The brief lists three config sources. There are five, and only two of them can see each other.

| # | mechanism | reads | who |
|---|---|---|---|
| 1 | `cfg_on`/`cfg_int`/`cfg_str` → `lucent::config::flag/number/text` | the real environment, cached per name | 152 sites |
| 2 | raw `getenv()` | the real environment, uncached | 54 sites, 40 of them `PSXPORT_SBS_*` in `sbs.cpp` |
| 3 | `psxport_settings.ini` via `runtime/recomp/mods.cpp` | a `key=value` file; **no env path at all** | 15 keys |
| 4 | REPL / debug-server `debug <chans>` → `lucent::enable_channels` | a live command | `repl.cpp:123`, `dbg_server.cpp:318` |
| 5 | `common/env.cpp` `psxport::GetEnv` | environment **then a `.env` file walked upward** | 1 caller: `tools/discdump.cpp` |

Facts that follow, each one checkable:

- **Mechanisms 1/2 and mechanism 3 are DISJOINT SETS.** Not one of `mods.cpp`'s 15 ini keys
  (`aspect`, `ires`, `ssao`, `light`, `shadows`, `fps60`, `ssao_strength`, `ssao_radius`, `ssao_bias`,
  `ssao_range`, `shadow_strength`, `light_dir`, `light_ambient`, `light_diffuse`, legacy `ires_auto`)
  has an environment override, and not one of the 171 env knobs is persisted. *That is why precedence
  was never written down: the two layers never met.* It is also exactly why `PSXPORT_FPS60=1` is
  inert — `fps60` is an ini key and the env name was never wired to it.
- **`Mods::load()` silently discards an unknown key.** `mods.cpp:30-47` is an `if/else-if` chain with
  no terminal `else`; a typo'd or retired key is dropped with no message. A missing settings file
  also returns silently (`mods.cpp:26`).
- **`.env` is not a general config source.** Only `spider1/run.sh:66-68` (two disc keys) and
  `common/env.cpp:25-44` (used by `discdump` alone) read it. A `PSXPORT_ORACLE=1` line in `.env` does
  nothing — including for the SBS-contamination guard in `cfg.cpp:140`, whose comment claims it
  protects against "a stray `.env`".
- **Mechanism 4 only touches channels.** `repl.cpp:123` calls `lucent::enable_channels(ch)` directly.
  It cannot change any other knob. `docs/config.md:473`'s sentence — the only precedence sentence in
  the repo — is therefore true but far narrower than it reads: *the REPL `debug` command outranks
  `PSXPORT_DEBUG`, and nothing else has a precedence rule at all.*
- **Introspection already half-exists and is half-blind.** `cfg_dump()` (`cfg.cpp:164`) prints every
  `PSXPORT_*` variable that is SET in the environment. It cannot report which knobs EXIST, which
  layer a value came from, or that `PSXPORT_ORACL=1` was set and matches nothing.

---

## What is taken from Dusklight, and what is deliberately not

Read: `~/repo/dusklight` @ HEAD, `src/dusk/config_var.hpp` (387 lines), `src/dusk/config.hpp` (194),
`src/dusk/config.cpp` (641). CC0.

**Taken.**

- **The layer enum as the single source of precedence.** `ConfigVarLayer { Default, Value, Override,
  Speedrun }` with `getValue()` a `switch` over the layer (`config_var.hpp:207-222`). One value is
  live at a time and the CVar *knows which layer it is on* — that is what makes "why is this knob
  this value?" answerable instead of inferred.
  *Detail worth copying carefully:* the ladder is **not** the enum order. `Speedrun` sits below
  `Override` because `setSpeedrunValue` refuses to act when `layer == Override`
  (`config_var.hpp:279`), and `clearSpeedrunOverride` restores a saved `priorLayer`. Precedence is
  enforced by the mutators, not by comparing enum values.
- **Only `Value` (and `Speedrun`'s stashed `priorLayer` value) is ever persisted.** `save()`
  (`config.cpp:537-545`) writes a key only when the layer is `Value`/`Speedrun`, and
  `getValueForSave()` (`config_var.hpp:305`) deliberately returns the *user's* value even while a
  temporary override is live. Defaults and overrides never leak into the file.
- **An unknown key is KEPT, never dropped.** `LoadFromPath` (`config.cpp:481-489`) puts a key with no
  registered CVar into `UnregisteredConfigVars`; `save()` writes it back out (`config.cpp:547-549`);
  and `Register()` (`config.cpp:409-419`) back-fills it the moment a matching CVar registers. Launch
  arguments get the same treatment via `UnregisteredConfigVarOverrides` (`config.cpp:421-430`,
  `load_arg_override` `config.cpp:565`). This is the direct cure for `Mods::load()`'s silent drop.
- **Access before registration is fatal, not defaulted.** `checkRegistered()` `abort()`s
  (`config_var.hpp:93`), and `Register()` fatals on a duplicate name (`config.cpp:402`). A knob that
  does not exist cannot quietly read as "off".
- **`EnumerateRegistered` + `getLayer()`** (`config.hpp:139`, `config_var.hpp:127`) — the built-in
  answer to "which CVars exist and where is each one resolving from".
- **The header split.** `config_var.hpp` = access only, safe to include widely; `config.hpp` = define,
  mutate, load, save, and it pulls in `nlohmann/json`. Its own comment says "Avoid including this
  header in the entire game, it's heavier than I'd like!" psxport has ~800 files that want to *read*
  config and a dozen that want to *define* it; the same split applies unchanged.
- **`subscribe()` change callbacks** (`config.hpp:157-180`) — how the F1 overlay's live toggles stop
  being ad-hoc setters. Note their explicit rule: load and launch-arg application do **not** notify,
  because subsystems read their own initial value at init.

**Not taken.**

- `nlohmann::json` as the file format. psxport's settings file is `key=value` and three games ship
  with existing files; the CVar core should keep that format (a `dumpToText`/`loadFromText` impl
  pair) rather than orphan them. The Dusklight shape does not depend on JSON — only `ConfigImplBase`'s
  three virtuals do.
- The `Speedrun` layer as named. psxport has no speedrun mode. The slot it occupies — *a temporary,
  never-persisted layer that a launch argument still outranks* — is exactly what the REPL /
  debug-server needs, so take the slot and call it **`Runtime`**.
- `dolphin/types.h`, `borealis::io`, `absl::flat_hash_map`. Framework-specific.

---

## The mapping, EVALUATED rather than assumed

The brief proposes: code default → `Default`; `psxport_settings.ini` → `Value`; env var → `Override`;
REPL → a runtime layer above `Value`. Checked against the measurement, that mapping holds, with three
qualifications that change the work.

**It holds because env genuinely is "launch argument, never persisted".** Nothing in the tree writes
an environment variable back anywhere; `setenv` appears only in `run.sh` (`PSXPORT_VK_HEADLESS`/
`PSXPORT_VK_WINDOW`) and in tests. Mapping env to `Override` preserves every current knob's behaviour
exactly: today env is the only source for 171 knobs, and `Override` outranks everything.

**Qualification 1 — the mapping is not a merge of two overlapping sets, it is a UNION of two disjoint
ones.** 171 env knobs with no `Value` layer, 15 ini keys with no `Override` layer, zero overlap. So
"give each knob a precedence answer" is not the hard part; the hard part is that **15 settings gain an
env override they never had and 171 knobs gain persistence they never had**, and both are new
behaviour. The safe reading: give every CVar both layers as a matter of mechanism, but persist a knob
only if it is marked persistable. A `PSXPORT_SBS_LW_ADDR` written into `psxport_settings.ini` because
someone once ran a byte-trace is a new class of bug.

**Qualification 2 — `PSXPORT_ENH` and `oracle_mode()` are a SUPPRESSION rule, not a layer.**
`cfg.cpp:123-152` (`oracle_mode` at 123, `cfg_enh` at 132): `cfg_enh(name)` returns 0 unconditionally — with a warning — when
`PSXPORT_ORACLE` or any SBS mode is on, *whatever the user asked for*. That is deliberate (a stray
enhancement must never contaminate a byte-compare) and it is not expressible as a precedence ladder,
because it is one CVar forcing another regardless of layer. Keep it as an explicit resolve-time hook
with its own log line; do not try to model it as a layer, and do not lose the warning.

**Qualification 3 — `PSXPORT_DEBUG` and `PSXPORT_LOG_FILE` must NOT become ordinary CVars.** See
below. They are lucent's own configuration, resolved inside lucent from a build-time-baked name.

---

## `PSXPORT_DEBUG` — the highest-risk single migration, restated from measurement

`PSXPORT_DEBUG` appears 86 times in the checkout. **None of them is a read.** The value is resolved
inside lucent, from a variable name baked in at build time by `cmake/psxport.cmake:198`
(`set(LUCENT_CHANNEL_ENV "PSXPORT_DEBUG")`), lazily on the first log call. `runtime/` calls
`lucent::debug(...)` at 158 sites across **57 distinct channels** (`cd`, `gpu`, `ires`, `ovload`,
`recdep`, `sbs`, `vramup`, …); every one of those is gated by that single variable.

The failure mode to design against, stated as what would actually break:

> A `ConfigVar<std::string> cvDebug{"PSXPORT_DEBUG"}` is registered, the CVar layer resolves it, and
> the code helpfully "stops reading the environment directly". lucent never gets told. Every channel
> in four repos goes silent, no error, no warning — and the first symptom is a debugging session that
> concludes "the subsystem is never reached".

The gate that catches this already exists: **`tests/test_lucent_channel_env.cpp`**, which asserts a
`lucent::debug` on a `PSXPORT_DEBUG` channel is emitted when it is the *first* logging call in the
process, and again from a static initialiser in a freshly `exec`'d child. It must keep passing
unmodified through every step below. Do not weaken it, do not skip it, and do not "adapt" it to the
new API — if it needs adapting, the migration has changed observable behaviour.

The only safe shape: `PSXPORT_DEBUG` and `PSXPORT_LOG_FILE` are **registered in the CVar registry for
VISIBILITY only** (so `config dump` lists them and a typo is reported), while lucent keeps reading
them itself. If a REPL `debug` command is ever routed through the CVar system, the setter must call
`lucent::enable_channels()` — it cannot be the reverse. Migrate these two LAST, and only after every
other group is landed and gated.

---

## The ordered migration plan

Each group is independently gateable: a hermetic test in `tests/` that fails before and passes after,
plus a named runtime check. No group depends on a later one. **No group migrates more than it can
gate** — the compatibility path (`cfg_on`/`cfg_int`/`cfg_str` continuing to resolve through the
`Override` layer for any name not yet registered) keeps the other 170 knobs working throughout.

**Group 0 — the core, zero knobs migrated.**
`config_var.h` (access) + `config.h`/`config.cpp` (define/mutate/load/save). Layers `Default < Value <
Runtime < Override`. `Register`, `GetConfigVar`, `EnumerateRegistered`, unknown-key retention,
fatal-on-unregistered-access, fatal-on-duplicate-name. `cfg_on`/`cfg_int`/`cfg_str` re-implemented over
it: a registered name resolves through the ladder, an unregistered name falls through to
`lucent::config::*` exactly as today **and is recorded in an "asked for but not registered" set.**
*Gate:* a hermetic `tests/test_config_var.cpp` that asserts each layer's precedence, that `Override`
survives a `setValue`, that a `Value` write persists and an `Override` does not, and that an unknown
key round-trips through save. *Negative control:* the test must include a case that FAILS if the
ladder is ordered wrongly — set `Value` after `Override` and assert the read is still the override.

**Group 1 — the self-report, still zero knobs migrated.** This lands before any knob moves, because
it is the instrument every later group is measured with.
`config dump` (REPL + debug-server + a boot-time line) printing, for every registered CVar: name,
type, current value, **which layer it resolved from**, and its default. Plus the two lists that make
a negative meaningful: **registered-but-never-read** and **requested-but-not-registered** (the typo
list). *Gate:* a test that sets `PSXPORT_ORACL=1` (a deliberate typo of a registered name) and asserts
it appears in the unknown list; and one that asserts the dump prints a nonzero denominator when NO
knob is set — "0 CVars set" must be distinguishable from "the dump never ran".

**Group 2 — the 15 `psxport_settings.ini` keys.** The smallest closed set, the only one that already
has a `Value` layer, and the one that fixes a live bug: `Mods::load()`'s silent drop of an unknown
key. Each becomes a `ConfigVar<T>` with the ini key as its name; `mods.cpp` becomes a thin view over
them; unknown keys are retained and reported instead of discarded. This is also where the 18 dead
`docs/config.md` render knobs get their honest answer: either they become real `Override` names for
these CVars (`PSXPORT_FPS60` → the `fps60` CVar, at last doing what the doc has claimed for months),
or the doc lines are deleted. **Decide per knob and record the decision; do not leave a doc line
standing over nothing.** *Gate:* write an ini with a good key, a retired key and a garbage key; assert
the good one loads, the other two are *reported*, and a save round-trips all three.

**Group 3 — PATH/INPUT, 11 knobs.** `PSXPORT_DISC`, `PSXPORT_TOMBA2_DISC`, `PSXPORT_SETTINGS`,
`PSXPORT_ASSET_DIR`, `PSXPORT_PAD_REPLAY`, `PSXPORT_PAD_RECORD`, `PSXPORT_SBS_PAD_REPLAY`,
`PSXPORT_WAV`, `PSXPORT_GAME_ROOT`, `PSXPORT_WORKDIR`, `PSXPORT_LIBCHDR`. The table's 12th PATH row,
`PSXPORT_LOG_FILE`, belongs to Group 7 — it is lucent's own configuration, and its only "read site"
in this tree is a `setenv` inside `tests/test_lucent_channel_env.cpp:118`. The games' own disc knobs
(`PSXPORT_SPIDERMAN_DISC`, `PSXPORT_SPYRO_DISC`, `PSXPORT_SPIDERMAN_CARD`) are game-registered CVars
using the same mechanism; they are not framework rows.
This group is few, high-traffic, and each knob has a documented resolution order currently
implemented three different ways (`disc.cpp:54`, `common/env.cpp:50`, `run.sh:66`). Folding `common/env.cpp`'s `.env`
walk into the CVar loader as a genuine layer — below `Override`, above `Value` — is the one place
where the migration removes a mechanism rather than adding one. *Gate:* the existing disc-resolution
order (CLI > env > `.env` > drop-in `*.chd`) must hold, tested hermetically with a temp dir.

**Group 4 — BEHAVIOUR, 60 knobs, in three sub-batches.** These are the knobs that change what the
program does, so each needs a runtime check as well as a unit test.
- **4a, run mode (7):** `PSXPORT_GATE`, `PSXPORT_RENDER_PSX`, `PSXPORT_ORACLE`, `PSXPORT_ENH`,
  `PSXPORT_SBS`, `PSXPORT_SBS_MODE`, `PSXPORT_NOPACE`. The five canonical run flags plus pacing.
  `oracle_mode()`/`cfg_enh()`'s suppression rule (qualification 2) lands here, keeping its warning.
  *Gate:* the boot gate at each of the 5 documented flag combinations reaches at least as far as
  before; `PSXPORT_ENH=all PSXPORT_ORACLE=1` still logs the suppression warning and returns 0.
- **4b, presentation and lifetime (14):** `VK_HEADLESS`, `VK_WINDOW`, `WINDOWED`, `FULLSCREEN`,
  `PRESENT_SINK`, `NOAUDIO`, `NO_FMV`, `WATCHDOG`, `WATCHDOG_BOOT`, `REPL`, `DEBUG_SERVER`,
  `AUTO_SKIP`, `NATIVE_FRAMES`, `GPU_DEBUG`. Note `WATCHDOG`'s default of 3 s and `WATCHDOG_BOOT`'s
  `max(secs,45)` (`watchdog.cpp:106,115`) are undocumented — becoming a `ConfigVar` default is what
  makes them visible. *Gate:* headless boot gate, and `PSXPORT_WATCHDOG=1` still fires.
- **4c, the rest (39):** pad forcing, FMV tuning, interpreter windows, render layer isolation, z-bias.
  Lower traffic, migrate in file order.

**Group 5 — DEV-ONLY, 15 knobs.** Selftest and mirror-verify scaffolding. Isolated by construction;
`PSXPORT_SBS_CANARY` in particular is itself a negative control and must keep tripping.

**Group 6 — DIAGNOSTIC, 84 knobs, LAST and largest.** 40 of them are `PSXPORT_SBS_*` read by raw
`getenv` in `sbs.cpp` alone, most inside a `static const … = []{…}()` initialiser — the value is
latched on first use, so a CVar that can change at runtime is a behaviour change, not a refactor.
Migrate `sbs.cpp` as one unit, keeping the latch explicit. **A large fraction of this group should be
DELETED rather than migrated**: `PROVAT`, `PRIMDUMP`, `POLYDUMP`, `PIXTRACE`, `SEMIDUMP`, `GRAMDUMP`,
`CLOBBERDUMP` and their `_AT`/`_BT` companions are per-investigation instruments, and a knob that
exists to answer one question that has been answered is dead weight the next reader has to rule out.
Every one kept must be justified by a live use; the rest go, doc line and all.

**Group 7 — `PSXPORT_DEBUG` and `PSXPORT_LOG_FILE`.** Visibility-only registration, per the section
above. `tests/test_lucent_channel_env.cpp` unmodified and green.

**Not in any group, do first, costs nothing:** delete the 74 dead names' doc lines and the 29-row
tombstone table. That is 103 fewer names for every later group to reason about, and it is pure
subtraction — no code changes.

---

## Method, and what this map does NOT establish

**Method.** `scratch/cfgscan/scan2.py` walks the trees and buckets every `PSXPORT_*` occurrence as
`read-c` / `read-env` / `read-py` / `read-sh` / `macro` / `guard` / `cmake` / `setenv` / `doc` /
`mention`; `final.py` applies the false-positive filters and joins against `docs/config.md`;
`classify.py` carries the per-knob CLASS. The scripts live in the gitignored `scratch/` — they are
not part of the framework diff. If this map is worth regenerating, they belong in `tools/`.

**Negative controls actually built in.** The scan exits non-zero if a scan root is missing rather than
reporting an empty inventory. Every rejected read is printed with its reason, not dropped — all six:
`config.md:86,87,88,752` (doc code-blocks), `game_iface.h:71` and `native_boot.cpp:629` (comments
quoting a call). The class map prints any framework-live knob it does not cover; it currently covers
171 of 171. Every "0 reads" verdict carries the file/line denominator it was computed over.

**What is NOT established here, stated so nobody reads this map as more than it is.**

- **CLASS is a judgement.** Each of the 171 was assigned by reading its call-site line, not by a name
  heuristic — but the BEHAVIOUR/DIAGNOSTIC line is genuinely blurry for the render-isolation knobs
  (`NOBG`, `NOHUD`, `ONLYWORLD`, `PAINTER*`), which are diagnostics *implemented by changing what is
  drawn*. They are classed BEHAVIOUR here because that is what they do.
- **Doc-vs-code was checked mechanically for NAME and LIVENESS only.** Semantics — defaults, accepted
  value syntax — were spot-checked on five knobs: `PSXPORT_ZFIGHT` (doc `config.md:483` matches
  `render_queue.cpp:259-265` exactly, including the `=1`-means-default special case),
  `PSXPORT_CW_MAX` (doc "default 64" matches `mem.cpp:62`), `PSXPORT_MIRROR_VERIFY_EVERY` (doc
  "default 1" matches `verify_harness.cpp:277`), `PSXPORT_WATCHDOG` and `PSXPORT_WATCHDOG_BOOT`
  (defaults 3 and `max(secs,45)` — **undocumented**). The other 83 documented knobs were not
  semantically audited.
- **One doc/code mismatch was found and is NOT fixed here:** `PSXPORT_GPU_LOG` sits in the retired
  "old env flag → channel" table at `config.md:165` (mapped to channel `gpu`), yet
  `gpu_native.cpp:1924` still reads it — `if (lucent::channel_on("gpu") || cfg_on("PSXPORT_GPU_LOG"))`.
  It works; the doc says it should not exist. `docs/config.md` is owned by a sibling agent under this
  same claim.
- **`PSXPORT_SS`** (`config.md:724`, in a list of "valued diagnostics") is the one name in M whose
  identity is uncertain — a two-letter token in a backticked list. Nothing reads it under that name.
  Listed as dead; if someone knows it by another name, correct this row rather than deleting it.
- **`PSXPORT_VK_SHOT` / `VK_SHOTSEQ` / `VK_DIFF` / `GPUTRACE` / `XA_DBG`** are listed as live valued
  diagnostics at `config.md:722-724`, and `Tomba2Engine/docs/render-arch.md:111` states plainly that
  "`PSXPORT_VK_SHOT`/`VK_SHOTSEQ` env vars are gone". The framework doc was never updated. Note also
  `GPUTRACE` vs the live `PSXPORT_GPU_TRACE` — a one-character doc name that has never matched code.
- **No code was run.** This is a static inventory. It cannot tell you that a knob with a read site is
  ever *reached* on a real run — only that the read site exists. Group 1's
  registered-but-never-read list is what converts this map into a runtime fact.

---

## The inventory

### BEHAVIOUR — changes what the program does — 60 knobs

| knob | type | sites | read at | doc |
|---|---|---|---|---|
| `PSXPORT_AUTO_SKIP` | string | 2 | `runtime/recomp/native_boot.cpp:297` · `runtime/recomp/native_boot.cpp:344` | config.md:697 |
| `PSXPORT_DEBUG_SERVER` | bool | 3 | `runtime/recomp/dbg_server.cpp:461` · `runtime/recomp/sbs.cpp:2091` · `runtime/recomp/native_boot.cpp:305` | config.md:559 |
| `PSXPORT_DUALVIEW` | string | 1 | `runtime/recomp/native_boot.cpp:615` | **none** |
| `PSXPORT_ENH` | string | 1 | `runtime/recomp/cfg.cpp:138` | config.md:90,127 |
| `PSXPORT_FMV_DCONLY` | bool | 1 | `runtime/recomp/fmv_decode.cpp:269` | **none** |
| `PSXPORT_FMV_FPS` | int(str) | 1 | `runtime/recomp/native_fmv.cpp:215` | **none** |
| `PSXPORT_FMV_MAXFRAMES` | int | 2 | `runtime/recomp/native_fmv.cpp:199` · `tools/fmv_export/fmv_export.cpp:282` | **none** |
| `PSXPORT_FMV_ROWMAJOR` | bool | 1 | `runtime/recomp/fmv_decode.cpp:473` | **none** |
| `PSXPORT_FORCE_BUTTONS` | string | 1 | `runtime/recomp/pad_input.cpp:347` | **none** |
| `PSXPORT_FORCE_HOLD` | string | 1 | `runtime/recomp/pad_input.cpp:352` | **none** |
| `PSXPORT_FORCE_HOLD_AT` | hex-int | 1 | `runtime/recomp/pad_input.cpp:354` | **none** |
| `PSXPORT_FORCE_STOP_AT` | int(str) | 1 | `runtime/recomp/pad_input.cpp:365` | **none** |
| `PSXPORT_FPS60_TFORCE` | int | 1 | `runtime/recomp/fps60.cpp:356` | config.md:542 |
| `PSXPORT_FULLSCREEN` | bool | 1 | `runtime/recomp/gpu_vk.cpp:535` | config.md:557 |
| `PSXPORT_GATE` | string | 1 | `runtime/recomp/native_boot.cpp:604` | config.md:11,13 |
| `PSXPORT_GPU_DEBUG` | bool | 1 | `runtime/recomp/gpu_vk.cpp:546` | config.md:553 |
| `PSXPORT_INTERP_DEPTH` | bool | 1 | `runtime/recomp/interp.cpp:112` | **none** |
| `PSXPORT_INTERP_FUNCS` | string | 1 | `runtime/recomp/interp.cpp:364` | **none** |
| `PSXPORT_NATIVE_FRAMES` | int | 1 | `runtime/recomp/native_boot.cpp:301` | config.md:558 |
| `PSXPORT_NOAUDIO` | bool | 2 | `runtime/recomp/native_fmv.cpp:119` · `runtime/recomp/spu_audio.cpp:94` | config.md:558 |
| `PSXPORT_NOBG` | int(str) | 1 | `runtime/recomp/render_queue.cpp:470` | config.md:476 |
| `PSXPORT_NOHUD` | int(str) | 1 | `runtime/recomp/render_queue.cpp:474` | config.md:477 |
| `PSXPORT_NOPACE` | bool | 1 | `runtime/recomp/gpu_native.cpp:1542` | config.md:558 |
| `PSXPORT_NO_FMV` | bool | 2 +game:Tomba2Engine | `runtime/recomp/native_boot.cpp:643` · `runtime/recomp/native_boot.cpp:644` | config.md:558 |
| `PSXPORT_ONLYWORLD` | int(str) | 1 | `runtime/recomp/render_queue.cpp:466` | config.md:476 |
| `PSXPORT_ORACLE` | bool | 1 | `runtime/recomp/cfg.cpp:125` | config.md:51,91 |
| `PSXPORT_PAD_NOPAD` | int(str) | 1 | `runtime/recomp/pad_input.cpp:106` | **none** |
| `PSXPORT_PCTRAP` | hex-int | 1 | `runtime/recomp/interp.cpp:539` | **none** |
| `PSXPORT_PCTRAP_SKIP` | hex-int | 1 | `runtime/recomp/interp.cpp:540` | **none** |
| `PSXPORT_PRESENT_SINK` | string | 1 | `runtime/recomp/gpu_vk.cpp:185` | **none** |
| `PSXPORT_RENDER_PSX` | string | 1 +game:spyro | `runtime/recomp/native_boot.cpp:610` | config.md:12,13 |
| `PSXPORT_REPL` | bool | 1 +game:spyro | **MIGRATED 2026-08-12** → `cv_repl` (`config_vars.h`); read by `native_boot.cpp` (the only pump) + `repl_service.cpp` (the refusal for loops with none) | config.md:559 |
| `PSXPORT_SBS` | bool | 1 +game:Tomba2Engine | `runtime/recomp/cfg.cpp:140` | config.md:724 |
| `PSXPORT_SBS_ARMSLOT` | string | 1 | `runtime/recomp/sbs.cpp:2251` | config.md:613 |
| `PSXPORT_SBS_AUTONAV` | string | 2 | `runtime/recomp/sbs.cpp:133` · `runtime/recomp/sbs.cpp:2096` | config.md:449,588 |
| `PSXPORT_SBS_CUT_PRESSES` | int(str) | 1 | `runtime/recomp/sbs.cpp:575` | **none** |
| `PSXPORT_SBS_EXIT_FRAME` | int | 1 | `runtime/recomp/sbs.cpp:2835` | config.md:583 |
| `PSXPORT_SBS_FORCES4C` | string | 1 | `runtime/recomp/sbs.cpp:2278` | config.md:602 |
| `PSXPORT_SBS_FORCE_PSX_RENDER` | string | 1 | `runtime/recomp/sbs.cpp:655` | **none** |
| `PSXPORT_SBS_KEYS` | string | 1 | `runtime/recomp/sbs.cpp:1330` | **none** |
| `PSXPORT_SBS_MODE` | bool | 2 +game:Tomba2Engine | `runtime/recomp/cfg.cpp:140` · `runtime/recomp/sbs.cpp:1875` | config.md:14,561 |
| `PSXPORT_SBS_NOPAUSE` | string | 1 | `runtime/recomp/sbs.cpp:1094` | config.md:641 |
| `PSXPORT_SBS_NOPRESENT` | string | 1 | `runtime/recomp/sbs.cpp:1225` | **none** |
| `PSXPORT_SBS_ONLY_LABEL` | string | 1 | `runtime/recomp/sbs.cpp:1098` | config.md:642 |
| `PSXPORT_SBS_POSTDRIVE` | string | 1 | `runtime/recomp/sbs.cpp:103` | **none** |
| `PSXPORT_SBS_PRENAV` | string | 1 | `runtime/recomp/sbs.cpp:2181` | **none** |
| `PSXPORT_SBS_SKIPTICK` | string | 1 | `runtime/recomp/sbs.cpp:933` | config.md:645 |
| `PSXPORT_SBS_SKIP_CONTINUE` | string | 1 | `runtime/recomp/sbs.cpp:984` | config.md:573 |
| `PSXPORT_SBS_WARP` | string | 1 | `runtime/recomp/sbs.cpp:2196` | config.md:609 |
| `PSXPORT_SBS_WATCH_CUT` | string | 1 | `runtime/recomp/sbs.cpp:574` | **none** |
| `PSXPORT_SUBSTRATE_HI` | string | 1 | `runtime/recomp/interp.cpp:480` | **none** |
| `PSXPORT_SUBSTRATE_LO` | string | 1 | `runtime/recomp/interp.cpp:480` | **none** |
| `PSXPORT_THUNK_FORCE_GEN` | string | 1 | `runtime/recomp/override_registry.cpp:45` | **none** |
| `PSXPORT_VK_HEADLESS` | bool | 1 +game:Tomba2Engine | `runtime/recomp/gpu_vk.cpp:140` | config.md:541,557 |
| `PSXPORT_VK_WINDOW` | bool | 1 | `runtime/recomp/gpu_vk.cpp:140` | **none** |
| `PSXPORT_WATCHDOG` | string | 1 | `runtime/recomp/watchdog.cpp:105` | config.md:559 |
| `PSXPORT_WATCHDOG_BOOT` | string | 1 | `runtime/recomp/watchdog.cpp:114` | config.md:554 |
| `PSXPORT_WINDOWED` | int(str) | 1 | `runtime/recomp/gpu_vk.cpp:536` | config.md:542,557 |
| `PSXPORT_ZBIAS` | string | 1 | `runtime/recomp/gpu_vk.cpp:1581` | config.md:499,504 |
| `PSXPORT_ZBIAS_MAX` | string | 1 | `runtime/recomp/gpu_vk.cpp:1586` | config.md:503 |

### DIAGNOSTIC — turns observation on — 84 knobs

| knob | type | sites | read at | doc |
|---|---|---|---|---|
| `PSXPORT_BGMDBG` | string | 1 | `runtime/recomp/native_boot.cpp:513` | config.md:724 |
| `PSXPORT_CLOBBERDUMP` | path | 3 | `runtime/recomp/gpu_native.cpp:1393` · `runtime/recomp/gpu_native.cpp:1398` · `runtime/recomp/gpu_native.cpp:1400` | config.md:723 |
| `PSXPORT_CLUTWATCH` | string | 1 | `runtime/recomp/gpu_native.cpp:1926` | config.md:723 |
| `PSXPORT_CW` | string | 1 | `runtime/recomp/mem.cpp:52` | config.md:723,741 |
| `PSXPORT_CW_BT` | string | 1 | `runtime/recomp/mem.cpp:65` | config.md:723 |
| `PSXPORT_CW_MAX` | int | 1 | `runtime/recomp/mem.cpp:62` | config.md:741 |
| `PSXPORT_DC_ALL` | bool | 1 | `runtime/recomp/dualcore.cpp:144` | config.md:746 |
| `PSXPORT_DC_HI` | hex-int | 1 | `runtime/recomp/dualcore.cpp:148` | config.md:745 |
| `PSXPORT_DC_LO` | hex-int | 1 | `runtime/recomp/dualcore.cpp:147` | config.md:745 |
| `PSXPORT_DC_N` | int | 1 | `runtime/recomp/dualcore.cpp:145` | config.md:744 |
| `PSXPORT_DEBUG` | string | 3 | `tests/test_lucent_channel_env.cpp:65` · `tests/test_lucent_channel_env.cpp:76` · `tests/test_lucent_channel_env.cpp:87` | config.md:19,71 |
| `PSXPORT_DERAIL_DUMP` | path | 1 | `runtime/recomp/interp.cpp:334` | **none** |
| `PSXPORT_DISPWATCH` | string | 1 | `runtime/recomp/overlay_router.cpp:294` | config.md:387 |
| `PSXPORT_FADEDBG` | string | 1 | `runtime/recomp/gpu_native.cpp:1763` | config.md:722 |
| `PSXPORT_FNTRACE` | string | 1 | `runtime/recomp/fntrace.cpp:171` | **none** |
| `PSXPORT_FNTRACE_REGS` | int(str) | 1 | `runtime/recomp/fntrace.cpp:170` | **none** |
| `PSXPORT_GP0RAW` | int(str) | 1 | `runtime/recomp/gpu_native.cpp:1222` | **none** |
| `PSXPORT_GPU_DUMP` | path | 1 | `runtime/recomp/gpu_native.cpp:1693` | config.md:724 |
| `PSXPORT_GPU_LOG` | bool | 1 | `runtime/recomp/gpu_native.cpp:1924` | **MISMATCH** — listed as RETIRED at config.md:165, still read |
| `PSXPORT_GPU_TRACE` | bool | 3 | `runtime/recomp/gpu_vk.cpp:1034` · `runtime/recomp/gpu_vk.cpp:1346` · `runtime/recomp/gpu_vk.cpp:1836` | config.md:552 |
| `PSXPORT_GRAMDUMP` | path | 1 | `runtime/recomp/gpu_native.cpp:1714` | **none** |
| `PSXPORT_GTEPROBE` | int(str) | 1 | `runtime/recomp/gte_beetle.cpp:382` | config.md:721 |
| `PSXPORT_INTERP_TRACE` | string | 1 | `runtime/recomp/native_stub.cpp:110` | **none** |
| `PSXPORT_MISS_RAMDUMP` | path | 1 | `runtime/recomp/hle.cpp:684` | **none** |
| `PSXPORT_NCALL_TRACE` | string | 1 | `runtime/recomp/interp.cpp:66` | **none** |
| `PSXPORT_NDIFF` | int(str) | 1 | `runtime/recomp/native_diff.cpp:41` | **none** |
| `PSXPORT_NDIFF_MAXDIFF` | int(str) | 1 | `runtime/recomp/native_diff.cpp:42` | **none** |
| `PSXPORT_PAD_DUMP_AT` | path | 1 | `runtime/recomp/pad_input.cpp:474` | **none** |
| `PSXPORT_PAD_SHOT_AT` | string | 1 | `runtime/recomp/pad_input.cpp:460` | **none** |
| `PSXPORT_PAD_TRACE` | string | 1 | `runtime/recomp/pad_input.cpp:493` | **none** |
| `PSXPORT_PAINTER` | int(str) | 2 | `runtime/recomp/gpu_native.cpp:866` · `runtime/recomp/gpu_native.cpp:1001` | **none** |
| `PSXPORT_PAINTFG` | int(str) | 1 | `runtime/recomp/gpu_native.cpp:841` | **none** |
| `PSXPORT_PAINTWORLD` | int(str) | 1 | `runtime/recomp/render_queue.cpp:460` | **none** |
| `PSXPORT_PIXTRACE` | string | 1 | `runtime/recomp/gpu_native.cpp:416` | **none** |
| `PSXPORT_POLYAT` | tuple | 2 | `runtime/recomp/gpu_native.cpp:907` · `runtime/recomp/gpu_native.cpp:954` | config.md:722 |
| `PSXPORT_POLYDUMP` | int(str) | 2 | `runtime/recomp/gpu_native.cpp:906` · `runtime/recomp/gpu_native.cpp:953` | config.md:722 |
| `PSXPORT_PRESENT_SHOT_AT` | string | 1 | `runtime/recomp/gpu_native.cpp:1868` | **none** |
| `PSXPORT_PRIMAT` | tuple | 2 | `runtime/recomp/gpu_native.cpp:828` · `runtime/recomp/render_queue.cpp:490` | config.md:477 |
| `PSXPORT_PRIMDUMP` | path | 1 | `runtime/recomp/gpu_native.cpp:159` | **none** |
| `PSXPORT_PROF` | bool | 1 | `runtime/recomp/hostprof.cpp:107` | **none** |
| `PSXPORT_PROF_HZ` | int(str) | 1 | `runtime/recomp/hostprof.cpp:109` | **none** |
| `PSXPORT_PROF_OUT` | path | 1 | `runtime/recomp/hostprof.cpp:81` | **none** |
| `PSXPORT_PROJPROBE` | bool | 2 | `runtime/recomp/gte_beetle.cpp:402` · `runtime/recomp/gpu_native.cpp:1635` | config.md:550 |
| `PSXPORT_PROVAT` | string | 2 | `runtime/recomp/gpu_native.cpp:603` · `runtime/recomp/gpu_native.cpp:1679` | config.md:721 |
| `PSXPORT_RAMDUMP` | path | 2 | `runtime/recomp/native_boot.cpp:558` · `runtime/recomp/native_boot.cpp:577` | config.md:723 |
| `PSXPORT_RAMDUMP_FRAME` | path | 1 | `runtime/recomp/native_boot.cpp:555` | config.md:723 |
| `PSXPORT_SBS_ALLOCRA_ALL` | string | 1 | `runtime/recomp/sbs.cpp:1680` | **none** |
| `PSXPORT_SBS_ALLOCTRACE` | string | 1 | `runtime/recomp/sbs.cpp:1901` | **none** |
| `PSXPORT_SBS_BYTETRACE` | string | 1 | `runtime/recomp/sbs.cpp:1890` | config.md:639 |
| `PSXPORT_SBS_BYTETRACE_ALL` | string | 1 | `runtime/recomp/sbs.cpp:1722` | **none** |
| `PSXPORT_SBS_BYTETRACE_STRICT` | string | 1 | `runtime/recomp/sbs.cpp:1737` | **none** |
| `PSXPORT_SBS_DUMP` | path | 1 | `runtime/recomp/sbs.cpp:2098` | **none** |
| `PSXPORT_SBS_FRAMEPROF` | string | 1 | `runtime/recomp/sbs.cpp:1904` | config.md:629 |
| `PSXPORT_SBS_HI` | hex-int | 1 | `runtime/recomp/sbs.cpp:1884` | **none** |
| `PSXPORT_SBS_LASTWRITER` | string | 1 | `runtime/recomp/sbs.cpp:2005` | **none** |
| `PSXPORT_SBS_LO` | hex-int | 1 | `runtime/recomp/sbs.cpp:1883` | **none** |
| `PSXPORT_SBS_LW_ADDR` | string | 1 | `runtime/recomp/sbs.cpp:795` | **none** |
| `PSXPORT_SBS_PREWATCH` | string | 1 | `runtime/recomp/sbs.cpp:2074` | config.md:634 |
| `PSXPORT_SBS_REGDIFF` | string | 1 | `runtime/recomp/sbs.cpp:1912` | config.md:624 |
| `PSXPORT_SBS_RENDERDIFF` | string | 1 | `runtime/recomp/sbs.cpp:1243` | config.md:575 |
| `PSXPORT_SBS_RENDERDIFF_FROM` | string | 1 | `runtime/recomp/sbs.cpp:1282` | config.md:643 |
| `PSXPORT_SBS_SHOT` | string | 1 | `runtime/recomp/sbs.cpp:2317` | **none** |
| `PSXPORT_SBS_STAGETRACE` | int(str) | 1 | `runtime/recomp/sbs.cpp:2136` | **none** |
| `PSXPORT_SBS_UPPROBE` | hex-int | 1 | `runtime/recomp/sbs.cpp:1421` | **none** |
| `PSXPORT_SBS_WW_FROMFRAME` | hex-int | 1 | `runtime/recomp/sbs.cpp:1510` | config.md:635 |
| `PSXPORT_SBS_WW_ONVALUEDIVERGE` | string | 2 | `runtime/recomp/sbs.cpp:1496` · `runtime/recomp/sbs.cpp:2383` | config.md:637 |
| `PSXPORT_SCENEDUMP` | int(str) | 1 | `runtime/recomp/gpu_native.cpp:1987` | config.md:721 |
| `PSXPORT_SEMIDUMP` | int(str) | 1 | `runtime/recomp/gpu_native.cpp:250` | config.md:722 |
| `PSXPORT_SHOT_AT` | string | 1 | `runtime/recomp/gpu_native.cpp:1846` | **none** |
| `PSXPORT_SNAP_AT` | string | 1 | `runtime/recomp/snapshot.cpp:44` | **none** |
| `PSXPORT_SNAP_EVERY` | hex-int | 1 | `runtime/recomp/snapshot.cpp:45` | **none** |
| `PSXPORT_SNAP_MAX` | int(str) | 1 | `runtime/recomp/snapshot.cpp:46` | **none** |
| `PSXPORT_SPUBT` | string | 1 | `runtime/recomp/spu_beetle.cpp:187` | **none** |
| `PSXPORT_SPUDMA` | string | 1 | `runtime/recomp/mem.cpp:650` | **none** |
| `PSXPORT_TEXEXPORT` | int(str) | 1 | `runtime/recomp/gpu_native.cpp:966` | **none** |
| `PSXPORT_TEXWATCH` | string | 1 | `runtime/recomp/gpu_native.cpp:644` | **none** |
| `PSXPORT_TEXWATCH_BT` | string | 1 | `runtime/recomp/gpu_native.cpp:1369` | **none** |
| `PSXPORT_VRAMDUMP` | path | 1 | `runtime/recomp/gpu_native.cpp:1700` | config.md:723 |
| `PSXPORT_VRAMDUMP_AT` | path | 1 | `runtime/recomp/gpu_native.cpp:1686` | config.md:723 |
| `PSXPORT_WWATCH` | string | 2 | `runtime/recomp/mem.cpp:85` · `runtime/recomp/mem.cpp:90` | config.md:723,725 |
| `PSXPORT_WWATCH_BT` | string | 1 | `runtime/recomp/mem.cpp:95` | config.md:727 |
| `PSXPORT_ZFIGHT` | string | 1 | `runtime/recomp/render_queue.cpp:259` | config.md:483 |
| `PSXPORT_ZFIGHT_BOX` | tuple | 1 | `runtime/recomp/render_queue.cpp:274` | config.md:489 |
| `PSXPORT_ZFIGHT_FRAME` | int(str) | 1 | `runtime/recomp/render_queue.cpp:265` | config.md:488 |

### PATH/INPUT — where the run gets its bytes — 12 knobs

| knob | type | sites | read at | doc |
|---|---|---|---|---|
| `PSXPORT_ASSET_DIR` | path | 1 | `runtime/recomp/rmlui_overlay.cpp:39` | config.md:39,40 |
| `PSXPORT_DISC` | path | 5 +game:spyro,Tomba2Engine | `runtime/recomp/disc.cpp:54` · `tools/xa_wavdump.c:94` · `tools/fmv_export/test_fmv_decode.cpp:246` +2 more | config.md:560 |
| `PSXPORT_GAME_ROOT` | string | 2 | `tools/abi_extract.py:1265` · `tools/port_check.py:564` | **none** |
| `PSXPORT_LIBCHDR` | string | 1 | `tools/fmv_export/str_decode.py:58` | **none** |
| `PSXPORT_LOG_FILE` | ? | 1 | `tests/test_lucent_channel_env.cpp:118` | config.md:80,93 |
| `PSXPORT_PAD_RECORD` | path | 1 | `runtime/recomp/pad_input.cpp:404` | **none** |
| `PSXPORT_PAD_REPLAY` | path | 2 | `runtime/recomp/pad_input.cpp:411` · `runtime/recomp/pad_input.cpp:432` | **none** |
| `PSXPORT_SBS_PAD_REPLAY` | path | 1 | `runtime/recomp/sbs.cpp:1356` | config.md:25 |
| `PSXPORT_SETTINGS` | path | 1 | `runtime/recomp/mods.cpp:12` | config.md:46 |
| `PSXPORT_TOMBA2_DISC` | path | 10 +game:Tomba2Engine | `tools/abcompare.py:55` · `tools/abcompare.py:62` · `tools/chd_dump_cdda.c:19` +7 more | config.md:560 |
| `PSXPORT_WAV` | path | 1 | `runtime/recomp/spu_audio.cpp:90` | config.md:724 |
| `PSXPORT_WORKDIR` | ? | 1 | `tools/decomp.sh:19` | **none** |

### DEV-ONLY — harness / selftest / build scaffolding — 15 knobs

| knob | type | sites | read at | doc |
|---|---|---|---|---|
| `PSXPORT_CLEAR_NORETURN` | string | 1 | `tools/ghidra_decomp.py:45` | **none** |
| `PSXPORT_FMV_DUMPCODES` | bool | 2 | `tools/fmv_export/fmv_export.cpp:337` · `tools/fmv_export/fmv_export.cpp:348` | **none** |
| `PSXPORT_FNTRACE_SELFTEST` | bool | 1 | `runtime/recomp/fntrace.cpp:119` | **none** |
| `PSXPORT_GPU_SELFTEST` | bool | 1 | `runtime/recomp/gpu_vk.cpp:1751` | config.md:554 |
| `PSXPORT_LABEL_ALL` | string | 1 | `tools/recomp/emit.py:1212` | **none** |
| `PSXPORT_MIRROR_VERIFY` | string | 1 | `runtime/recomp/verify_harness.cpp:63` | config.md:650,659 |
| `PSXPORT_MIRROR_VERIFY_CONTINUE` | bool | 2 | `runtime/recomp/verify_harness.cpp:184` · `runtime/recomp/verify_harness.cpp:251` | config.md:713 |
| `PSXPORT_MIRROR_VERIFY_EVERY` | int | 1 | `runtime/recomp/verify_harness.cpp:277` | config.md:673 |
| `PSXPORT_MIRROR_VERIFY_FULL` | bool | 1 | `runtime/recomp/verify_harness.cpp:119` | config.md:701,710 |
| `PSXPORT_SBS_CANARY` | string | 1 | `runtime/recomp/sbs.cpp:2514` | config.md:22 |
| `PSXPORT_SELFTEST` | string | 1 +game:spider1,spyro,Tomba2Engine | `runtime/recomp/selftest.cpp:681` | **none** |
| `PSXPORT_SELFTEST_VERBOSE` | bool | 4 +game:Tomba2Engine | `runtime/recomp/selftest.cpp:46` · `runtime/recomp/selftest.cpp:162` · `runtime/recomp/selftest.cpp:213` +1 more | **none** |
| `PSXPORT_SHARDS` | string | 1 | `tools/recomp/emit.py:2368` | **none** |
| `PSXPORT_TEST_PREMAIN_CHILD` | string | 1 | `tests/test_lucent_channel_env.cpp:39` | **none** |
| `PSXPORT_USE_GHIDRA` | string | 1 | `tools/recomp/emit.py:2166` | **none** |

### Read ONLY by a game repo — the framework has no read site (35)

These are game-owned knobs. They matter here because the CVar registry has to let a game
register its own CVars, and because 6 of them are documented in the FRAMEWORK's `docs/config.md`.

| knob | read at | in psxport docs/config.md |
|---|---|---|
| `PSXPORT_ANIMTG_ENTRY` | Tomba2Engine `ai/beh_anim_trigger_gates.cpp:51` | no |
| `PSXPORT_BEH_SUBSTRATE` | Tomba2Engine `object/behavior_dispatch.cpp:256` | yes, line 761 |
| `PSXPORT_BEH_TRACE` | Tomba2Engine `object/behavior_dispatch.cpp:223` | no |
| `PSXPORT_CULLQ_OBJ` | Tomba2Engine `ai/area_seaside_perframe.cpp:134` | no |
| `PSXPORT_CULL_FAR` | Tomba2Engine `render/cull.cpp:272` | yes, line 551 |
| `PSXPORT_CULL_FAR_MULT` | Tomba2Engine `render/cull.cpp:91` | no |
| `PSXPORT_CULL_FOV` | Tomba2Engine `render/cull.cpp:273` | yes, line 551 |
| `PSXPORT_CULL_ONLY_TYPE` | Tomba2Engine `render/cull.cpp:307` | no |
| `PSXPORT_CULL_SKIP_TYPE` | Tomba2Engine `render/cull.cpp:308` | no |
| `PSXPORT_DISCDUMP` | Tomba2Engine `ensure_recomp.py:101`; spider1 `ensure_recomp.py:111`; spyro `ensure_recomp.py:132` | no |
| `PSXPORT_DUALCORE` | Tomba2Engine `core/main.cpp:75` | yes, line 744 |
| `PSXPORT_FIELD_SONG` | Tomba2Engine `audio/music_coord.cpp:281` | no |
| `PSXPORT_FLAGBIT_ENTRY` | Tomba2Engine `ai/beh_flagbit_timer_machine.cpp:55` | no |
| `PSXPORT_FLAGBIT_TRACE` | Tomba2Engine `ai/beh_flagbit_timer_machine.cpp:67` | no |
| `PSXPORT_FORCE_RECOMP` | Tomba2Engine `ensure_recomp.py:231`; spider1 `ensure_recomp.py:210`; spyro `ensure_recomp.py:309` | no |
| `PSXPORT_INTERP_FN` | spyro `core/native_render.cpp:173` | no |
| `PSXPORT_MARGIN_POKE` | Tomba2Engine `render/margin_render.cpp:183` | no |
| `PSXPORT_MUTE_FN` | spyro `core/native_render.cpp:149` | no |
| `PSXPORT_NATIVE_TERRAIN` | spyro `core/wide_clip.cpp:264` | no |
| `PSXPORT_NDIFF_IDENTITY` | spyro `core/native_render.cpp:200` | no |
| `PSXPORT_NO_NATIVE` | spyro `core/game_hooks.cpp:70` | no |
| `PSXPORT_PC_SKIP` | Tomba2Engine `core/main.cpp:49` | yes, line 767 |
| `PSXPORT_PRESENT_BURST` | spider1 `core/sync_native.cpp:266` | no |
| `PSXPORT_RECALLOC_TRACE` | Tomba2Engine `world/graphics_bind.cpp:69` | no |
| `PSXPORT_RNG_CALLTRACE` | Tomba2Engine `math/rng.cpp:30` | no |
| `PSXPORT_SELFTEST_ITERS` | Tomba2Engine `render/effect_mod_selftest.cpp:126` | no |
| `PSXPORT_SKIPPASS` | Tomba2Engine `render/render_frame.cpp:32` | no |
| `PSXPORT_SKIP_VERIFY` | Tomba2Engine `core/verify_skip.cpp:16` | no |
| `PSXPORT_SPIDERMAN_CARD` | spider1 `core/game_config.cpp:401` | no |
| `PSXPORT_SPIDERMAN_DISC` | spider1 `core/game_config.cpp:73` | no |
| `PSXPORT_SPYRO_DISC` | spyro `core/main.cpp:39` | no |
| `PSXPORT_SPYRO_FRAME_LOOP` | spyro `core/frame_loop.cpp:300` | no |
| `PSXPORT_SPYRO_NATIVE_RENDER` | spyro `core/frame_loop.cpp:304` | no |
| `PSXPORT_T2_NOSEQTICK` | Tomba2Engine `game_tomba2.cpp:111` | yes, line 559 |
| `PSXPORT_UNPACKDUMP` | Tomba2Engine `core/asset.cpp:80` | no |

### READ BY NOTHING — 74 names

| knob | doc | verdict |
|---|---|---|
| `PSXPORT_ATTACH` | config.md:550 | **documented as LIVE. Delete the doc line (or wire the knob).** |
| `PSXPORT_AUTO_GAMEPLAY` | config.md:558 | **documented as LIVE. Delete the doc line (or wire the knob).** |
| `PSXPORT_AUTO_NEWGAME` | config.md:559 | **documented as LIVE. Delete the doc line (or wire the knob).** |
| `PSXPORT_CULL` | config.md:551 | **documented as LIVE. Delete the doc line (or wire the knob).** |
| `PSXPORT_FPS60` | config.md:542 | **documented as LIVE. Delete the doc line (or wire the knob).** |
| `PSXPORT_FPS60_GATE` | config.md:542 | **documented as LIVE. Delete the doc line (or wire the knob).** |
| `PSXPORT_FPS60_SYNTH` | config.md:542 | **documented as LIVE. Delete the doc line (or wire the knob).** |
| `PSXPORT_GPUTRACE` | config.md:722 | **documented as LIVE. Delete the doc line (or wire the knob).** |
| `PSXPORT_GPU_WINDOW` | config.md:542 | **documented as LIVE. Delete the doc line (or wire the knob).** |
| `PSXPORT_IRES` | config.md:542 | **documented as LIVE. Delete the doc line (or wire the knob).** |
| `PSXPORT_LIGHT` | config.md:546 | **documented as LIVE. Delete the doc line (or wire the knob).** |
| `PSXPORT_LIGHT_AMBIENT` | config.md:547 | **documented as LIVE. Delete the doc line (or wire the knob).** |
| `PSXPORT_LIGHT_DIFFUSE` | config.md:547 | **documented as LIVE. Delete the doc line (or wire the knob).** |
| `PSXPORT_LIGHT_DIR` | config.md:547 | **documented as LIVE. Delete the doc line (or wire the knob).** |
| `PSXPORT_NATIVE_DEPTH` | config.md:545 | **documented as LIVE. Delete the doc line (or wire the knob).** |
| `PSXPORT_NOSKIP` | config.md:558 | **documented as LIVE. Delete the doc line (or wire the knob).** |
| `PSXPORT_SCEA_SKIP` | config.md:559 | **documented as LIVE. Delete the doc line (or wire the knob).** |
| `PSXPORT_SS` | config.md:724 | **documented as LIVE. Delete the doc line (or wire the knob).** |
| `PSXPORT_SSAO` | config.md:546 | **documented as LIVE. Delete the doc line (or wire the knob).** |
| `PSXPORT_SSAO_BIAS` | config.md:546 | **documented as LIVE. Delete the doc line (or wire the knob).** |
| `PSXPORT_SSAO_RADIUS` | config.md:546 | **documented as LIVE. Delete the doc line (or wire the knob).** |
| `PSXPORT_SSAO_RANGE` | config.md:546 | **documented as LIVE. Delete the doc line (or wire the knob).** |
| `PSXPORT_SSAO_STRENGTH` | config.md:546 | **documented as LIVE. Delete the doc line (or wire the knob).** |
| `PSXPORT_SSAO_VIZ` | config.md:546,547 | **documented as LIVE. Delete the doc line (or wire the knob).** |
| `PSXPORT_SW_GPU` | config.md:541 | **documented as LIVE. Delete the doc line (or wire the knob).** |
| `PSXPORT_TOMBA2_CARD` | config.md:560 | **documented as LIVE. Delete the doc line (or wire the knob).** |
| `PSXPORT_TRANSPLANT` | config.md:551 | **documented as LIVE. Delete the doc line (or wire the knob).** |
| `PSXPORT_UI` | config.md:548 | **documented as LIVE. Delete the doc line (or wire the knob).** |
| `PSXPORT_VK` | config.md:541 | **documented as LIVE. Delete the doc line (or wire the knob).** |
| `PSXPORT_VK_DIFF` | config.md:722 | **documented as LIVE. Delete the doc line (or wire the knob).** |
| `PSXPORT_VK_NODEPTH` | config.md:541 | **documented as LIVE. Delete the doc line (or wire the knob).** |
| `PSXPORT_VK_SHOT` | config.md:722 | **documented as LIVE. Delete the doc line (or wire the knob).** |
| `PSXPORT_VK_SHOTSEQ` | config.md:722 | **documented as LIVE. Delete the doc line (or wire the knob).** |
| `PSXPORT_VK_TRITEST` | config.md:541 | **documented as LIVE. Delete the doc line (or wire the knob).** |
| `PSXPORT_WIDE` | config.md:542 | **documented as LIVE. Delete the doc line (or wire the knob).** |
| `PSXPORT_XA_DBG` | config.md:724 | **documented as LIVE. Delete the doc line (or wire the knob).** |
| `PSXPORT_AUDIO_LOG` | config.md:155 | tombstone row in the retired→channel table — delete the row |
| `PSXPORT_AUDIO_RATE` | config.md:156 | tombstone row in the retired→channel table — delete the row |
| `PSXPORT_CARD_VERBOSE` | config.md:157 | tombstone row in the retired→channel table — delete the row |
| `PSXPORT_CDCMD_DBG` | config.md:158,169 | tombstone row in the retired→channel table — delete the row |
| `PSXPORT_CDC_VERBOSE` | config.md:159 | tombstone row in the retired→channel table — delete the row |
| `PSXPORT_CD_VERBOSE` | config.md:160 | tombstone row in the retired→channel table — delete the row |
| `PSXPORT_ENGINE_DBG` | config.md:161 | tombstone row in the retired→channel table — delete the row |
| `PSXPORT_ENVDBG` | config.md:162 | tombstone row in the retired→channel table — delete the row |
| `PSXPORT_FMV_DEBUG` | config.md:163 | tombstone row in the retired→channel table — delete the row |
| `PSXPORT_FPS60_SDBG` | config.md:169 | tombstone row in the retired→channel table — delete the row |
| `PSXPORT_GP1LOG` | config.md:164 | tombstone row in the retired→channel table — delete the row |
| `PSXPORT_LDHAZARD` | config.md:166 | tombstone row in the retired→channel table — delete the row |
| `PSXPORT_OBJLOG` | config.md:167 | tombstone row in the retired→channel table — delete the row |
| `PSXPORT_OTDBG` | config.md:168 | tombstone row in the retired→channel table — delete the row |
| `PSXPORT_REDDBG` | config.md:155 | tombstone row in the retired→channel table — delete the row |
| `PSXPORT_RTPCALLER` | config.md:156 | tombstone row in the retired→channel table — delete the row |
| `PSXPORT_SCHEDDBG` | config.md:157 | tombstone row in the retired→channel table — delete the row |
| `PSXPORT_SEQDBG` | config.md:158 | tombstone row in the retired→channel table — delete the row |
| `PSXPORT_SPINDBG` | config.md:159 | tombstone row in the retired→channel table — delete the row |
| `PSXPORT_SPU_DBG` | config.md:160 | tombstone row in the retired→channel table — delete the row |
| `PSXPORT_SPU_PROF` | config.md:161 | tombstone row in the retired→channel table — delete the row |
| `PSXPORT_STAGETL` | config.md:162 | tombstone row in the retired→channel table — delete the row |
| `PSXPORT_STREAMDBG` | config.md:163 | tombstone row in the retired→channel table — delete the row |
| `PSXPORT_TEXTDBG` | config.md:164 | tombstone row in the retired→channel table — delete the row |
| `PSXPORT_UNPACKLOG` | config.md:165 | tombstone row in the retired→channel table — delete the row |
| `PSXPORT_UPLOADLOG` | config.md:166 | tombstone row in the retired→channel table — delete the row |
| `PSXPORT_VRAMSCAN` | config.md:168 | tombstone row in the retired→channel table — delete the row |
| `PSXPORT_VSYNCLOG` | config.md:167 | tombstone row in the retired→channel table — delete the row |
| `PSXPORT_WS_SXHIST` | config.md:172 | tombstone row in the retired→channel table — delete the row |
| `PSXPORT_CARD` | — | no doc, no reader; a stale mention in a comment/journal only |
| `PSXPORT_DERAIL` | — | no doc, no reader; a stale mention in a comment/journal only |
| `PSXPORT_HOOKS` | — | no doc, no reader; a stale mention in a comment/journal only |
| `PSXPORT_MDEC_OFFS` | — | no doc, no reader; a stale mention in a comment/journal only |
| `PSXPORT_NATIVE_BOOT` | — | no doc, no reader; a stale mention in a comment/journal only |
| `PSXPORT_RQ` | — | no doc, no reader; a stale mention in a comment/journal only |
| `PSXPORT_SBS_PCFAITHFUL` | — | no doc, no reader; a stale mention in a comment/journal only |
| `PSXPORT_SPRITEDBG` | — | no doc, no reader; a stale mention in a comment/journal only |
| `PSXPORT_TOMBA2_D` | — | no doc, no reader; a stale mention in a comment/journal only |
