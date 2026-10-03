// rml_text — the DATA -> MARKUP boundary for anything the overlay puts into the RmlUi DOM.
//
// WHY THIS EXISTS. `Rml::Element::SetInnerRML` parses its argument as MARKUP. Overlay readouts are
// DATA — numbers, a stage name, a track title — and handing data to a markup parser is a category
// error with two visible consequences, both of which were shipping:
//
//   * A separator written as `&middot;` rendered as the literal eight characters "&middot;",
//     because RmlUi decodes only the four XML predefined named entities (`&lt; &gt; &amp; &quot;`)
//     plus numeric character references `&#NNN;` / `&#xHH;`. HTML4 names are not in
//     `StringUtilities::DecodeRml`, and `ElementText::BuildToken`'s second decoder knows only
//     lt/gt/amp/quot/nbsp. That is upstream RmlUi's design (RML is XML-ish, not HTML), so the fix
//     is on our side of the boundary, not in the vendored library.
//
//   * The overlay's "only rewrite when it changed" guard silently never fired for such a string.
//     `Element::GetInnerRML()` returns `EncodeRml(text)`, so comparing it against a RAW markup
//     string containing `&`, `<`, `>` or `"` is always unequal — the element was reparsed and
//     relaid-out every frame.
//
// Both vanish once data enters as data. Encode at the boundary and it is impossible for a value to
// be read as markup, and the DOM round trip is stable so the change guard works.
//
// PLAIN TEXT IS UTF-8. RmlUi is UTF-8 native, so a separator is simply the U+00B7 character in the
// string — no entity, nothing to decode. Use RML_TEXT_MIDDLE_DOT rather than spelling the bytes.
#ifndef PSXPORT_RML_TEXT_H
#define PSXPORT_RML_TEXT_H

#include <string>
#include <string_view>

// U+00B7 MIDDLE DOT, as UTF-8, with the spaces the overlay separates fields with. Named because the
// raw bytes in a format string are the thing nobody can read or grep for.
#define RML_TEXT_MIDDLE_DOT "\xc2\xb7"
#define RML_TEXT_SEP " " RML_TEXT_MIDDLE_DOT " "

// Turn PLAIN TEXT into RML markup that renders exactly that text. The inverse of what RmlUi's
// parser does, so `DecodeRml(rml_text_markup(t)) == t` for every t — which is the property the
// overlay's change guard depends on. Non-ASCII UTF-8 passes through untouched.
std::string rml_text_markup(std::string_view plain);

#endif
