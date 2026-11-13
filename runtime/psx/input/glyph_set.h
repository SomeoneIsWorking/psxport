// glyph_set.h — psx::input::GlyphSet: the button glyphs of the pad in the player's hand.
//
// The device the player last pressed on names the family: the keyboard, or an SDL gamepad whose type picks
// Xbox, PlayStation or Nintendo art (an unrecognised pad reads as Xbox, SDL's own positional naming). Each PSX
// button maps to that family's glyph through the binding HostInput reads the device with, so a prompt shows the
// control that presses the button, and in the family's face colour where it prints one. Glyphs are PromptFont
// codepoints (assets/rml/promptfont.ttf, SIL OFL 1.1).
// HostInput owns the one instance; nothing else reads the pad type.
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace psx::input {

enum class PadFamily : std::uint8_t { Keyboard, Xbox, PlayStation, Nintendo };

inline constexpr char32_t kNoGlyph = 0;

struct FaceColour {
  std::uint8_t red = 0;
  std::uint8_t green = 0;
  std::uint8_t blue = 0;
};

// The Xbox face buttons' colours; PlayStation and Nintendo prompts keep the game's own.
inline constexpr FaceColour kXboxA{.red = 0x5C, .green = 0xB8, .blue = 0x3C};
inline constexpr FaceColour kXboxB{.red = 0xE0, .green = 0x3C, .blue = 0x32};
inline constexpr FaceColour kXboxX{.red = 0x2E, .green = 0x6E, .blue = 0xE0};
inline constexpr FaceColour kXboxY{.red = 0xF0, .green = 0xC0, .blue = 0x1E};

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
  // The PromptFont codepoint of one PSX button bit (kButton*, active high) for the active family; kNoGlyph
  // for a bit that is not one bound button.
  char32_t glyph(std::uint16_t button) const;
  // The colour the active family prints one PSX button's face in, none where the game's own colour stands.
  std::optional<FaceColour> faceColour(std::uint16_t button) const;

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
