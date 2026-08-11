# Adopting Dusklight's UI structure in `psxport/runtime/recomp/rmlui_overlay.cpp`

**Status: A PLAN. Nothing here is implemented.** Written 2026-08-06 alongside the entity-bug fix
(`coord/claims/rmlui-overlay/`), which deliberately did NOT restructure the overlay.

Source read: `~/repo/dusklight` @ `13b3b68f`, CC0. **Take the SHAPE, not a copy-paste.** Every item
below cites the Dusklight file it comes from.

---

## The measured gap

| | Dusklight | psxport |
|---|---|---|
| RmlUi UI code | 70 files in `src/dusk/ui/`, one file pair per component | **ONE file**, `rmlui_overlay.cpp` (~570 lines) |
| Developer overlays | a SECOND stack, `src/dusk/imgui/` (21 files), on purpose | folded into the same shipped RmlUi menu |
| Stylesheets | 10 `res/rml/*.rcss`, one per component area | 2 (`rml.rcss`, `menu.rcss`) |
| Row behaviour | the row OWNS its binding (`ControlledSelectButton` takes `getValue`/`isDisabled`/`isModified` as `std::function` props) | 3 parallel `if/else` chains over the same ~20 string ids |
| Listeners | `ScopedEventListener` RAII, `OnDetach` nulls itself, owned by the component | `TabClick`/`RowClick` stored as `void*`, manually `new`/`delete`d in `init`/`shutdown` |
| State | RCSS **pseudo-classes** (`SetPseudoClass("selected"/"disabled")`) | `SetClass("selected"/"active")` juggled in C++ |
| Documents | a `Document` class with `show/hide/cover/uncover/push/pop` and a document STACK | one document + a `bool mVisible` |

**One thing psxport already got right and must not be redone:** `assets/rml/menu.rml`'s markup
vocabulary (`<window>/<tab-bar>/<tab>/<content>/<pane>/<select-button>/<key>/<value>`) is already
Dusklight's, via soh3d — its own file header says so. The DOM shape came across; the C++ did not.

## The concrete cost, not a style complaint

Adding one settings row today means editing **four** places: `menu.rml`, `row_value_text()`,
`do_toggle()` *or* `do_adjust()`, and sometimes the arrow-key switch in `event()`. The three C++
chains are matched by STRING ID, so a typo in one of them is a row that silently stops updating —
there is no compiler check that the three agree. That is the same failure class as the bug just
fixed: meaning recovered by matching strings after the fact, rather than owned structurally.

---

## Adoption order

Ordered by dependency, and by how much each unblocks. **Steps 1–3 are the ones worth doing;**
4–6 only pay off once the overlay grows past one document.

### 1. `ScopedEventListener` — RAII listeners *(smallest, entirely self-contained)*
From `src/dusk/ui/event.{hpp,cpp}`. Take: the `Rml::EventListener` subclass holding
`element/event/capture/std::function`, removing itself in the destructor, **and `OnDetach()` nulling
`mElement`** so a listener outliving its element cannot double-remove.

Replaces `mTabListeners`/`mRowListener` (`void*` + manual `new`/`delete` + `attachHandlers()`
re-adding to every element on each document load). Deletes two hand-rolled listener classes and two
`void*` members. No behaviour change, so it needs no new gate beyond the existing suite.

### 2. `Component` + `add_child<T>()` — the subtree-owning base *(the real unlock)*
From `src/dusk/ui/component.{hpp,cpp}`. Take: `mRoot` + `mChildren` (vector of `unique_ptr<Component>`)
+ `mListeners`; `add_child<T>(args…)` constructing `T(mRoot, args…)`; the recursive `update()` and
`focus()` (focus self, else first focusable child); `contains()` walking up parents.

**Then the row types**, from `src/dusk/ui/select_button.hpp` — this is the item that kills the three
string-matched chains:

```
ControlledSelectButton::Props {
    key;  getValue();  isDisabled();  isModified();
}
```

Each row carries its own value-formatter and predicates. `row_value_text` / `do_toggle` /
`do_adjust` collapse into one registration per setting, in one place, checked by the compiler.
`refreshAllRows()` (currently `GetElementsByTagName` + a string-matched if-chain per row per refresh)
becomes `Component::update()` recursing into children.

