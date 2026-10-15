#include "rml_text.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/StringUtilities.h>
#include <RmlUi/Core/Variant.h>

// Delegates to RmlUi's own encoder rather than re-deriving the escape table: it is the exact
// inverse of the `DecodeRml` the parser runs on the way in (both in
// vendor/rmlui/Source/Core/StringUtilities.cpp), so the round trip stays correct by construction if
// the vendored library ever changes. tests/test_rml_text_encoding.cpp asserts that round trip.
std::string psx::ui::rmlTextMarkup(std::string_view plain) {
  return Rml::StringUtilities::EncodeRml(Rml::String(plain));
}

// Reads the stored string without Variant's any-type conversion table.
std::string psx::ui::rmlAttributeText(const Rml::Element &element, const std::string &name, std::string_view fallback) {
  const Rml::Variant *value = element.GetAttribute(name);
  if (value == nullptr || value->GetType() != Rml::Variant::STRING) {
    return std::string(fallback);
  }
  return value->GetReference<Rml::String>();
}
