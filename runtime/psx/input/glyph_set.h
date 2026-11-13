// glyph_set.h — psx::input::GlyphSet: the button glyphs of the pad in the player's hand.
//
// The device the player last pressed on names the family: the keyboard, or an SDL gamepad whose type picks
// Xbox, PlayStation or Nintendo art (an unrecognised pad reads as Xbox, SDL's own positional naming). Each PSX
// button maps to that family's glyph through the binding HostInput reads the device with, so a prompt shows the
// control that presses the button. Glyphs are PromptFont codepoints (assets/rml/promptfont.ttf, SIL OFL 1.1).
// HostInput owns the one instance; nothing else reads the pad type.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace psx::input {

enum class PadFamily : std::uint8_t { Keyboard, Xbox, PlayStation, Nintendo };

std::string_view padFamilyName(PadFamily family);

class GlyphSet {
public:
  PadFamily family() const {
    return family_;
  }
  // Bumped whenever the family changes, so a UI rebuilds its prompts only then.
  std::uint32_t generation() const {
    return generation_;
  }
  // The glyph, UTF-8, of one PSX button bit (kButton*, active high) for the active family; empty for a bit
  // that is not one button.
  std::string glyph(std::uint16_t button) const;

  // A key went down: the keyboard's glyphs.
  void noteKeyboard();
  // A gamepad of SDL_GamepadType `sdlType` was pressed or opened.
  void noteGamepad(int sdlType);

private:
  void setFamily(PadFamily family, int sdlType);

  PadFamily family_ = PadFamily::Keyboard;
  int sdlType_ = 0; // SDL_GamepadType of the last gamepad, which labels its face buttons
  std::uint32_t generation_ = 0;
};

} // namespace psx::input
