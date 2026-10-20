# Title host

The framework's multi-title product host: one process, one window and one presentation device, an
in-window title selector, the chosen title, and back to the selector. A port with several titles
(Spyro 1/2/3, Crash 1/2/3, Spider-Man 1/2, Tomba 1/2) supplies a catalog and a factory for it; the
framework's one `main` runs it through `psx::host::ProductHost`. A title writes no `main`, command line or
selector code of its own. Code lives in `runtime/psx/host/` (private to the framework) and, for the
catalog contract a title implements (`title_catalog.h`, `single_title_catalog`, `title_selection`,
`title_availability`, `logo_image.h`), in the public `runtime/psx/catalog/`; namespace `psx::host`.

## What it owns

| File | Owner |
| --- | --- |
| `title_catalog.h` | `TitleIdentity` (one title: serial plus file identity, and its disc facts) and `TitleCatalog` (what a product supplies, including its provisioning root) |
| `single_title_catalog.{h,cpp}` | `SingleTitleCatalog`: the catalog of a one-title product, built from the title's identity, its provisioning root and its runtime |
| `title_selection.{h,cpp}` | `selectExecutable`, `selectExecutableFile`: does these bytes match this catalog entry (size, PS-X EXE header words, SHA-256); the file variant returns the bytes it authenticated |
| `title_availability.{h,cpp}` | `TitleAvailabilityProbe`: which catalog titles are provisioned and authenticate at `<root>/<slug>/<serial>` right now |
| `executable_provisioner.{h,cpp}` | `provisionExecutable`: a missing `<root>/<slug>/<serial>` extracted from the title's own disc, authenticated before it is written |
| `title_disc.{h,cpp}` | `TitleDisc`: a title's disc for one read, keyed (`discEnvVar`, `PSXPORT_DISC`, the saved choice) or one exact image |
| `saved_disc.{h,cpp}` | `SavedDisc`: the disc image the player chose at first run, `<user data>/<slug>/disc-path`, and `bind` that points a `DiscState` at the title's key and then at it |
| `first_run_setup.{h,cpp}` | `FirstRunSetup`: the shared setup-ui screen that asks for the disc (`judge` is its validator), saved through `SavedDisc` |
| `title_session.{h,cpp}` | `TitleSession`: one boot-to-exit run of one title as a steppable owner; destruction is the whole teardown |
| `session_run.h` (`runtime/psx/game/`) | `SessionRun`, held per `Game` as `Game::run`: a title's request to end its run |
| `run_budget.{h,cpp}` | `RunBudget`: the process's run cap in presentation-fence steps, decided once from `PSXPORT_NATIVE_FRAMES` |
| `panel_sessions.{h,cpp}` | `PanelSessions`: one live session per panel (the available titles `PSXPORT_PICKER_TITLES` asks for) and the rules about which of them runs |
| `picker_session.{h,cpp}` | `PickerSession`: the selector frame loop; one guest advances per frame |
| `picker_runtime.{h,cpp}` | `PickerRuntime`: the selector's own `GameRuntime`; the control commands `picker`, `pick`, `select` |
| `picker_content.{h,cpp}`, `picker_layout.{h,cpp}`, `picker_composite.{h,cpp}` | what the panels advertise, their geometry and the picture rect each one shows, and the composited window frame |
| `panel_logo.{h,cpp}` | `PanelLogo`: a `LogoImage` plus its lazily uploaded texture and nearest magnification |
| `logo_image.h` | `LogoImage`: straight-alpha RGBA8, included by `game_runtime.h` without SDL |
| `product_host.{h,cpp}` | `ProductHost`: the process top level; owns the command line and help, the window and device, the `QuitScope`, the `RunBudget` and the process watchdog |
| `product_main.cpp` | The framework's only `main`: builds the title's catalog and calls `ProductHost::run`; compiled into each product by `psxport_add_product` |

## The catalog contract

```cpp
class TitleCatalog {
  virtual std::string_view productName() const = 0;          // the window title, picker heading and help name
  virtual std::filesystem::path provisioningRoot() const = 0; // where <slug>/<serial> is provisioned
  virtual std::span<const TitleIdentity> titles() const = 0; // index i is catalog entry i everywhere
  virtual GameRuntime &runtime(std::size_t index) const = 0; // installed before that title's Game exists
};
```

