// psx::ui::Component — the base every overlay widget derives from.
//
// SHAPE TAKEN FROM DUSKLIGHT (CC0), `src/dusk/ui/component.{hpp,cpp}`. What is theirs: a component
// owns an element subtree plus the children and event listeners hanging off it, so destroying the
// component tears down exactly what it built; state that the stylesheet cares about lives in the
// DOM as a PSEUDO-CLASS rather than as a C++ bool the sheet cannot see; and `focus()` walks into
// children when the component itself is not focusable.
//
// THE ONE DELIBERATE DEVIATION, and it is why this is not a copy-paste. Dusklight's components
// CREATE their DOM: `add_child<T>(args…)` news a T with `mRoot` as its parent, and T's constructor
// calls `doc->CreateElement(…)` + `parent->AppendChild(…)`. Ours ADOPT an element that the shipped
// `assets/rml/menu.rml` already authored, because the menu's structure is content, not code — a
// game ships its own `menu.rml` and the framework must not hard-code Tomba!2's six tabs in C++.
// So the ownership direction is identical (parent component owns child component's lifetime; child
// owns its subtree's listeners) and only the construction direction differs: `adopt<T>(element, …)`
// rather than `add_child<T>(…)`. The name is different ON PURPOSE — silently redefining
// `add_child` to mean "attach to something that already exists" would read as Dusklight's method
// and behave as ours.
//
// WHY PSEUDO-CLASS AND NOT `SetClass`. RmlUi's `class` attribute is authored content: the document
// says `<tab class="selected">` for the initially-selected tab, and a C++ `SetClass` writes into
// the same namespace the author is using. A pseudo-class is a separate channel the document cannot
// spell, so runtime state can never collide with an authored class and cannot be clobbered by a
// component that rewrites `class` for another reason. Dusklight styles `tab:selected` /
// `button:disabled` exactly this way (`res/rml/tabbing.rcss:29`, `res/rml/window.rcss:201,212`).
//
// NAMING TRAP, measured in Dusklight's own sheets: `:active` is an RmlUi BUILT-IN pseudo-class
// meaning "mouse is held down on this element" (`res/rml/tabbing.rcss:42` uses `tab:active` for
// exactly that, alongside `tab:selected` on line 29). So a component may NOT express "this pane is
// the shown one" as `:active`; ours uses `:shown`. The old `pane.active` class name was one letter
// away from being silently overridden by a mouse press.
#ifndef PSXPORT_UI_COMPONENT_H
#define PSXPORT_UI_COMPONENT_H

#include "ui_event.h"

#include <RmlUi/Core.h>

#include <memory>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace psx::ui {

// ---- the ONE place overlay text enters the DOM ---------------------------------------------------
// Every readout and row value is DATA — numbers, a stage name, a track title — and `SetInnerRML`
// PARSES ITS ARGUMENT AS MARKUP, so data must be encoded on the way in. Going through one boundary
// is what keeps two failure modes structurally impossible rather than merely absent today:
//
//   * a value can never be read as markup. The readouts used to be snprintf'd straight into
//     SetInnerRML with a `&middot;` separator, and RmlUi decodes only `&lt; &gt; &amp; &quot;` plus
//     numeric references — so the user saw the literal text "&middot;" (see rml_text.h).
//   * the "don't rewrite unless it changed" guard actually fires. `GetInnerRML()` returns
//     `EncodeRml(text)`, so comparing it against a RAW string containing `& < > "` never matched
//     and the element was reparsed and relaid-out every single frame.
//
// tests/test_rml_text_encoding.cpp asserts the whole UI subsystem holds exactly one raw inner-RML
// call site — the one in ui_component.cpp. Add readouts by calling set_text, never by reaching for
// that setter again.
void set_text(Rml::Element *el, std::string_view text);

class Component {
public:
  Component() = default;
  explicit Component(Rml::Element *root) : mRoot(root) {}
  virtual ~Component();

  Component(const Component &) = delete;
  Component &operator=(const Component &) = delete;

  // Per-frame CPU step. The base walks children; override to refresh your own element and call
  // Component::update() to keep the walk going.
  virtual void update();

  // Focus this component, or the first child that will take focus. Returns false if nothing did.
  virtual bool focus();

  virtual bool selected() const {
    return mRoot && mRoot->IsPseudoClassSet("selected");
  }
  virtual void set_selected(bool value);
  virtual bool disabled() const {
    return mRoot && mRoot->IsPseudoClassSet("disabled");
  }
  virtual void set_disabled(bool value);

  // Register an event listener whose lifetime is this component's. `element` may be null, meaning
  // this component's own root.
  void listen(Rml::Element *element, Rml::EventId event, ScopedEventListener::Callback callback, bool capture = false);

  // Is `element` this component's root or a descendant of it?
  bool contains(Rml::Element *element) const;

  Rml::Element *root() const {
    return mRoot;
  }

protected:
  // ADOPT an authored element as a child component (see the header comment for why this is not
  // Dusklight's `add_child`). `root` must be a descendant of this component's root; the returned
  // reference stays valid until this component is destroyed or clear_children() is called.
  // (Dusklight constrains the equivalent with a C++20 `requires`; the framework builds as C++17
  // — cmake/psxport.cmake sets CXX_STANDARD 17 for the mednafen backends — so the same constraint
  // is a static_assert. Same guarantee, same message when it fires.)
  template <typename T, typename... Args> T &adopt(Rml::Element *root, Args &&...args) {
    static_assert(std::is_base_of<Component, T>::value, "adopt<T>() requires T to derive from psx::ui::Component");
    auto child = std::make_unique<T>(root, std::forward<Args>(args)...);
    T &ref = *child;
    mChildren.emplace_back(std::move(child));
    return ref;
  }

  // Drop every child component (and therefore every listener they registered). Does NOT remove
  // the authored elements — we do not own them, the document does.
  void clear_children() {
    mChildren.clear();
  }

  Rml::Element *mRoot = nullptr;
  std::vector<std::unique_ptr<Component>> mChildren;
  std::vector<std::unique_ptr<ScopedEventListener>> mListeners;
};

} // namespace psx::ui

#endif
