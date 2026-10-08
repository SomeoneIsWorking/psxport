// psx::ui::Component — the base every overlay widget derives from.
//
// A component owns an element subtree plus its child components and event listeners, so destroying
// it tears down exactly those lifetimes. Stylesheet-visible state lives in DOM pseudo-classes, and
// `focus()` walks into children when the component itself is not focusable.
//
// Components adopt elements from the shipped
// `assets/rml/menu.rml` already authored, because the menu's structure is content, not code — a
// game ships its own `menu.rml` and the framework must not hard-code Tomba!2's six tabs in C++.
// `adopt<T>(element, …)` makes that ownership transfer explicit.
//
// WHY PSEUDO-CLASS AND NOT `SetClass`. RmlUi's `class` attribute is authored content: the document
// says `<tab class="selected">` for the initially-selected tab, and a C++ `SetClass` writes into
// the same namespace the author is using. A pseudo-class is a separate channel the document cannot
// spell, so runtime state can never collide with an authored class and cannot be clobbered by a
// component that rewrites `class` for another reason.
//
// `:active` is an RmlUi built-in pseudo-class meaning "mouse is held down on this element". A
// component therefore may not express "this pane is
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
  // Adopt an authored element as a child component. `root` must be a descendant of this component's root; the returned
  // reference stays valid until this component is destroyed or clear_children() is called.
  // The framework builds as C++17, so the type constraint is a static_assert.
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