**Do this step behind the boundary the entity fix just built**: rows set their text through
`set_text()` / `rml_text_markup()`, never by concatenating markup. The lint in
`tests/test_rml_text_encoding.cpp` (exactly one raw inner-RML call site) is what keeps that true
while the file is being torn apart.

### 3. State as RCSS pseudo-classes
From `Component::set_selected` / `set_disabled`. Take: `SetPseudoClass("selected"/"disabled")` plus,
for disabled, `SetAttribute("disabled")` **and `Blur()`** so a disabled row cannot keep focus.

Replaces `SetClass("selected", …)` / `SetClass("active", …)` in `setActiveTab`. Styling moves out of
C++ into the stylesheet, which is the point of using RmlUi at all. Cheap, and it is a prerequisite
for splitting the stylesheets sensibly (step 6).

### 4. Split the file, one pair per component
Only once 1–3 exist — splitting first just scatters the same coupling. Target, mirroring Dusklight's
naming: `ui/component.{h,cpp}`, `ui/event.{h,cpp}`, `ui/select_button.{h,cpp}`, `ui/tab_bar.{h,cpp}`,
`ui/window.{h,cpp}`, `ui/document.{h,cpp}`. This is also the `docs/workspace/LAYOUT.md` work (`runtime/recomp/`
is 149 files flat) — do them together, not twice.

### 5. **Split the DEV overlay off the shipped UI** — the item with a live bug attached
Dusklight keeps `src/dusk/imgui/` separate *on purpose*: shipped UI and developer overlays have
different requirements and must not share a framework. psxport folded the dev readouts (video/world
position/area-warp/debug toggles) into the shipped RmlUi menu, and that is **why framework code reads
Tomba!2 guest addresses** — `overlay_glue.cpp:29` reads `0x1F8000D2`/`0x801FE00C` and
`refreshReadouts()` decodes Tomba!2 stage pointers, in a framework that `#include`s nothing from a
game. In Spyro that readout is meaningless and churns every frame (spyro issue #52 defect 6).

Two acceptable shapes; pick one deliberately:
- the honest minimum — the world readout moves behind a `GameHook`, so the framework asks the game
  for a display string instead of decoding one game's RAM; or
- the full Dusklight shape — a second, developer-only stack (`vendor/imgui` is **already vendored**),
  leaving the RmlUi menu for shipped settings only.

Either way this is a real bug fix, not a refactor, and it is the highest-value item after step 2.

### 6. `Document` + stylesheet split
From `src/dusk/ui/document.{hpp,cpp}`: `show/hide/cover/uncover/push/pop`, `set_document_styles`,
`DocumentScope`. Worth it only when there is a second document (a modal, a mod browser, a controller
config). Today `bool mVisible` is honestly sufficient — **do not build the stack before there is
something to stack.** Split `menu.rcss` per component in the same change.

---

## What NOT to take

- **`Document`'s stack, and `Component`'s virtual `update()`, before step 4.** They only pay off with
  several components; adopted early they are ceremony around one menu.
- **Dusklight's frame interpolation** (`src/dusk/frame_interpolation.{h,cpp}`). Nothing to do with the
  UI, and `docs/workspace/PROTOCOL.md` gates it on execution ownership — theirs lerps FLOAT decomp values,
  ours would be s16 GTE output.
- **Dusklight's sound-effect constants** (`ui.hpp`'s `kSoundClick = Z2SE_SY_CURSOR_OK`, …). That is a
  Zelda id namespace; a psxport equivalent must come through `GameHooks`, not be named in framework code.

## Unresolved

I could not locate where Dusklight creates its `Rml::Context` — no `Rml::CreateContext` or
`SetDimensions` anywhere under `src/`. So I have **no** reading on how it reconciles logical points
against pixels, which is exactly the question behind spyro issue #52 defect 4. If that comparison
matters, find it before assuming psxport's answer is the odd one. (psxport's answer as of the
entity-fix patch: context and render viewport both come from ONE `SDL_GetWindowSizeInPixels`-based
sink measurement.)

Note also that Dusklight's HEAD commit is *"Migrate to Borealis (#2266)"* — a stack migration is in
flight there. Re-read before starting step 2; the componentisation above may itself be moving.
