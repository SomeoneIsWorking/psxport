// psx::ui::ScopedEventListener — an RmlUi event listener whose registration is its lifetime.
//
// SHAPE TAKEN FROM DUSKLIGHT (CC0), `src/dusk/ui/event.{hpp,cpp}`. Adapted only in namespace and
// naming convention; the mechanism — register in the constructor, deregister in the destructor,
// and null the element in OnDetach so a listener that outlives its element is inert rather than
// dangling — is theirs and is the whole reason it is worth taking.
//
// WHAT IT REPLACES, and why that was a real hazard rather than a style nit: the overlay used to
// hold `void* mTabListeners` (a heap `std::vector<std::unique_ptr<TabClick>>`) and `void*
// mRowListener` (a heap `RowClick`), `AddEventListener` them onto elements, and then `delete` the
// vectors in `shutdown()` — AFTER `Rml::Shutdown()` had already destroyed every document and
// element. Nothing ever called `RemoveEventListener`. That is only survivable because RmlUi tears
// the elements down first; reverse the two lines, or reload the document without a full shutdown,
// and the elements hold pointers to freed listeners. Ownership expressed as a `void*` plus a
// hand-written teardown ordering is exactly the class of bug RAII exists to delete.
#ifndef PSXPORT_UI_EVENT_H
#define PSXPORT_UI_EVENT_H

#include <RmlUi/Core.h>

#include <functional>

namespace psx::ui {

class ScopedEventListener final : public Rml::EventListener {
public:
    using Callback = std::function<void(Rml::Event&)>;

    ScopedEventListener(Rml::Element* element, Rml::EventId event, Callback callback,
                        bool capture = false);
    ~ScopedEventListener() override;

    ScopedEventListener(const ScopedEventListener&)            = delete;
    ScopedEventListener& operator=(const ScopedEventListener&) = delete;
    ScopedEventListener(ScopedEventListener&&)                 = delete;
    ScopedEventListener& operator=(ScopedEventListener&&)      = delete;

    void ProcessEvent(Rml::Event& event) override;
    // RmlUi calls this when the element we registered on is destroyed. Forgetting the pointer here
    // is what makes destruction-after-the-document safe: the destructor then has nothing to
    // deregister from, instead of calling into freed memory.
    void OnDetach(Rml::Element* element) override;

private:
    Rml::Element*  mElement = nullptr;
    Rml::EventId   mEvent   = Rml::EventId::Invalid;
    bool           mCapture = false;
    Callback       mCallback;
};

}  // namespace psx::ui

#endif
