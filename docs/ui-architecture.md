# The overlay UI — component model, and why there is only ONE stack

Modelled on **Dusklight** (`github.com/TwilitRealm/dusklight`, CC0), `src/dusk/ui/`. What was taken
and where ours deliberately differs is documented at each file; this page is the map and the
decisions.

## Where it lives

    runtime/recomp/rmlui_overlay.{h,cpp}   RmlUi LIFETIME ONLY — render interface, SDL system
                                           interface, fonts, context, LoadDocument, SDL event
                                           routing, the GPU record call. Knows no elements.
    runtime/recomp/overlay_glue.{h,cpp}    the four hooks gpu_vk.cpp calls
    runtime/recomp/rml_text.{h,cpp}        the DATA -> RML markup encoder

    runtime/ui/ui_event.{h,cpp}            ScopedEventListener — registration IS lifetime
    runtime/ui/ui_component.{h,cpp}        Component base + set_text(), the ONE data->DOM boundary
    runtime/ui/ui_assets.{h,cpp}           asset resolution that refuses to report success
    runtime/ui/mod_row_model.{h,cpp}       what a row MEANS (the Mods toggle/adjust tables)
    runtime/ui/warp_control.{h,cpp}        the Debug tab's dev area warp
    runtime/ui/render_path_control.{h,cpp} the Display tab's title-capability-filtered player paths;
                                           the PSX software path stays diagnostic-only
    runtime/ui/menu_row.{h,cpp}            one <select-button> + its binding
    runtime/ui/menu_pane.{h,cpp}           one tab's page of rows
    runtime/ui/menu_tab_bar.{h,cpp}        the <tab> row and which one is selected
    runtime/ui/menu_readouts.{h,cpp}       the live video/world/music/warp status lines
    runtime/ui/menu_document.{h,cpp}       the tree over assets/rml/menu.rml

    assets/rml/menu.rml                    the document — CONTENT, shipped, per-game
    assets/rml/menu.rcss, rml.rcss         the stylesheets — no CSS lives in C++

This replaced a single 592-line `rmlui_overlay.cpp` holding RmlUi setup, tab selection, row value
formatting, the `Mods` ladders, the dev warp, the readouts, focus navigation and the SDL key switch.

## The three shapes taken from Dusklight

1. **A component owns its subtree** (`src/dusk/ui/component.{hpp,cpp}`). Destroying a component
   tears down its children and every listener it registered. `RmlOverlay::shutdown()` now drops the
   component tree *before* `Rml::Shutdown()`; the old code deleted its listener objects *after*,
   and never called `RemoveEventListener` at all.
2. **Scoped event listeners** (`src/dusk/ui/event.{hpp,cpp}`). `OnDetach` nulls the element, so a
   listener outliving its element is inert rather than dangling.
3. **State as a pseudo-class, not a C++ bool or a `class`** (`res/rml/tabbing.rcss:29`,
   `res/rml/window.rcss:201`). `class` is the document author's namespace; a runtime `SetClass`
   writes into it. **Naming trap:** `:active` is an RmlUi BUILT-IN meaning "mouse held down"
   (Dusklight uses `tab:active` for exactly that, one rule away from `tab:selected`). A pane's
   visibility is therefore `:shown`, never `:active`.

## The one place ours deliberately differs

**Dusklight's components CREATE their DOM; ours ADOPT it.** Theirs call `CreateElement` /
`AppendChild`, so `add_child<T>(args…)` news a T with `mRoot` as parent. Ours take an element that
`assets/rml/menu.rml` already authored, so the method is `adopt<T>(element, args…)` — a different
name on purpose, because silently redefining `add_child` would read as their method and behave as
ours.

The reason is the framework/game seam: **the menu's structure is CONTENT.** A game ships its own
`menu.rml`, and psxport hard-coding Tomba!2's six tabs in C++ would be the framework knowing a game
— the thing `psxport_smoke` exists to prevent.

## ONE stack, not two — the ImGui decision (2026-08-06)

Dusklight runs **two** UI stacks on purpose: `src/dusk/ui/` (RmlUi, game-facing) and
`src/dusk/imgui/` (13 .cpp of developer overlays — console, save editor, heap/process/camera,
actor spawner). Shipped UI and debug UI have different requirements and should not share a
framework. psxport should NOT copy that yet. Measured, not assumed:

