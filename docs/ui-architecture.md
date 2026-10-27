# The overlay UI — component model, and why there is only ONE stack

This page is psxport's authority for UI ownership. The boundaries follow psxport's lifecycle,
headless-operation, content-authoring, and testability requirements; the codemap decides placement.

## Where it lives

    runtime/ui/rmlui_overlay.{h,cpp}      one Game's overlay: fonts, context, LoadDocument, SDL
                                           event routing, the GPU record call. Knows no elements.
    runtime/ui/rmlui_library.{h,cpp}     one Rml::Initialise per RenderDevice: render and system
                                           interfaces, context names
    runtime/psx/rml_text.{h,cpp}        the DATA -> RML markup encoder
    shared/bug-report (bug_report::Form) the B-key bug report form: a modal document the overlay
                                           hosts in the menu context (showBugReport/closeBugReport);
                                           while open it takes every event, ESC included, and
                                           typingText() keeps P / '.' / B from firing as commands

    runtime/ui/ui_event.{h,cpp}            ScopedEventListener — registration IS lifetime
    runtime/ui/ui_component.{h,cpp}        Component base + set_text(), the ONE data->DOM boundary
    runtime/ui/ui_assets.{h,cpp}           asset resolution that refuses to report success
    runtime/ui/mod_row_model.{h,cpp}       what a row MEANS (the Mods toggle/adjust tables)
    runtime/ui/title_debug_rows.{h,cpp}    the Debug and About tabs generated from the title's TitleDebugMenu
    runtime/ui/menu_row.{h,cpp}            one <select-button> + its binding (incl. a title's stepped integer setting)
    runtime/ui/menu_pane.{h,cpp}           one tab's page of rows
    runtime/ui/menu_tab_bar.{h,cpp}        the <tab> row and which one is selected
    runtime/ui/menu_readouts.{h,cpp}       the live video line, the debug status and the title's readouts
    runtime/ui/menu_document.{h,cpp}       the tree over assets/rml/menu.rml

    assets/rml/menu.rml                    the document — CONTENT, shipped, per-game
    assets/rml/menu.rcss, rml.rcss         the stylesheets — no CSS lives in C++

This replaced a single 592-line `rmlui_overlay.cpp` holding RmlUi setup, tab selection, row value
formatting, the `Mods` ladders, the dev warp, the readouts, focus navigation and the SDL key switch.

## Component decisions

1. **A component owns its subtree.** Destroying a component
   tears down its children and every listener it registered. `RmlOverlay::shutdown()` now drops the
   component tree *before* `Rml::Shutdown()`; the old code deleted its listener objects *after*,
   and never called `RemoveEventListener` at all.
2. **Scoped event listeners.** `OnDetach` nulls the element, so a
   listener outliving its element is inert rather than dangling.
3. **State as a pseudo-class, not a C++ bool or a `class`.** `class` is the document author's namespace; a runtime `SetClass`
   writes into it. **Naming trap:** `:active` is an RmlUi BUILT-IN meaning "mouse held down"
   while a press is held. A pane's visibility is therefore `:shown`, never `:active`.

## Authored DOM ownership

psxport components adopt elements already authored by `assets/rml/menu.rml`; they do not construct
the DOM in C++. The method is therefore named `adopt<T>(element, args…)`.

The reason is the framework/game seam: **the menu's structure is CONTENT.** A game ships its own
`menu.rml`, and psxport hard-coding Tomba!2's six tabs in C++ would be the framework knowing a game
— the framework/game coupling the link boundary exists to prevent.

The two generated exceptions are the Debug tab's `#title_debug` and the About tab's `#about`:
`TitleDebugRows` fills them from the running title's `TitleDebugMenu` before the panes adopt their
rows, so every title gets its own warps, readouts, sound test and About lines with no title text in
`menu.rml`. Row ids are `debug:<verb>:<param>` (a stepped parameter) and `debug:<verb>` (run).

## ONE stack, not two — the ImGui decision (2026-08-06)

Shipped UI and developer tooling have different lifetimes, audiences, and headless requirements, so
they do not share a presentation owner. psxport currently needs no second graphical developer stack.
Measured, not assumed:

**`vendor/imgui` is dead, not tangled.** 2.9 MB of source committed directly (not a submodule),
referenced by **zero** build files and **zero** source files. The only references that existed were
two stale comments — `rmlui_overlay.cpp:1` ("replaces the former Dear ImGui overlay") and
`docs/config.md`'s `UI` entry, which still described the overlay as "Dear ImGui … windowed only"
when both halves had been false for some time. Both are now gone, so there is nothing half-wired.

**psxport already has the developer stack, and it is better suited than ImGui here.**
`runtime/psx/debug/debug_commands.cpp` holds the developer verbs; `repl.cpp` (stdin) and `dbg_server.cpp`
(HTTP + `tools/dbgclient.py`) carry them and work with no window at all.

**And that is the deciding constraint.** `docs/workspace/PROTOCOL.md`: *agents never run windowed*. An ImGui
developer stack draws into the swapchain, so the primary consumer of developer tooling in this
project — an agent — could not see or drive any of it. That is precisely the defect this UI work
was sent to fix, and adopting a second stack now would double the amount of host UI that no
instrument can capture.

The sink pass (`RenderState::recordScreen`: the present image letterboxed, the overlay on top) draws the
window, and `pshot` runs the same pass into an off-screen image at the sink size, so host UI is
captured headless exactly as a player sees it.

If the "not yet" holds, the operator should remove the dead vendored copy rather than leave 2.9 MB
implying a stack that grep says does not exist (re-vendoring later is one `git clone`):

    git rm -r vendor/imgui

## Driving the menu without a window

A hidden window drives the menu through the player's own path: the control channel's `key`, `text`,
`mouse`, `touch` and `gamepad` verbs push SDL events into the window's queue on the main thread
(`input/input_injection.h`), and `HostInput` hands them to RmlUi exactly as a real device would. ESC opens
the menu, a click at a tab's window coordinates selects it, arrows and Return walk and run rows. `pshot`
shows the result. `menu` alone dumps every tab, pane and row with its LIVE value to the log.

The dump is the instrument that answers "did a change lose a row?" by measuring instead of by
counting two files by hand. It prints its counts first, so the enumeration always arrives with its
denominator.

An authored row is content, but it is not proof that a title implements the feature. Bindings ask
the title's `Mods` policy (`GameRuntime::interpolates()` for fps60) whether they are available. `MenuPane` removes an
unavailable binding from layout and navigation; it does not leave a disabled or inert row. Unknown
authored ids remain visible and loudly inert because those are document defects, not capability
absences.

## The bug report form

B opens the
shared `bug_report::Form` over the held frame; `psx::debug::BugReportSession` owns when and what
(see `docs/codemap.md`, "Bug report"). The form is a modal document in the menu context and is
recorded in the overlay's window pass, never into the present image, so the report's own screenshot
is the game's picture. Its pictures load through `RmlRenderInterfaceGpu::LoadTexture` (SDL_image,
premultiplied), which is why that method decodes files rather than refusing them.

RmlUi is the upstream 6.3 release as the `vendor/rmlui` submodule.

## Rules for adding to the menu

- **Text goes in through `psx::ui::set_text()` and nowhere else.** It is the only raw inner-RML call
  site in the subsystem; keep it the only one in the subsystem.
- **A new row kind is a new `RowBinding`**, not a branch in `MenuDocument`.
- **A new Mods-backed `toggle=`/`adjust=` id is a row in `mod_row_model.cpp`'s table.** Feature-owned
  state gets a cohesive control plus `RowBinding`, as `title_debug_rows` does;
  `MenuDocument` only maps the authored id to that binding. An unresolved id is an ERROR at load.
- **Non-ASCII glyphs in `.rml` are numeric character references** (`&#183;`), never HTML entity
  names: RML is XML-ish, and an unknown name renders literally. The test lints the shipped assets.
