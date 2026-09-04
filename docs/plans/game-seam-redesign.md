# The game seam, second generation — from one flat config to a typed seam

**Status: PARTIAL.** The inheritance slice and first immutable fact slice are implemented:
`GameRuntime` installation, `GuestProgramImage`, per-Core
context lifecycle, override registration, boot initialization, and per-Game `FrameDriver` /
`TaskScheduler` factories. `Game` owns the factory products. `FrameLoopShell` now requires a driver
and delegates `dc_step_frame`/standalone stepping to it; product boot rejects the former guest-owned
loop route. `PlatformHlePlan::vsyncAddress` is mandatory for direct products, with the legacy adapter
carrying the same measured fact, and all VSync modes bind to one fatal non-replaceable framework trap.
Current ports remain source-compatible
through a bounded `LegacyGameRuntimeAdapter`; the adapter exposes no virtual config getter, and the
new smoke derives `GameRuntime` with both legacy views null. The crt0, resident-MAIN router, and
backtrace heuristic now consume the runtime-owned image rather than `c->cfg`; the legacy adapter owns
the only projection from old fields. The control-flow moves in steps 5–7 and
the final `GameConfig` diet remain outstanding. Written 2026-08-11 against the psxport dev clone at
`10c37cf5` (working tree dirty with the render-noise/ot_attr work of the same session — the packet-pool
literal fixes referenced below are in that tree, some uncommitted). Every citation is `file:line` in
that state; read a line number as "where this was when written". Companion docs:
`docs/plans/render-path-tristate.md` (the render seam's switch, LANDED), `docs/plans/graphics-producer-db.md`
(a consumer of the diagnostics seam this plan reshapes).

A second agent is implementing §7 (the lint) in parallel from this same document; §7 is written to be
buildable without reading the rest.

---

## 0. The ask (USER, 2026-08-11)

> *"this gameconfig is not a good design for all game specific functionality, is impossible to achieve
> via single config, could use base classes and inheritence instead or some other way"*

The verdict up front, because the rest of the document hangs off it:

**The user is right that one flat config cannot carry game-specific FUNCTIONALITY, and
`enum SchedBody` (game_iface.h:32-46) is the proof:** the game-agnostic framework enumerates one
game's engine methods (`SCHED_DEMO_STAGEMAIN`, `SCHED_SOP_AREALOAD`, `SCHED_CORO_TEXGROUP`, …)
because a flat C vtable has exactly one `schedStageBody(Core*, int which, void*)` slot and something
must give the `int` its meaning. That enum is the shape a flat table forces, and inheritance removes
it: a game's scheduler subclass calls its own engine methods directly, and the framework never
learns their names.

**But inheritance is the fix for exactly ONE of the three kinds of game knowledge the audit found**
(94 confirmed game-specific facts in agnostic code, three kinds). Scalar facts stay data — a virtual
`otRegionBase()` getter is boilerplate that can return a silent wrong 0 exactly as easily as a field
can. Per-instruction diagnostic taps must not be virtual at all (§5). So the answer to "single config
is impossible" is not "one big base class" either — it is **one game object that aggregates three
mechanisms**, each matched to its kind (§2), so a game still hands the framework ONE thing.

**And the single largest defect is not the dispatch mechanism — it is RESIDENCE.**
`runtime/recomp/pc_scheduler.cpp` compiled into `libpsxport` is Tomba!2 code living in the framework
no matter how it is reached. Note the instrument gap this exposes: `psxport_smoke` "proves
agnosticism (zero game symbols)" — but a byte-faithful transcription of five Tomba!2 functions with
hardcoded guest addresses contains **no game symbols**. The smoke link is structurally blind to this
entire class of leak. That is what §7's lint exists to see.

---

## 1. Measured facts (verify these, then build on them)

### 1.1 The audit's three claims, checked against the tree

**"pc_scheduler.cpp is not framework code at all" — CONFIRMED.** Its own header comment names the
sources: "Byte-shape source: generated recomp bodies gen_func_80051F80 / 80051F14 / 80044BD4 /
80052010 / 80051FB4" (pc_scheduler.cpp:19-23). The file carries Tomba's scratchpad anchors
(`kCurTaskPtr 0x1F800138`, `kDoneFlag 0x1F80019B`, `kWaitFrameCtr 0x1F800198`,
`kSpawnParam3/2 0x801FE0DD/DE`, `kTask1State 0x801FE070`, lines 26-31), guest resume PCs
(`0x80051FA4`, `0x80051F40/54/68/70`, `0x80044C10/2C/50/64/A0/A8`, …), BIOS-leaf dispatches
(`0x80080890/60/70/A0`), and stage-entry dispatch keyed to Tomba's task entries
(`0x801062E4` DEMO, `0x80109164` SOP, `0x8010637C` GAME, `0x8010649C` STAGE-0, `0x80044F58`/
`0x8004514C`/`0x800452C0` task-1 bodies — lines 342-345, 367-368, 407-408, 436-437, 500-504,
558-559). `hasNativeHandlerForEntry` (line 341) hardcodes the same three stage PCs that
`GameConfig::stageDemo/stageGame/stageStart` already carry (Tomba2Engine game_config.cpp:76-78) —
the framework file SHADOWS the fields built to replace it.

**"native_boot.cpp's frame loop walks one game's state machine" — CONFIRMED.** Beyond the
`cfg->`-parameterised OT/pool dance (native_boot.cpp:122-128, whose *sequence* is still the
transcription of Tomba's `LAB_80050c6c` loop body), the loop hardcodes: the cutscene-active flag
`0x1F800137` and the Cross/Start autoskip choreography (356-369); the REPL `warp` door-record
mechanism `0x800BF83A/0x800BF839`, area table `0x80108f60`, area byte `0x800bf870`, sm word writes
at `+0x48/4a/4c/4e` (410-440); the seqdbg probe `0x801054B0/0x80104C28/0x800AC424/0x800AC42C`
(466-475); menu detection `(entry & 0xFFFFF000) == 0x80108000` (496); BGM slot scan `0x800be3d8 +
i*0xB0` (520-529); the 4-level sm probe over `TASKBASE+0x48..0x52` and `0x80109450` (531-544);
the CD-stream one-shot `0x801fe0e0/134/138/146` (550-558). Every one is a fact about MAIN.EXE
`SCUS_944.54`, executing in a file every port links.

**"scheduler.h #defines Tomba's task base and shadows GameConfig" — CONFIRMED.**
`#define TASKBASE 0x801fe000u`, `TASKSTRIDE 0x70u`, `CUR_TASK 0x1f800138u` (scheduler.h:6-8) vs
`GameConfig::taskTableBase = 0x801fe000` / `taskSlotStride` / `curTaskPtr` (game_iface.h:99-100,
set in Tomba2Engine game_config.cpp:72). Consumers of the #defines: pc_scheduler.cpp (11 sites),
native_boot.cpp (16 sites). Two names for one fact, one of them a lie on every other game.

**The diagnostic taps — CONFIRMED, and counted.** interp.cpp's flat interpreter loop tests
**17 Tomba PCs across 10 channels on every dispatched instruction**: `0x8007E9C8` (fadeshot, :549),
`0x800939A0` (keyon, :559), `0x80074BF8/0x80074E48` (bgmreq, :567), `0x80026874/0x80052208/
0x800522B0/0x80075834/0x800788CC` (demoflag, :573), `0x800914D0` (septrace, :582),
`0x800909C0/0x80090BD0/0x80091460` (tickdbg, :589), `0x80090210` (seqopen, :598), `0x80090560`
(seqplay, :603), `0x8008E390` (banksel, :609), `0x8007E998` (text, :622) — plus Tomba stream-state
literals inside the otherwise-generic spindbg dump (:530-531). Inline address tests elsewhere:
hle.cpp:663-679 (the recomp-MISS handler tests Tomba's MODE/area overlay window
`0x80108F9C..0x8018A000` and dumps Tomba's stage word `0x801fe00c`, area byte `0x800bf870`, SOP sig
`0x80109450`); mem.cpp:927 (installs a native override at Tomba's libc memset `0x8009A420` from
framework code); sbs.cpp (the biggest holder: `FISH_GATE 0x800BF89C` :175, the divergence-whitelist
windows :500-513 — which cite Tomba GAME-SIDE files "asset.cpp:222", "engine.cpp:1523" from inside
the framework — the `region_name` table :909-914, the probe/watch tables :1016-1051, nav constant
`0x800E7E80` :672).

**Already fixed, this session (do not re-plan it):** the packet-pool/OT window triplication. The
brief's seed list (dualcore.cpp:121, selftest.cpp:305, sbs.cpp:841, ot_attr.cpp) is STALE against
this tree: all four now derive from `GameConfig` via `render_noise.h` (`RenderNoiseMask::from`),
with the honest-zero behaviour this plan generalises — an un-RE'd game gets an EMPTY mask and a
loud warn-once naming the blinded verdict (render_noise.h:74-110), and dualcore REFUSES to run
without `stageGame`/`taskTableBase` rather than comparing boot and calling it clean
(dualcore.cpp `DualCore::run`). `task_slot_layout.h` centralises the slot offsets and marks itself
STOPGAP. Those are the worked examples the rest of this plan copies.

### 1.2 Reachability — measured, and it CORRECTS the brief

The brief asserted "spider1 calls native_boot_run(), so native_boot.cpp's frame loop and PcScheduler
RUN ON SPIDER-MAN with Tomba!2 addresses live." **Measured: false today.**

* spider1 (spider1/game/core/main.cpp:93) does call `native_boot_run()`. But its `bootInit` hook
  (game_hooks.cpp:41-43) dispatches the guest's own `main()` on the substrate and Spider-Man's
  main, like every PSX main, never returns — so control never comes back to `game_main`'s frame
  loop, and `native_step_frame` / `PcScheduler::step` never execute. The port's present/pace lives
  in its native VSync HLE instead (sync_native.cpp:62-66). Confirmation from the port's own
  authors: the frame-loop hooks are deliberate fail-fast aborts — "the hook is only ever called
  from a framework path this port has NOT yet stood up (the Tomba-shaped native frame loop,
  PcScheduler stage bodies…)" (game_hooks.cpp:14-16) — which would fire on the loop's first frame
  if it ever ran.
* spyro (spyro/game/core/main.cpp:101) calls only `dc_boot_init` and PROVED the same never-returns
  property of its guest main statically and at runtime (frame_loop.cpp:11-33), then wrote its own
  loop in game code because — its words — "THIS PORT CANNOT USE THE FRAMEWORK'S FRAME LOOP, and
  must not try to… `native_step_frame`'s per-frame OT/packet-pool block assumes ONE `otBasePtr`
  global rewritten per frame. Spyro keeps per-parity pool pointers INSIDE its two draw envs"
  (frame_loop.cpp:35-46).
* One armed hazard IS live: any spider1/spyro path that reaches `dc_step_frame` (the selftest
  harnesses use it) executes `native_step_frame`'s preamble with zeroed config before any fail-fast
  hook fires — `mem_w32(cfg->otBasePtr=0, 0)` writes guest RAM word 0, and `rc2(c, cfg->clearOtagR=0,…)`
  dispatches guest address 0, which the null-callback path swallows silently (hle.cpp:703-709).
  That is a concrete honest-zero violation: `native_step_frame` consumes six config fields with no
  zero check and no announcement (native_boot.cpp:110-128).

**So the priority is set by adoption, not by live corruption.** The Tomba code in the framework
mostly does not RUN on the other ports; its damage is (a) it POISONED every harness verdict of the
render-noise class until this session's fixes, (b) it FORCED spyro to fork the frame loop into game
code and forces spider1 to fail-fast-fence an entire framework spine, and (c) it makes the next
port's author read 700 lines of another game's choreography to learn which half is reusable. The
redesign's payoff is a frame loop and scheduler the NEXT port can subclass instead of fork.

### 1.3 Dependency direction

The boundary follows from ownership: game loops, title interpolation, saves, and title-specific
configuration change with the game and therefore live in the game repository. PSX devices, host
platform services, and reusable execution mechanics change with the framework and live in psxport.
The dependency points one way: game → framework. Spyro's measured `frame_loop.cpp` boundary confirms
that this split works in an existing consumer. **The game is the application; psxport is a library
with a few well-typed extension points**, not a framework that calls back into one game's engine.

---

## 2. The taxonomy — the rule, applicable without asking

Given a piece of game-specific knowledge currently in (or headed for) framework code, ask three
questions IN ORDER. The first "yes" decides the mechanism.

**Q1 — Is it a VALUE that parameterises a framework loop whose SHAPE is the same for every game?**
(an address, a stride, a count, a path, an env key.)
→ **`GameConfig` scalar.** Test: rewrite the consuming framework code in your head with the number
replaced by `cfg->x` — if the code now reads without naming any game, it was data. The consumer
MUST have honest-zero behaviour: refuse or announce-once, naming what it cannot see
(`render_noise.h:74-110` is the canon; `dc_boot_init`'s REFUSING TO RUN is the harness form).
A field must name its framework consumer in its comment; **a field whose only consumer is game
code is not a config field, it is a game constant that leaked into the seam** (§4.3 deletes
several of these).

**Q2 — Is it CONTROL FLOW — an ordering, a state machine, a dispatch decision, a transcription?**
→ **Game code, in the game repo, behind a framework-declared abstract class.** The seed test,
sharpened: *delete the game's facts from the function — does the remaining framework function
still have a job?* `native_step_frame` minus Tomba's OT dance, autoskip, warp, and probes is an
empty `for` loop with a watchdog: it was never framework code. Compare: `RenderNoiseMask::from`
minus Tomba's numbers is still a complete algorithm over any game's numbers — that one is
framework code with Q1 parameters. Positive marker for Q2: the code cites a `FUN_xxxxxxxx` or a
decomp line range as its byte-shape source. A transcription's provenance is a game executable;
its home is that game's repo, full stop.

**Q3 — Is it a DIAGNOSTIC KEYED to game addresses?** (a PC tap, a region-name table, a watch list,
a miss-context dump.)
→ **Framework owns the MECHANISM — registry, hot-path membership test, formatting; the game
supplies ROWS at init.** An empty registry is honest by construction (nothing registered, nothing
fires) — but any report whose VERDICT depends on coverage must print its denominator: "0 game
taps registered", "no game atlas — addresses will be raw", per the diagnostics-negative rule.

Boundary cases the audit hit, decided:

* **A guest address inside a console-fixed range is still game data if the value picks out one
  game's object.** Scratchpad is the sharp case: `0x1F800000-0x1F8003FF` as a REGION is the
  console; `0x1F800138` meaning "current-task pointer" is Tomba's allocation of it
  (pc_scheduler.cpp:26). The lint (§7) therefore allows only the region's BOUNDS, not interior
  offsets.
* **Slot/struct OFFSETS are game data exactly like bases** (`task_slot_layout.h`'s own STOPGAP
  banner says so). Q1 if a generic consumer reads them; they die with their consumer if Q2 moves it.
* **A per-game POLICY switch is Q1 only while its answer is immutable** (`paceQuota`): the
  framework's code path is generic; the game states which leg. `hle.vsyncTrap` is no longer a policy
  switch: it is the adapter's mandatory measured-address fact and every product receives the same
  fatal handler. A title that
  changes ownership by frame is Q2. `preserveVramBackdrop` crossed that boundary in issue 0022:
  Spyro needs guest VRAM for upload-only boot screens and rejects it under complete native frames,
  so the live owner is now the required pure virtual
  `GameRuntime::guestVramIsPicture(const Game&)`. The renderer's checked query refuses a missing
  runtime, and its per-`Game` composite latch rebuilds on either ownership transition instead of
  reusing pixels built under the previous answer.

---

## 3. The interfaces

One header, `runtime/recomp/game_runtime.h`. A game implements ONE class and installs ONE pointer.
All names concrete; signatures are the proposal, not a sketch.

```cpp
// game_runtime.h — the framework↔game seam, second generation. The framework #includes nothing
// from a game; a game implements GameRuntime in its own repo and installs it before any Game is
// constructed. GameConfig/GameHooks are not part of this interface.
#pragma once
#include <memory>

class Core; class Game;
namespace lucent { class Line; }

// ---- Kind 2: behaviour seams. Base-class DEFAULTS are where the honest zero lives: every
// optional virtual's default either refuses loudly (names itself, aborts) or announces its
// blindness once. A game overrides with real behaviour or leaves the honest default.
// Overriding-to-silence is the one banned move (it is spider1's unstood_up() convention,
// promoted from a per-port discipline to the type's default).

// One frame of deterministic guest work, including the game's own auto-drive and probes. The frame
// LOOP is host-owned; the title supplies one finite measured engine step and the framework supplies
// loop FURNITURE (below) plus the contract "step and present exactly once".
class FrameDriver {
public:
  virtual ~FrameDriver() = default;
  virtual void stepFrame(Core& c, uint32_t f) = 0; // the complete finite title frame
};

// The game's cooperative-task scheduler, if it has one the framework must be able to tick.
class TaskScheduler {
public:
  virtual ~TaskScheduler() = default;
  virtual void step() = 0;                    // one pass over the game's task slots
  virtual void yield(Core& c) = 0;            // the ChangeThread funnel (PlatformHle routes here)
  virtual void tickSleepCountdown() {}        // optional per-frame sweep
};

// What the harnesses (dualcore/SBS/selftests) may ask about "where is the game". Kills the
// stageStart/stageDemo/stageGame fields and task_slot_layout.h: the PREDICATE was always the
// game's, not just its constants (mem_r32(taskTable+0xC)==stageGame is a Tomba shape that means
// nothing on spyro).
class GameplayProbe {
public:
  virtual ~GameplayProbe() = default;
  virtual bool gameplayReached(Core* c) = 0;              // dualcore/SBS REACH_GAME
  virtual uint64_t stateSignature(Core* c) = 0;           // change-detector for state probes
  virtual void describeState(Core* c, lucent::Line& ln) = 0;  // one line for logs/harness reports
};

// Kind-3 STATIC rows: names and context for guest addresses. Pure data provider — no dispatch.
class GuestAtlas {
public:
  virtual ~GuestAtlas() = default;
  virtual const char* regionName(uint32_t addr) const { return nullptr; }   // sbs region tables
  virtual void describeMiss(Core* c, uint32_t addr) const {}                // hle.cpp miss context
  struct Watch { const char* name; uint32_t lo, hi; };
  virtual const Watch* watchTable(size_t* count) const { *count = 0; return nullptr; }
};

// THE game object. Stateless apart from config: per-Core state lives in the objects the create*
// factories return (SBS constructs two Games from one runtime — this is why GameHooks took Core*
// everywhere, and the factories are the typed version of that discipline).
class GameRuntime {
public:
  virtual ~GameRuntime() = default;

  // Lifecycle (pure: every port already implements these as hooks today).
  virtual void* createContext(Core& c) = 0;
  virtual void  destroyContext(void* ctx) = 0;
  virtual void  registerOverrides(Game& g) = 0;   // overrides + PC-tap rows + anything registered
  virtual void  bootInit(Core& c) = 0;

  // Kind 2 factories. A null FrameDriver is permitted only for smoke/tools that never enter product
  // boot. Product preflight refuses it before bootInit can dispatch guest main; there is no
  // guest-owned or game-side bypass around the framework shell.
  //   no TaskScheduler -> PlatformHle's ChangeThread routing refuses registration of a yield
  //                       funnel; nothing silently no-ops.
  virtual std::unique_ptr<FrameDriver>   createFrameDriver(Game& g)       { return nullptr; }
  virtual std::unique_ptr<TaskScheduler> createTaskScheduler(Game& g)     { return nullptr; }

  // Kind 2/3 singletons (stateless, may be static members of the game's runtime).
  virtual GameplayProbe*    probe()  { return nullptr; }   // null => harnesses refuse, loudly
  virtual const GuestAtlas* atlas()  { return nullptr; }   // null => raw addresses + one notice

  // Present-path / audio / REPL / selftest members migrate from GameHooks 1:1 as virtuals with
  // honest defaults (fadeState -> mode 0; audioMixFrame -> silence + announce-once if a music
  // channel is expected; replCommand -> false; selftestGame -> 2 "unknown"). The fps60WorldPass /
  // fps60BbSwapPrev pair migrates marked TRANSITIONAL with its existing death condition
  // (game_iface.h:381-388) — this plan does not redesign the render seam beyond re-homing it;
  // the fps60 submit-model plan owns that.
};

// Install: once, before any Game exists. Both SBS cores share it.
void         psxport_install_game(GameRuntime& rt);
GameRuntime* psxport_game_runtime();
```

**Ownership and reach.** The game's `main()` owns the `GameRuntime` instance (a static in
`game/core/main.cpp`), process lifetime, never deleted by the framework. `Game`'s constructor
snapshots `rt = psxport_game_runtime()` and materialises per-Game state:
`taskScheduler = rt->createTaskScheduler(*this)`, `frameDriver = rt->createFrameDriver(*this)`. A
`Core* c` reaches everything through `c->runtime`, `c->game->taskScheduler`, and
`c->game->frameDriver`. During migration only,
the legacy adapter also fills `c->cfg`/`c->hooks`. `Game::pcSched`
(game.h:70) — a Tomba class held by value in a framework object — is deleted; its replacement is
the `sched` unique_ptr.

**The loop entry.** The implemented `FrameLoopShell` owns no title services. It is the mandatory
preflight plus the one finite stepping route used by `dc_step_frame` and the standalone loop:

```cpp
class FrameLoopShell {
public:
  FrameDriver& requireDriver(Game& game) const;
  void step(Core& core, uint32_t frame) const;
};
```

The shell does not bracket pad/audio/present around the driver. Each title's driver owns its measured
service order and one presentation commit (or a measured unpresented fence). Boot diagnostics, FMV,
watchdog, REPL pause/step, frame-budget resolution, `crt0_setup`, and `render_path_install` remain
framework scaffolding in their existing cohesive owners; they are not copied into title drivers.

**Transitional adapter, so migration is per-member.** Step 4 of §6 introduces
`LegacyGameRuntimeAdapter : GameRuntime` (framework-side, temporary) built over an installed
`(GameConfig*, GameHooks*)` pair; the legacy `psxport_install_game(cfg, hooks)` constructs one.
Every subsequent step moves one member from delegation to a real override in one game, and the
adapter dies when all three games install a `GameRuntime` directly.

The exact consumer migration is intentionally incremental:

1. Define `<Title>Runtime final : public LegacyGameRuntimeAdapter` in the game's `game/core/` and
   pass its existing `GameConfig` and `GameHooks` to the base constructor.
2. Replace `psxport_install_game(&config, &hooks)` with a process-lifetime runtime object and
   `psxport_install_game(runtime)`.
3. Override `createContext`/`destroyContext`, `registerOverrides`, `bootInit`, and the driver/scheduler
   factories as each cohesive owner moves. An unoverridden member still delegates through the adapter.
4. Extract only generic immutable fact groups that a living framework algorithm iterates; move every
   other config field beside its derived behavior. Once `c->cfg`/`c->hooks` have no consumers, derive
   directly from `GameRuntime` and delete the adapter plus the old pair.

**First typed ownership slice — `GuestProgramImage`, IMPLEMENTED.** A direct `GameRuntime` can now
provide one immutable `GuestProgramImage` value, containing
only the executable facts jointly consumed by `crt0_setup`/`crt0_plan`, `overlay_router`, and the
resident-code backtrace heuristic: BSS range, stack/heap declarations, GP/libc/main/crt0 entries, and
resident text range. Those three generic algorithms consume that one typed value; it is not one
virtual getter per integer and does not absorb disc, CD, pad, render-memory, scheduler, or game policy.
The framework side no longer reads the corresponding `GameConfig` fields. They remain solely as input
to `LegacyGameRuntimeAdapter` until each consumer moves its measured constants into its derived runtime;
deleting them before those consumer changes would knowingly break every pinned port. The consumer
migrations and final field deletion are one follow-up milestone, not a second framework authority.
The following
fact slices use the same consumer-owned rule (`DiscIdentity`, `RenderMemoryLayout`, platform-library
entry tables). The platform-library slice has landed as `PlatformHlePlan`: standard libgte projection
leaves are typed address facts whose handlers remain framework-owned, while genuinely title-specific
sync behavior uses explicit `{addr, fn}` rows. The remaining slices each need a named algorithm and
deletion set. Any fact used only by derived behavior
moves directly beside that behavior instead of entering one of these values.

Until the remaining slices land, a real consumer derives `LegacyGameRuntimeAdapter`; deriving
`GameRuntime` directly is valid only for a consumer whose framework paths require no remaining legacy
facts. Crt0/routing/backtrace no longer impose that limitation; disc, render-memory, and other live
config reads still do. Platform HLE itself no longer imposes that limitation on a
direct runtime that supplies a complete `PlatformHlePlan`. This limitation is explicit so
`core.cfg == nullptr` is never
mistaken for a completed consumer migration.

**What stays in GameConfig, and why that is not a cop-out.** Everything that passes Q1 with a
living framework consumer: `discEnvVar`,
`bootFmv`, `cardEnvVar/cardDefaultPath`, `windowTitle`, `paceQuota`, the
`hle` platform-sync group (adapter input for PlatformHle::initBuiltins; direct runtimes own the
equivalent `PlatformHlePlan`), the pad group, the CD chokepoint group
(cd_override.cpp), `overlaySlots`, `packetPool*/otRegion*/poolPtr*` (render_noise.h — the harness
mask is a genuinely generic consumer even after the frame loop leaves), `taskTableBase/
taskSlotStride/taskCount` (SBS snapshot/restore of the task table region). These are facts a
generic algorithm iterates; making them virtuals adds a vtable hop and removes nothing — a virtual
returning 0 is precisely as dishonest as a field holding 0, so the honesty work (announce/refuse
in the consumer) is identical either way and already the codified rule. The config ALSO sheds
fields (§4.3), and every survivor's comment must name its framework consumer — that naming is what
keeps "it's just a config field" from becoming the next leak.

`GameConfig::preserveVramBackdrop` is now adapter input only. The renderer asks the derived runtime
per frame; the field remains solely to preserve unmigrated consumers until their runtimes override
the typed policy and the adapter projection can be deleted.

---

## 4. What dies

### 4.1 Code that changes repos

| today (framework) | goes to | framework keeps |
|---|---|---|
| `pc_scheduler.{h,cpp}` — the five transcribed primitives + all stanzas + entry-PC dispatch | `Tomba2Engine/game/core/pc_scheduler.{h,cpp}`, `class TombaScheduler : TaskScheduler` | `task_scheduler.h` (the interface), `coro.{h,cpp}` (generic fibers) |
| `scheduler.cpp` substrate stanzas + `native_task_spawn` (Tomba slot layout: state@+0, sp@+8, entry@+0xC, gp@+0x10) | same file move — they are the psx_fallback half of the SAME scheduler | `scheduler_yield`'s seam becomes `TaskScheduler::yield` reached via PlatformHle |
| `scheduler.h` `#define TASKBASE/TASKSTRIDE/CUR_TASK` | deleted (step 1 re-points to cfg fields; the whole header moves with the scheduler in step 5) | — |
| `native_boot.cpp` `native_step_frame` body, autoskip/newgame/skip/warp, seqdbg/state/bgmtick/sm probes, dual-view second pass | `Tomba2Engine/game/core/frame_driver.cpp`, `class TombaFrameDriver : FrameDriver` | `native_boot_run` scaffold, `crt0_setup`, `FrameLoopShell`, `dc_boot_init`/`dc_step_frame` (now delegating to `driver`) |
| interp.cpp's 17 Tomba PC taps (10 channels) + spindbg's Tomba stream dump | `Tomba2Engine/game/core/diag_taps.cpp` — rows registered into `PcTaps` (§5) | `PcTaps` registry, spindbg/pctrap/derail (env-keyed, no game address) |
| hle.cpp:663-679 miss-context dump (overlay window test + Tomba state dump) | `GuestAtlas::describeMiss` on Tomba's atlas | the generic miss report, backtrace, RAM dump |
| mem.cpp:891-927 guest-memset override install at `0x8009A420` | Tomba's `registerOverrides` (game/core/register_overrides.cpp) | the `ov_guestMemset` mechanism if any other port wants it — as a helper taking the address |
| sbs.cpp region_name table (:909-914), probe/watch tables (:1016-1051), divergence whitelists (:500-513), `FISH_GATE`/nav constants (:175, :631, :672) | Tomba's `GuestAtlas` rows + `GameplayProbe` (the SBS nav machine asks the probe, not the sm words) | the SBS engine: lockstep, snapshot, lwmap, wwatch, replay |
| selftest.cpp `startgame`/`narration`/`oracle`/`oraclediff` (Tomba gameplay tests, overlay sig `0x801138A4`, scene bytes) | Tomba via the existing `selftestGame` hook (game_iface.h:394) | `mdecpump`, `spuirq` (console-generic), the harness plumbing |

### 4.2 Seam members that die outright

* **`enum SchedBody` + `schedStageBody` + `schedFreshEntry` + `schedRng` + `hasNativeHandlerForEntry`**
  (game_iface.h:32-46, 328-336, 371-374): once `TombaScheduler` lives beside `Engine` in
  Tomba2Engine, it calls `eng(c).demo.frame()` etc. directly with full types. The enum, the
  multiplex hook, and the RNG hook (which exists only because the framework scheduler needed
  Tomba's `FUN_8009A450`) have no caller left.
* **`frameUpdate`, `drawOTag`, `musicCoordTick`, `renderBbFrameReset`, `devWarpAreaLoad/Enter`,
  `devAreaCount/Name`, `devWarpAllowed`, `bootInit`-as-hook:** their ONLY callers are
  native_boot.cpp/repl.cpp paths that move into `TombaFrameDriver` / Tomba REPL commands (via the
  surviving `replCommand` delegation). **Most of GameHooks exists only because the frame loop
  lives on the wrong side of the seam**; move the loop home and the hooks evaporate rather than
  needing redesign.
* **`task_slot_layout.h`** — dies when `GameplayProbe` lands (its own banner already schedules this).

### 4.3 GameConfig fields that die (their only consumer moved)

`clearOtagR`, `putDrawEnv`, `drawSync`, `otBasePtr`, `dwellCounter`, `irqEventClasses[3]`,
`dualviewRenderOrch`, `dualviewSubmit`, `stageStart`, `stageDemo`, `stageGame` — each consumed
solely by `native_step_frame`/harness-nav code that becomes Tomba code; the values return home as
named constants next to the code that means them. Honest note: several of these were ADDED this
year precisely to de-Tomba the framework. That was the right fix for the wrong kind — it treated a
behaviour's parameters as standalone facts; once the behaviour moves home, its parameters go with
it. `poolPtrCur/poolPtrLast/packetPool*/otRegion*` SURVIVE (render_noise.h consumer). At the same
step, all three games' `game_config.cpp` convert to designated initializers — the positional-init
hazard game_iface.h warns about four separate times (:117, :243, :251, :286-288) ends here.

---

## 5. Cost, honestly

The line falls between per-frame and per-instruction, and the design differs on each side of it.

* **Per frame / per task-slot / per dispatch (cold side).** `FrameDriver::stepFrame`,
  `TaskScheduler::step`, probe calls: single-digit virtual calls per 16.6 ms frame — unmeasurable.
  `rec_dispatch` already performs a registry lookup per dispatched call; routing yield through
  `TaskScheduler::yield` adds one indirect call to an operation that parks a fiber. Free, and no
  measurement is needed to say so.
* **Per guest STORE (hot).** Precedent already measured in this tree: a by-name channel lookup on
  the store path put 6/6 profile samples in `lucent::detail::channel_enabled` on the Spyro port
  (ot_attr.cpp:52-58) and was fixed with an inline pre-resolved gate. Rule: nothing virtual, no
  name hashes; inline flag tests + derived windows (`RenderNoiseMask` shape) only.
* **Per INSTRUCTION (the hot side — interp.cpp's flat loop).** A virtual call here would be
  slower than the literals it replaces; that design is rejected. But note what the literals
  actually cost TODAY: the tap chain executes ~17 compare/branches per dispatched instruction
  **unconditionally, channels on or off** (interp.cpp:549-627). The replacement is a registry with
  an armed-set bitmap:

  ```
  PcTaps::add(pc, channel, fn) at init; rearm() rebuilds the bitmap from lucent's enabled set
  (REPL `debug X` re-arms live). Bitmap: one bit per instruction word of the 2 MB RAM =
  512 K bits = 64 KB. Hot path:  word = (pc & 0x1FFFFF) >> 2;
                                 if (bitmap[word >> 3] & (1 << (word & 7))) fire(c, pc);
  ```

  One load + AND + predictable branch, against ~17 always-on compares. With every channel off the
  bitmap is all-zero and the branch never mispredicts. Expected: neutral-to-faster. **Expected is
  not measured:** the gate for step 2 is wall time of an interp-heavy run (`PSXPORT_SELFTEST=oracle`
  on Tomba, which drives the flat interpreter through full scenes) before/after, three runs each,
  accept within noise. If the 64 KB map measurably pressures cache, the fallback is a two-level
  test (armed-count==0 fast-out + sorted-array bound check) — decided by the measurement, not
  argued.
* **Interp-side context loads.** `fire()` is the cold path; its body may do anything (the current
  taps' logging dominates their own cost once armed, unchanged).

---

## 6. Migration order — smallest first, each step landable and gated

Every step leaves `psxport_smoke` linking, `ctest` green, and the three boot gates green
(build explicitly, then each repo's own gate tool — e.g. `python3 tools/gate.py boot`; NEVER
`./run.sh`, which is the USER's play launcher and re-syncs the submodule under you); steps
touching Tomba behaviour additionally hold the SBS byte-compare. No step touches two taxonomy
kinds at once.

0. **The lint (§7), in ratchet mode** — lands first, parallel agent, so every later step SHRINKS a
   machine-checked baseline instead of a prose list. RED: seed a fake `0x80123456` into a
   framework file → gate fails naming it.
1. **Kill scheduler.h's #defines** (pure Q1; the fields exist and Tomba already sets them). All
   `TASKBASE/TASKSTRIDE/CUR_TASK` uses in pc_scheduler.cpp/native_boot.cpp read
   `cfg->taskTableBase/taskSlotStride/curTaskPtr`, with the standard refuse-on-zero at each
   consumer entry. RED test (hermetic, tests/): install a stub config with
   `taskTableBase=0x80123400`, arm slot 0 state=1 countdown=2 at that base, call
   `tickSleepCountdown()`, assert the decrement landed at `0x80123402` and NOT at `0x801fe002`.
   Fails today by construction.
2. **`PcTaps` registry; Tomba's 17 taps move to `Tomba2Engine/game/core/diag_taps.cpp`.** RED
   (prove-it-fires, per the instruments rule): a hermetic test registers a tap on a synthetic PC,
   runs `interp_flat` over a two-instruction snippet, asserts the tap fired — plus the arm/disarm
   transition. Boot log on spyro/spider1 gains "PcTaps: 0 game taps registered". Perf gate: §5's
   oracle-selftest wall-time measurement.
3. **`GuestAtlas`:** hle.cpp miss dump, sbs region/watch tables, dualcore's report labels. RED:
   hermetic — force a recomp-MISS at an address inside Tomba's overlay window under a stub
   (non-Tomba) config; today the report prints Tomba's `miss-state` block reading meaningless
   words; after, it prints "no game atlas — raw address only". (Also removes sbs.cpp's citations
   of Tomba game-source files from framework comments.)
4. **`GameRuntime` + bounded `LegacyGameRuntimeAdapter` — IMPLEMENTED, first slice.** Context lifecycle,
   override registration and boot initialization dispatch virtually; `Game` owns the optional
   per-Game driver/scheduler products. The smoke derives `GameRuntime` directly and proves both
   legacy views are null. `test_game_runtime` also gates the adapter's old-pair delegation. Deliberate
   delta from the 2026-08-11 sketch: there is no `virtual config()` and no base-class config bag.
   Immutable facts remain reachable only through the named legacy adapter until later steps extract
   the narrow fact groups their generic consumers genuinely iterate.
4a. **`GuestProgramImage` — IMPLEMENTED, first typed fact slice.** Crt0 planning/audit/application,
   resident MAIN dispatch, and the diagnostic backtrace range read the immutable runtime-owned value.
   `Core` snapshots it beside the runtime; the adapter projects legacy fields once in its constructor.
   `test_guest_program_image_ownership` prevents those algorithms and `GameHooks` from reclaiming it.
   Remaining follow-up: migrate each consumer's constants, then delete the adapter-only legacy fields.
4b. **`PlatformHlePlan` — IMPLEMENTED, direct-runtime platform-library fact slice.** The plan owns
   the measured SCEI-library windows, typed SetGeomOffset/SetGeomScreen, stock CdRead/CdReadSync,
   and mandatory VSync addresses,
   and bounded `{addr, fn}` rows for other title-specific sync behavior. `PlatformHle::initBuiltins()` maps both direct
   and legacy projection addresses through the same private framework handlers and the same
   half-open-window guard; VSync always maps to the private all-mode abort and refuses replacement.
   `test_platform_hle_direct_runtime` drives the projection lookup/handler seam and
   `test_vsync_ownership` covers both runtime shapes, all mode classes, and opposite controls.
   Remaining follow-up: migrate legacy consumers,
   then delete the adapter-only `GameConfig::hle` fields; do not expose or duplicate standard handlers
   in game repositories.
5. **SchedBody death.** `pc_scheduler.{h,cpp}` + scheduler.cpp's stanzas move to Tomba2Engine as
   `TombaScheduler`; enum + 3 hooks + `Game::pcSched` deleted; PlatformHle's ChangeThread routes
   to `sched->yield`. Gates: Tomba SBS byte-compare (the stanzas' cadence — the Slip #1-#4
   step-spreads — must survive the move bit-for-bit), Tomba boot gate, smoke. RED: before the
   move, a framework grep-level check cannot be the test (grep counts text) — the RED is the smoke
   target extended with `-Wl,--no-undefined` proving `libpsxport` resolves with no `SchedBody`
   symbol, failing while the enum's users exist.
6. **FrameDriver split — FRAMEWORK HALF IMPLEMENTED.** `FrameLoopShell` requires the factory product,
   `dc_step_frame` and the standalone loop delegate exactly once, and product entry refuses before a
   non-returning guest main. The framework title-frame body has been deleted; a static ownership test
   rejects its defining operations in `native_boot.cpp`. Consumer work remains: each title supplies its
   finite driver and proves its measured body + diagnostics locally. Gates: each title's real boot/frame/
   present gate plus relevant harnesses; a guest-owned loop or VSync presenter is not a supported state.
7. **`GameplayProbe`.** dualcore/SBS/selftest navigation asks the probe; Tomba's gameplay
   selftests move behind `selftestGame`; `task_slot_layout.h` + `stage*` fields deleted. RED:
   dualcore under a stub config must still REFUSE (it does today — keep that test) but now with
   "no GameplayProbe" wording; SBS nav on Tomba reproduces the same frame numbers for
   REACH_GAME as before the move (recorded in the step's evidence).
8. **GameConfig diet + designated initializers** (§4.3), `LegacyGameRuntimeAdapter` deleted once all three
   games construct a `GameRuntime`. Gate: lint baseline strictly smaller than at step 0; the
   remaining entries each carry a `psx-console:` marker or a burn-down issue reference.

---

## 7. The enforcement — a lint, specified exactly

> **LANDED 2026-08-12** as `tools/lint/game_literals.py` + `tools/lint/game_literals_baseline.txt`,
> wired as the ctest entries `game_literals` and `game_literals_selftest` (tests/CMakeLists.txt) and
> shipped as `scripts/hooks/pre-commit`. **The real starting line, measured, not the indicative grep
> below: 399 flagged guest literals in LIVE code across 24 files** (202 files scanned, 4700 32-bit
> address candidates classified), **plus 458 more in comments and strings that are NOT gated**.
> Two spec deltas, both measured (details in the tool's header and §7.1 below): the indicative list
> below overstates several files badly because it counts comments — `pc_scheduler.h` is 17 in a raw
> grep and **0** live — and the width test now normalises leading zeros, because `0x0801FE070` is a
> legal literal for a guest address that the raw 5-8-digit rule read as "9 digits, not an address".

### 7.1 As-built deltas from the spec above

* **Live vs comment is reported, not just discarded.** §7 mechanic 1's decision to gate live code
  only is kept exactly; the comment/string occurrences are counted separately and listed at the
  foot of the baseline, so nobody sizes a §6 step off a number that mixes the two classes.
* **The baseline also fails on SHRINK-without-regeneration**, as §7 mechanic 3 requires, and each
  entry carries indicative `# L…` line hints after the gated triple so a later step can retire it
  deliberately. The hints are not part of the key and never fail the gate.
* **The 5-8-digit width rule turned out to change no VERDICT** (every flag range needs ≥7
  significant digits, and no >8-digit value lands in one) — it governs only the reported
  denominator. So the selftest asserts the denominator, not just the verdict; sabotaging the width
  rule was survivable until it did.
* **ALLOW is evaluated before FLAG**, which makes widening a FLAG range downward into console
  territory inert. Recorded so it is not later mistaken for a hole.
* **`tools/recomp/test_*.py` are excluded** for §7's own reason for excluding `tests/` (their guest
  addresses are fixtures), and the count of files excluded that way is printed with every verdict
  rather than being a silent exemption.
* **GITIGNORED GENERATED ARTIFACTS under the source roots are excluded.** A denominator that depends
  on which local tools ran makes every negative unreproducible, and a flag inside generated output
  would name a line no human can edit. The ignored set comes from `git ls-files --others --ignored`;
  when git is unavailable the verdict says so explicitly rather than reporting a number it cannot
  justify.

**Name:** `tools/lint/game_literals.py` (python3, zero deps). **Scope:** `runtime/**` and
`tools/recomp/**` of psxport — framework code only. Not `generated/`, not `vendor/`, not `tests/`
(tests intentionally use synthetic guest addresses), not game repos.

**Mechanics.**

1. Per file: strip `//` and `/* */` comments and string/char literals (a comment naming a game
   address is cleanup debt, not an executable lie; flagging comments would triple the baseline
   with cosmetic entries — this is a scoping decision, revisit if a string-parsed address ever
   bites). Then tokenize hex literals `0x[0-9A-Fa-f]{5,8}` (5+ digits — nothing shorter can be a
   guest RAM address; strides/sizes below 0x10000 are structurally unflaggable and stay out).
2. Classify each value `v` (suffixes `u/U/l/L` stripped):
   * **ALLOW — the console:**
     - exact set: `0x80000000, 0xA0000000, 0x1FFFFFFF, 0x001FFFFF, 0x001FFFFC, 0x00200000,
       0x80200000, 0xFFFFFFFF, 0xFF000000, 0xDEAD0000` (segment bases/masks, RAM size/top,
       the two sentinels — sentinels are enumerated, not patterned);
     - `0x1F801000-0x1F802FFF` (hardware I/O registers);
     - exactly `0x1F800000` and `0x1F800400` (scratchpad REGION BOUNDS — interior offsets are a
       game's layout and are flagged);
     - `0x1FC00000-0x1FC7FFFF` and `0xBFC00000-0xBFC7FFFF` (BIOS ROM);
     - `0x00000000-0x0000FFFF` and `0x80000000-0x8000FFFF` (kernel/BIOS RAM region: exception
       vectors, A0/B0/C0 tables, the HLE work area — hle.cpp:131's constants stay legal).
   * **FLAG — the game:** `0x80010000-0x801FFFFF`, `0xA0010000-0xA01FFFFF`, and
     `0x1F800001-0x1F8003FF` (scratchpad interior). Unsegmented physical addresses
     (`0x00010000-0x001FFFFF`) are NOT flagged — too many size/stride false positives, and the
     audit found no leak spelled that way.
3. **Exceptions, two mechanisms and only two:**
   * inline, same line: `// psx-console: <why>` — for a genuine console constant the classifier
     cannot know (must state the console fact, not "needed here");
   * the ratchet baseline `tools/lint/game_literals_baseline.txt`: lines of
     `<path>:<value>:<count>` (per-file per-value counts, NOT line numbers — survives unrelated
     edits). The gate fails if any file's flagged count for a value EXCEEDS its baseline count,
     and fails if the baseline lists an entry whose count is now lower/zero without the baseline
     being regenerated — shrink is mandatory, growth is impossible without editing a reviewed
     file. `--write-baseline` regenerates; it refuses to produce a baseline larger than the
     existing one unless given `--grow` (which the hook and ctest never pass).
4. **Where it runs:** registered as a ctest (`add_test(NAME game_literals COMMAND python3 …)`) so
   `ctest --test-dir build` is the gate, and the same command in the repo pre-commit hook. Agents
   never bypass a failing hook (`--no-verify` is banned); the failure output names file, line,
   value, and the two legal remedies.
5. **The negative is designed first** (per the diagnostics rule): the tool exits 2 with "scanned
   NOTHING" if the scan root is missing or matches zero files, and every verdict line carries the
   denominator: `scanned 214 files / 1892 hex literals / 61 flagged / baseline covers 61`.
6. **Self-test:** `--selftest` runs the classifier over embedded positive and negative samples
   (one of each class above, including a scratchpad-interior positive and a `psx-console:`-marked
   negative) and must flag/pass respectively; registered as its own ctest so a broken instrument
   cannot certify the boundary.

**Initial burn-down, indicative** (raw grep of `0x80[01]xxxxx` + scratchpad-interior forms,
comments and mask values INCLUDED, so these overstate what the lint will flag — the lint's first
run produces the real numbers): sbs.cpp 256, pc_scheduler.cpp 82, selftest.cpp 73, native_boot.cpp
66, interp.cpp 55, cd_override.cpp 45, repl.cpp 41, mem.cpp 23, hle.cpp 21, scheduler.cpp 18,
pc_scheduler.h 17, dbg_server.cpp 16. The §6 steps are ordered to empty the biggest holders; the
baseline makes the ordering enforceable instead of aspirational.

---

## Assumed vs measured

* MEASURED: every §1 claim (file:line cited); spider1/spyro boot spines and the never-returns
  property of both guest mains (spyro's proof is static+runtime in frame_loop.cpp; spider1's is
  the fail-fast hook convention plus its VSync-owned present — the frame loop aborting on frame 1
  via `unstood_up` has not been separately demonstrated in a log this session).
* MEASURED: the per-store channel-lookup cost precedent (ot_attr.cpp:52-58, 6/6 samples).
* ASSUMED until step 2's gate runs: the tap bitmap is neutral-or-faster than the 17-compare chain.
  The fallback is specified, the decision is the measurement's.
* ASSUMED: the SBS byte-compare fully constrains the scheduler move (step 5). It is the strongest
  gate this workspace has for that code; if a cadence residual appears, the Slip #1-#4 step-spread
  counters are the first suspects and the move must not land with a new slip on the books.
