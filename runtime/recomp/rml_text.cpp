#include "rml_text.h"

#include <RmlUi/Core/StringUtilities.h>

// Delegates to RmlUi's own encoder rather than re-deriving the escape table: it is the exact
// inverse of the `DecodeRml` the parser runs on the way in (both in
// vendor/rmlui/Source/Core/StringUtilities.cpp), so the round trip stays correct by construction if
// the vendored library ever changes. tests/test_rml_text_encoding.cpp asserts that round trip.
std::string rml_text_markup(std::string_view plain) {
    return Rml::StringUtilities::EncodeRml(Rml::String(plain));
}
