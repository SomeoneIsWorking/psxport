# Title host

The framework's multi-title product host: one process, one window and one presentation device, an
in-window title selector, the chosen title, and back to the selector. A port with several titles
(Spyro 1/2/3, Crash 1/2/3, Spider-Man 1/2, Tomba 1/2) supplies a catalog and composes
`psx::host::ProductHost`; it writes no selector code of its own. Code lives in `runtime/psx/host/`,
namespace `psx::host`.

## What it owns

| File | Owner |
| --- | --- |
| `title_catalog.h` | `TitleIdentity` (one catalog title: serial plus file identity) and `TitleCatalog` (what a product supplies) |
| `title_selection.{h,cpp}` | `selectExecutable`, `selectExecutableFile`: does these bytes match this catalog entry (size, PS-X EXE header words, SHA-256) |
| `title_availability.{h,cpp}` | `TitleAvailabilityProbe`: which catalog titles are provisioned and authenticate at `<root>/<slug>/<serial>` right now |
| `title_session.{h,cpp}` | `TitleSession`: one boot-to-exit run of one title as a steppable owner; destruction is the whole teardown |
| `session_run.h` | `SessionRun`, held per `Game` as `Game::run`: the delivered-field cap and the end request |
| `panel_sessions.{h,cpp}` | `PanelSessions`: one live session per panel (the available titles `PSXPORT_PICKER_TITLES` asks for) and the rules about which of them runs |
| `picker_session.{h,cpp}` | `PickerSession`: the selector frame loop; one guest advances per frame |
| `picker_runtime.{h,cpp}` | `PickerRuntime`: the selector's own `GameRuntime`; the control commands `picker`, `pick`, `select` |
| `picker_content.{h,cpp}`, `picker_layout.{h,cpp}`, `picker_composite.{h,cpp}` | what the panels advertise, their geometry and cover crop, and the composited window frame |
| `panel_logo.{h,cpp}` | `PanelLogo`: a `LogoImage` plus its lazily uploaded texture and nearest magnification |
| `logo_image.h` | `LogoImage`: straight-alpha RGBA8, included by `game_runtime.h` without SDL |
| `product_host.{h,cpp}` | `ProductHost`: the process top level and the owner of the window and device |

## The catalog contract

```cpp
class TitleCatalog {
  virtual std::string_view productName() const = 0;          // the window title and picker heading
  virtual std::span<const TitleIdentity> titles() const = 0; // index i is catalog entry i everywhere
  virtual GameRuntime &runtime(std::size_t index) const = 0; // installed before that title's Game exists
};
```

A selection means serial plus file identity, never the label. `TitleIdentity::slug` is the directory
under the provisioning root, and `pick <slug>` on the control channel resolves against it. The
catalog and the `TitleIdentity` storage must outlive the host; `TitleAvailability::index` is the
catalog position handed back to `runtime(index)`.

`GameRuntime` carries two optional virtuals the host asks, each with a neutral default:

- `panelLogo(Core &)`: the title's own logo, read from its live machine; `nullopt` until there is one.
- `reportRun(Core &)`: title-specific lines after the framework's `run complete` line.

## Data flow

`ProductHost::runSelector` probes the catalog, builds `PickerContent`, and runs `PickerSession`. The
picker creates one unbooted `TitleSession` per available title, boots them one at a time (one guest
advances per frame; only the selected panel is audible and no panel sees the player's pad), and on a
choice destroys every panel session and returns a fresh `TitleSession` for the title. `runToEnd`
boots it, claims the debug endpoint, and steps it until `TitleSession::end()`; `session return` or
the ESC row ends it as `ReturnedToSelector` and the selector is rebuilt from a fresh probe.
`runExecutable` skips the selector and runs one serial-identified executable.
Each panel pre-rolls 900 steps from power-on, past its publisher cards onto the title's own
press-start page or opening movie; after that only the selected panel runs and the others hold.

`TitleSession::boot` sets `Game::run = SessionRun(frameCap)`; `step` ends the run when
`Game::run.shouldEnd()`. A title's frame drivers write `core.game->run` (`fieldDelivered`,
`requestEnd`). The destructor logs the generic `run complete` line, then calls `reportRun`.

## Invariants

- The window and its device are the product's and outlive every session.
- A Core snapshots the installed `GameRuntime` when it is constructed, so `TitleSession::boot`
  installs `catalog.runtime(index)` immediately before building its `Game`.
- Exactly one guest advances per picker frame.
- Nothing under `runtime/psx/host/` names a title, a serial or a slug.

## Adopting it

1. Implement `TitleCatalog` with the title identities (measured from the executables) and one
   `GameRuntime` per title.
2. Override `panelLogo` where the title can say it.
3. `main` constructs the catalog and `psx::host::ProductHost host(catalog, "scratch/assets");` then
   calls `runSelector()` or `runExecutable(path)`.
4. Have the title's frame drivers call `core.game->run.fieldDelivered()` and `requestEnd()`.

## Testing

`tests/test_title_selection.cpp`, `test_title_picker.cpp`, `test_picker_layout.cpp` and
`test_session_run.cpp` cover selection, availability, the control commands, panel geometry and the
run cap over a fake catalog, with no window. The headless walk (`picker`, `pick <slug>`,
`session return`, `pick <slug>`) runs on a built port with `PSXPORT_DEBUG_SERVER` and
`tools/dbgclient.py`; see the consuming port's docs for its route.