**`vendor/imgui` is dead, not tangled.** 2.9 MB of source committed directly (not a submodule),
referenced by **zero** build files and **zero** source files. The only references that existed were
two stale comments — `rmlui_overlay.cpp:1` ("replaces the former Dear ImGui overlay") and
`docs/config.md`'s `UI` entry, which still described the overlay as "Dear ImGui … windowed only"
when both halves had been false for some time. Both are now gone, so there is nothing half-wired.

**psxport already has the developer stack, and it is better suited than ImGui here.**
`runtime/recomp/repl.cpp` (stdin) and `runtime/recomp/dbg_server.cpp` (TCP + `tools/dbgclient.py`)
are the analogue of Dusklight's ImGui console, and unlike ImGui they work with no window at all.

**And that is the deciding constraint.** `docs/workspace/PROTOCOL.md`: *agents never run windowed*. An ImGui
developer stack draws into the swapchain, so the primary consumer of developer tooling in this
project — an agent — could not see or drive any of it. That is precisely the defect this UI work
was sent to fix, and adopting a second stack now would double the amount of host UI that no
instrument can capture.

**So: NOT YET. What it is waiting on, concretely — the SINK PASS must exist in both legs.**
`GpuVkState::show_present_image()` (gpu_vk.cpp) is the sink pass, and it runs only when
`plan.to_swapchain`, i.e. windowed. Every `overlay_glue_record()` call site lives in a windowed-only
path (gpu_vk.cpp:1246 inside `show_present_image`; gpu_vk.cpp:1330 after
`gpu_vk_present_image`'s `if (s_headless) return`). MEASURED 2026-08-06: two headless
`PSXPORT_PRESENT_SHOT_AT=70` captures, one with the menu SHOWN and one HIDDEN, are **byte-identical**
(md5 `f68b47bc971bed9bbfa4b27c19e7ee1c`) — even though the menu's own stylesheet paints an
`rgba(0,0,0,82%)` scrim over the whole screen. Negative control for that instrument: frames 100/300
match each other but frame 600 differs (md5 `df5c303f…`, 99.85% vs 90.68% non-black), so the
instrument can distinguish two present frames and the identity above is a real negative, not a blind
one.

Once the sink pass runs in both legs (its target the swapchain windowed, a sink-sized texture
headless) any host UI becomes capturable headless, and a second stack stops being a blind spot.
**That pass belongs to the `present-image-sink` claim, which explicitly places the overlay in the
windowed-only SHOW half — so it is that claim's decision to revisit, not this one's.**

If the "not yet" holds, the operator should remove the dead vendored copy rather than leave 2.9 MB
implying a stack that grep says does not exist (re-vendoring later is one `git clone`):

    git rm -r vendor/imgui

## Driving the menu without a window

The menu is driven by SDL keyboard events, which do not exist headless. The REPL therefore carries
a driving surface — host UI state only, no guest state, like `press`/`tap`:

    menu [on|off|toggle]              show / hide
    menu tab <index>                  select a tab
    menu nav <up|down|left|right|enter>   navigate / activate the focused row
    menu dump                         enumerate every tab, pane and row with its LIVE value

`menu dump` is the instrument that answers "did a change lose a row?" by measuring instead of by
counting two files by hand. It prints its counts first, so the enumeration always arrives with its
denominator.

An authored row is content, but it is not proof that a title implements the feature. Bindings ask
the title's `RenderCapabilities`/`Mods` policy whether they are available. `MenuPane` removes an
unavailable binding from layout and navigation; it does not leave a disabled or inert row. Unknown
authored ids remain visible and loudly inert because those are document defects, not capability
absences.

## Rules for adding to the menu

- **Text goes in through `psx::ui::set_text()` and nowhere else.** It is the only raw inner-RML call
  site in the subsystem, and `tests/test_rml_text_encoding.cpp` enumerates `runtime/ui/**` at run
  time (not a hardcoded list) and fails if there is ever more than one, or if it is in the wrong
  file.
- **A new row kind is a new `RowBinding`**, not a branch in `MenuDocument`.
- **A new Mods-backed `toggle=`/`adjust=` id is a row in `mod_row_model.cpp`'s table.** Feature-owned
  state gets a cohesive control plus `RowBinding`, as `warp_control` and `render_path_control` do;
  `MenuDocument` only maps the authored id to that binding. An unresolved id is an ERROR at load.
- **Non-ASCII glyphs in `.rml` are numeric character references** (`&#183;`), never HTML entity
  names: RML is XML-ish, and an unknown name renders literally. The test lints the shipped assets.