`productName()` is the window title for every catalog, one title or several; `ProductHost` sets it on
the presentation before the first present, and no `Game` names the window.

`TitleIdentity` also carries the title's disc facts: `discEnvVar` (the environment/`.env` key read
before `PSXPORT_DISC`), `discExecutablePath` (the boot executable's ISO9660 path, `/`-separated) and
`defaultExecutable(root)` (`<root>/<slug>/<serial>`, the one spelling of where a provisioned executable
lives, used by the probe, the size-1 catalog and the help text).

A selection means serial plus file identity, never the label. `TitleIdentity::slug` is the directory
under `provisioningRoot()` (relative to the working directory: `scratch/assets` or `scratch/bin`), and `pick <slug>` on the control channel resolves against it. The
catalog and the `TitleIdentity` storage must outlive the host; `TitleAvailability::index` is the
catalog position handed back to `runtime(index)`.

## Provisioning, boot entry and admission

Before `ProductHost` selects anything it calls `provisionExecutable` for each catalog title whose
`defaultExecutable` is missing (the one title of a size-1 catalog, every title before the selector's
first probe). It opens the title's disc on its own `DiscState` keyed by `discEnvVar`, reads
`discExecutablePath`, authenticates the bytes with `selectExecutable` against that title alone, and
writes them only on a match. A present file is never read or rewritten. Only `ProductHost` calls it.

The disc is resolved in one order everywhere (`runtime/psx/cd/disc.cpp`): the title's key, `PSXPORT_DISC`
(environment, then `./.env`), the player's saved first-run choice, a `*.chd` drop-in. `SavedDisc::bind`
sets the first and third on a `DiscState`; `TitleSession::boot` and `TitleDisc` both use it.

With no argument a one-title product provisions its executable. If it is neither there nor extractable, `FirstRunSetup` shows the shared setup-ui screen (one judged image, adopted, not
copied): the chosen image must open as a CHD and its `discExecutablePath` must authenticate as the
title; an accepted path is saved and provisioning runs again. Closing the screen exits 0. Without a
window (`psx::config::windowRequested()` false) it renders one offscreen frame, logs the key to set and
exits 2, so a test run never waits on a player. The selector has no first-run screen yet.

A packaged run (`Config::packageRoot`, the AppImage's absolute `APPDIR`) applies
`psx::config::applyPackagedDefaults` (a window, assets under `<APPDIR>/usr/share/psxport`) and, for a
one-title product, enters `<user data>/<slug>` so the provisioning root, settings and memory card are
user data. A packaged multi-title product keeps the working directory it was started in.

`GameRuntime::startProgram` enters the guest, called by `TitleSession::boot` after the overrides,
the control channel and the display clear. Its default does nothing: the title's frame driver enters
at the executable's entry. A crt0 title runs `psx::game::Crt0Boot` (`runtime/psx/game/crt0_boot.h`)
with its measured `Crt0Image`; an incomplete group is refused by name
(`1 of 8 crt0 field(s) are unset (libcInitEntry)`) and the process exits 1 before any guest write.
Tomba 2 replays crt0; its MAIN.EXE handoff follows in its `startProgram`. CTR, Tekken 3, Vagrant
Story, Spider-Man 2 and C-12 keep the default.

`GameRuntime::admitExecutable(core, bytes)` maps the executable and sets the entry registers. Its default is `loadPsxExeImage` plus `applyPsxExeTopLevelRegisters`; a title that publishes
its own image identity overrides it (Crash Bash, which publishes its resident code range and sets no
top-level registers; Vagrant Story, which checks its header and registers its native leaves against the
published resident generation). A refusal leaves Core unchanged and refuses the boot. The bytes reaching it are
the ones the host authenticated against the catalog: `selectExecutableFile` (`runExecutable`) or the
probe (the selector) returns them, `TitleAvailability::bytes` carries them into the session, and the
file is read once. No title authenticates them again.

Every boot goes through `TitleSession::boot`; the framework has no other boot spine. Crash Bash, Toy
Story 2, Mega Man X4, CTR, Tekken 3, Vagrant Story and C-12 are one-title catalogs (`SingleTitleCatalog`): no argument provisions and boots their one executable,
and a title's own run-end lines come from `reportRun`.

`GameRuntime` carries two optional virtuals the host asks, each with a neutral default:

- `panelLogo(Core &)`: the title's own logo, read from its live machine; `nullopt` until there is one.
- `reportRun(Core &)`: title-specific lines after the framework's `run complete` line.

## Data flow

`ProductHost::run(argc, argv)` owns the command line. `-h`/`--help` prints the usage from
`productName()`, the provisioning root and the catalog's titles, and boots nothing. One executable
argument runs that serial-identified executable (`runExecutable`). No argument runs the selector, except
for a catalog of one title, which boots `<root>/<slug>/<serial>` directly. Anything else is refused with
exit status 2, as is an executable that does not authenticate.

`ProductHost::runSelector` probes the catalog, builds `PickerContent`, and runs `PickerSession`. The
picker creates one unbooted `TitleSession` per available title, boots them one at a time (one guest
advances per frame; only the selected panel is audible and no panel sees the player's pad), and on a
choice destroys every panel session and returns a fresh `TitleSession` for the title. `runToEnd`
boots it, claims the debug endpoint, and steps it until `TitleSession::end()`; `session return` or
the ESC row ends it as `ReturnedToSelector` and the selector is rebuilt from a fresh probe.
`runExecutable` skips the selector and runs one serial-identified executable.
Each panel pre-rolls 900 steps from power-on, past its publisher cards onto the title's own
press-start page or opening movie, whether or not it has drawn a picture by then; after that only the
selected panel runs and the others hold. A pre-roll is a fast-forward (`SessionControl::setPreRolling`,
read through `Game::fastForwarding`): its steps do not sleep on the panel's pacer and its audio is
dropped. Each picker frame pre-rolls one panel for as many steps as fit in one NTSC field
(`PreRollBudget`), at least one. A settled panel steps once per picker frame on its own pacer, so the
picker's loop runs at that title's step cadence, but the composite follows the selected panel's
presents: the picker is that panel's pane host (`TitleSession::setPaneHost`), compositing at each pane
build and showing at each pane show, so an interpolated logic frame's in-between reaches the window.
The loop builds, paces and shows a composite itself only on an iteration where no panel presented,
and paces only when the panel's step paced nothing. A pre-rolling panel gets no host, since its
fast-forward steps would composite many times inside one picker frame. Panel 0 starts selected and
audible. The selected panel's present image is the window's size and the panel shows its cover's
share of it; the other panels present at a third of their cover (`presentation.md`). The selected
panel's share of the window width is the settings file's `picker_selected_share` (default 0.6) and the
others split the rest evenly (`PickerLayout::retarget`), so two titles are 60/40 and three 60/20/20. A black or faded present falls
back to the retained copy (`RenderState::retainFilledPresentImage`), which is a GPU→CPU→GPU round trip and
so is refreshed on a gate; a fade therefore goes unnoticed for up to that many presents (measured 15,
issue 0162). The selector logs its end state with the panel
summary (`selector ended ... ; panels: [0]*slug demo ...`).

`ProductHost` hands every session its `SessionServices`: the presentation device, the process
`QuitScope`, the `RunBudget` and whether `PSXPORT_LOAD_STATE` is still owed (only the process's first
product session loads it; a title picked after a return boots from power-on). A session is a `Role::Panel` (a picker panel) or a
`Role::Product` (the confirmed title, or the executable `runExecutable` was given).

`TitleSession::boot`, in order: refuse a boot entry the image cannot honour; read the executable;
install the title's runtime and build its `Game`; attach the
`QuitScope` to its `HostInput`; panel defaults (pane route, suppressed pad, audibility, pre-roll);
the disc key from the title's `TitleIdentity`; `GameRuntime::admitExecutable`; the configuration audit
(`psx::config::report_once`); the per-Core device binds (init then bind, then `Pad::overridesInit`, then
the `a0`/`a1` seed); `GameRuntime::registerOverrides` then `FrameLoopShell::prepareProduct`;
`DbgServer::start` and `store_observe_attach`; for a product only, a `DisplayPresenter` transition present;
`GameRuntime::startProgram`; for the first product session only,
`psx::state::applyConfiguredState` (fatal on failure); then the present-to-present series restarts, so the boot's gap
is not counted as a missed present.

`TitleSession::step` ends the session before stepping on, in order: `session return`
(`ReturnedToSelector`), `Game::run.requestEnd()` (`TitleEnded`), and for a product a window close or
client `quit` (`Quit`) or an exhausted `RunBudget` (`BudgetSpent`). A product with `PSXPORT_REPL` then
waits in `Repl::beforeStep` for a `run N` that covers the step (`quit`, `end` or EOF is `Quit`).
The body is `FieldTurn::beginField`, `FrameLoopShell::step` (one presentation fence; the field number a
`FrameDriver` receives counts from 1 and goes up by one per step),
`RunBudget::step` and `Repl::afterStep` for a product, `FieldTurn::endField`.
A product session's destructor prints the one `RunEndReport` (`run complete`, fallback telemetry,
ledger, guest-call census, present intervals, record stability, audio queue, `reportRun`). `Quit` and
`BudgetSpent` end the process; `ReturnedToSelector` and `TitleEnded` rebuild the selector when the
run came from it, and end the process under `runExecutable`.

### RunBudget and PreRollBudget

`RunBudget` bounds a whole run, counted in presentation-fence steps: one per product step and one per
selector frame. An explicit `PSXPORT_NATIVE_FRAMES` wins; otherwise a pad replay or resume, the REPL or
a live control client leaves it uncapped, a windowed run is uncapped and a headless one stops at 120.
This is the only place the live-client rule lives: `DbgServer::start` opens the channel and answers
no cap, and `ProductHost` takes the process's cap from `RunBudget::fromConfig`.
It never counts a panel's pre-roll steps, and panels never print a run-end report, apply configured
state or pump the REPL. `PreRollBudget` stays the pre-roll owner: how many panel steps fit in one
selector frame's wall time.

### Process lifetime

Once `run` has a selector or a title to start, `ProductHost` builds its process state: the window
and device, the `QuitScope`, the `RunBudget`, the watchdog and the host diagnostics
(`memcensus_init` and `psx::config::report_once`: the memory census and the active-config report). Help and refused
arguments build none of it. The watchdog is disabled when the host is destroyed; sessions never arm
or disarm either. A window close is recorded on the `QuitScope` instead of exiting in
the event drain, so it reaches the run-end report like a client `quit`.

## Invariants

- The window and its device are the product's and outlive every session.
- A Core snapshots the installed `GameRuntime` when it is constructed, so `TitleSession::boot`
  installs `catalog.runtime(index)` immediately before building its `Game`.
- Exactly one guest advances per picker frame.
- The picker frame owns wall time while a panel pre-rolls: no pre-roll step sleeps.
- Nothing under `runtime/psx/host/` names a title, a serial or a slug.

## Adopting it

1. Implement `TitleCatalog` with the title identities (measured from the executables) and one
   `GameRuntime` per title.
2. Override `panelLogo` where the title can say it.
3. Declare `std::unique_ptr<psx::host::TitleCatalog> createCatalog();` in the catalog header and build
   the product with `psxport_add_product(<target> CATALOG <ns>::createCatalog HEADER <header>
   [SOURCES ...] [LIBRARIES ...])` (`cmake/psxport.cmake`). The title writes no `main`.
4. Have the title's frame drivers call `core.game->run.requestEnd()` when the title cannot go on; a
   picked title then returns to the selector.

## Running

Picker frame time is read
from consecutive `[picker] panels:` lines of a paced run (`PSXPORT_DEBUG=picker,pacer`, no NOPACE). The headless walk (`picker`, `pick <slug>`,
`session return`, `pick <slug>`) runs on a built port with `PSXPORT_DEBUG_SERVER` and
`tools/dbgclient.py`; see the consuming port's docs for its route.
