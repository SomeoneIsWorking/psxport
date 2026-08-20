#include "ui_event.h"

#include <utility>

namespace psx::ui {

ScopedEventListener::ScopedEventListener(Rml::Element *element, Rml::EventId event, Callback callback, bool capture)
    : mElement(element), mEvent(event), mCapture(capture), mCallback(std::move(callback)) {
  if (mElement) {
    mElement->AddEventListener(mEvent, this, mCapture);
  }
}

ScopedEventListener::~ScopedEventListener() {
  if (mElement) {
    mElement->RemoveEventListener(mEvent, this, mCapture);
    mElement = nullptr;
  }
}

void ScopedEventListener::ProcessEvent(Rml::Event &event) {
  if (mCallback) {
    mCallback(event);
  }
}

void ScopedEventListener::OnDetach(Rml::Element *element) {
  if (element == mElement) {
    mElement = nullptr;
  }
}

} // namespace psx::ui
