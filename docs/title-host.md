# Title host

The framework's multi-title product host: one process, one window and one presentation device, an
in-window title selector, the chosen title, and back to the selector. A port with several titles
(Spyro 1/2/3, Crash 1/2/3, Spider-Man 1/2, Tomba 1/2) supplies a catalog and a factory for it; the
framework's one `main` runs it through `psx::host::ProductHost`. A title writes no `main`, command line or
selector code of its own. Code lives in `runtime/psx/host/`,
namespace `psx::host`.

## What it owns

| File | Owner |
| --- | --- |
| `title_catalog.h` | `TitleIdentity` (one catalog title: serial plus file identity) and `TitleCatalog` (what a product supplies, including its provisioning root) |
| `title_selection.{h,cpp}` | `selectExecutable`, `selectExecutableFile`: does these bytes match this catalog entry (size, PS-X EXE header words, SHA-256) |
| `title_availability.{h,cpp}` | `TitleAvailabilityProbe`: which catalog titles are provisioned and authenticate at `<root>/<slug>/<serial>` right now |
| `title_session.{h,cpp}` | `TitleSession`: one boot-to-exit run of one title as a steppable owner; destruction is the whole teardown |
| `session_run.h` | `SessionRun`, held per `Game` as `Game::run`: a title's request to end its run |
| `run_budget.{h,cpp}` | `RunBudget`: the process's run cap in presentation-fence steps, decided once from `PSXPORT_NATIVE_FRAMES` |
| `panel_sessions.{h,cpp}` | `PanelSessions`: one live session per panel (the available titles `PSXPORT_PICKER_TITLES` asks for) and the rules about which of them runs |
| `picker_session.{h,cpp}` | `PickerSession`: the selector frame loop; one guest advances per frame |
| `picker_runtime.{h,cpp}` | `PickerRuntime`: the selector's own `GameRuntime`; the control commands `picker`, `pick`, `select` |
| `picker_content.{h,cpp}`, `picker_layout.{h,cpp}`, `picker_composite.{h,cpp}` | what the panels advertise, their geometry and cover crop, and the composited window frame |
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

A selection means serial plus file identity, never the label. `TitleIdentity::slug` is the directory
under `provisioningRoot()` (relative to the working directory: `scratch/assets` or `scratch/bin`), and `pick <slug>` on the control channel resolves against it. The
catalog and the `TitleIdentity` storage must outlive the host; `TitleAvailability::index` is the
catalog position handed back to `runtime(index)`.

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
picker then runs at that title's step cadence. Panel 0 starts selected and audible.

`ProductHost` hands every session its `SessionServices`: the presentation device, the process
`QuitScope`, the `RunBudget` and whether `PSXPORT_LOAD_STATE` is still owed (only the process's first
product session loads it; a title picked after a return boots from power-on). A session is a `Role::Panel` (a picker panel) or a
`Role::Product` (the confirmed title, or the executable `runExecutable` was given).

`TitleSession::boot`, in order: install the title's runtime and build its `Game`; attach the
`QuitScope` to its `HostInput`; panel defaults (pane route, suppressed pad, audibility, pre-roll);
`load_exe`; `psx::Machine` `reportConfigurationOnce`, `bindDevices` (init then bind, honouring
`Pad::overridesInit`), `prepare`, `attachControlChannel`; for a product only,
`clearDisplayForFrontEnd`; `setupGuestBoot` (crt0) and `GameRuntime::bootInit`; for the first product
session only, `applyConfiguredState`; then the present-to-present series restarts, so the boot's gap
is not counted as a missed present.

`TitleSession::step` ends the session before stepping on, in order: `session return`
(`ReturnedToSelector`), `Game::run.requestEnd()` (`TitleEnded`), and for a product a window close or
client `quit` (`Quit`) or an exhausted `RunBudget` (`BudgetSpent`). A product with `PSXPORT_REPL` then
waits in `Repl::beforeStep` for a `run N` that covers the step (`quit`, `end` or EOF is `Quit`).
The body is `FieldTurn::beginField`, `FrameLoopShell::step` (one presentation fence),
`RunBudget::step` and `Repl::afterStep` for a product, `FieldTurn::endField`.
A product session's destructor prints the one `RunEndReport` (`run complete`, fallback telemetry,
ledger, guest-call census, present intervals, record stability, audio queue, `reportRun`). `Quit` and
`BudgetSpent` end the process; `ReturnedToSelector` and `TitleEnded` rebuild the selector when the
run came from it, and end the process under `runExecutable`.

### RunBudget and PreRollBudget

`RunBudget` bounds a whole run, counted in presentation-fence steps: one per product step and one per
selector frame. An explicit `PSXPORT_NATIVE_FRAMES` wins; otherwise a pad replay or resume, the REPL or
a live control client leaves it uncapped, a windowed run is uncapped and a headless one stops at 120.
This is the only place the live-client rule lives: `Machine::attachControlChannel` and
`DbgServer::start` open the channel and answer no cap, and every loop that is not yet a
`TitleSession` (native_boot, Tekken 3, C-12, Tomba! 1) takes its cap from `RunBudget::fromConfig`.
It never counts a panel's pre-roll steps, and panels never print a run-end report, apply configured
state or pump the REPL. `PreRollBudget` stays the pre-roll owner: how many panel steps fit in one
selector frame's wall time.

### Process lifetime

Once `run` has a selector or a title to start, `ProductHost` builds its process state: the window
and device, the `QuitScope`, the `RunBudget`, the watchdog and the host diagnostics
(`Machine::armHostDiagnostics`: the memory census and the active-config dump). Help and refused
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

## Testing

`tests/test_title_selection.cpp`, `test_title_picker.cpp` and `test_picker_layout.cpp` cover
selection, availability, the control commands and panel geometry over a fake catalog, with no window.
`test_run_budget.cpp` covers the cap's precedence. `psxport_product_fixture` (`tests/product_fixture_catalog.*`) is a product built by `psxport_add_product` over an empty catalog; CTest runs its `-h`/`--help` and a refused argument through `product_main.cpp`. `test_product_host_session.cpp` runs the shipping
`ProductHost` through `run(argc, argv)` over a stub catalog and frame driver on an offscreen device: help
and refused arguments boot nothing; a one-title catalog boots without a selector; the cap, a client quit and
a window close each reach one run-end report; a capped selector boots its panels without one; a
window close in the selector ends it without a report; and a picked title that ends its own run
returns to the selector, where a second pick boots without reloading `PSXPORT_LOAD_STATE`. `test_pre_roll_budget.cpp` covers the per-frame pre-roll
budget and `test_frame_pacer.cpp` that a pre-rolling Game does not pace. Picker frame time is read
from consecutive `[picker] panels:` lines of a paced run (`PSXPORT_DEBUG=picker,pacer`, no NOPACE). The headless walk (`picker`, `pick <slug>`,
`session return`, `pick <slug>`) runs on a built port with `PSXPORT_DEBUG_SERVER` and
`tools/dbgclient.py`; see the consuming port's docs for its route.
