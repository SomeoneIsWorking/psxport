#include "ui_component.h"

#include "rml_text.h"   // rml_text_markup() — the DATA -> markup encoder

#include <RmlUi/Core/StringUtilities.h>
#include <lucent/log.h>

#include <string>

namespace psx::ui {

void set_text(Rml::Element* el, std::string_view text) {
    if (!el) return;
    const std::string markup = rml_text_markup(text);
    if (el->GetInnerRML() == markup) return;
    el->SetInnerRML(markup);
    // Report what the DOM NOW HOLDS, not what we sent — DecodeRml(GetInnerRML()) is literally the
    // character sequence RmlUi will hand to the font. That distinction is the whole point: the
    // "&middot;" bug was invisible to any diagnostic that echoed the string we composed, because
    // the string we composed was fine and the DOM's interpretation of it was not. Channel `rmlui`.
    const Rml::String& id = el->GetId();
    lucent::debug("rmlui", "text {} = \"{}\"", id.empty() ? el->GetTagName() : "#" + id,
                  Rml::StringUtilities::DecodeRml(el->GetInnerRML()));
}

Component::~Component() = default;

void Component::update() {
    for (const auto& child : mChildren) child->update();
}

bool Component::focus() {
    if (disabled()) return false;
    if (mRoot && mRoot->Focus(true)) {
        // Nearest alignment, not Dusklight's Center: our panes are short scrolling lists and
        // centring every focus move would scroll the list under a user who only pressed Down once.
        mRoot->ScrollIntoView(Rml::ScrollIntoViewOptions(Rml::ScrollAlignment::Nearest));
        return true;
    }
    for (const auto& child : mChildren)
        if (child->focus()) return true;
    return false;
}

void Component::set_selected(bool value) {
    // Subclasses may override selected() to compute a dynamic answer, but the only question here is
    // whether the pseudo-class is set, so call the base directly rather than the virtual.
    if (!mRoot || Component::selected() == value) return;
    mRoot->SetPseudoClass("selected", value);
}

void Component::set_disabled(bool value) {
    if (!mRoot || Component::disabled() == value) return;
    if (value) {
        mRoot->SetAttribute("disabled", "");
        mRoot->SetPseudoClass("disabled", true);
        mRoot->Blur();
    } else {
        mRoot->RemoveAttribute("disabled");
        mRoot->SetPseudoClass("disabled", false);
    }
}

void Component::listen(Rml::Element* element, Rml::EventId event,
                       ScopedEventListener::Callback callback, bool capture) {
    if (!element) element = mRoot;
    if (!element) return;
    mListeners.emplace_back(
        std::make_unique<ScopedEventListener>(element, event, std::move(callback), capture));
}

bool Component::contains(Rml::Element* element) const {
    for (const Rml::Element* node = element; node; node = node->GetParentNode())
        if (node == mRoot) return true;
    return false;
}

}  // namespace psx::ui
