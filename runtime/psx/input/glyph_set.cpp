// glyph_set.cpp — psx::input::GlyphSet: the active pad family and each PSX button's PromptFont glyph.
#include "glyph_set.h"
#include "host_input.h"

#include <lucent/log.h>

#ifdef PSXPORT_SDL
#include "input_binding.h"

#include <SDL3/SDL_gamepad.h>
#endif

#include <optional>

namespace psx::input {
namespace {

#ifdef PSXPORT_SDL

// PromptFont's glyphs for the controls a family names in its own way.
struct FamilyGlyphs {
  PadFamily family;
  char32_t l1, r1, l2, r2, start, select;
};
constexpr FamilyGlyphs kXbox{.family = PadFamily::Xbox,
                             .l1 = 0x2198,
                             .r1 = 0x2199,
                             .l2 = 0x2196,
                             .r2 = 0x2197,
                             .start = 0x21FB,
                             .select = 0x21FA};
constexpr FamilyGlyphs kPlayStation3{.family = PadFamily::PlayStation,
                                     .l1 = 0x21B0,
                                     .r1 = 0x21B1,
                                     .l2 = 0x21B2,
                                     .r2 = 0x21B3,
                                     .start = 0x21F8,
                                     .select = 0x21F7};
constexpr FamilyGlyphs kPlayStation4{.family = PadFamily::PlayStation,
                                     .l1 = 0x21B0,
                                     .r1 = 0x21B1,
                                     .l2 = 0x21B2,
                                     .r2 = 0x21B3,
                                     .start = 0x21E8,
                                     .select = 0x21E6};
constexpr FamilyGlyphs kPlayStation5{.family = PadFamily::PlayStation,
                                     .l1 = 0x21B0,
                                     .r1 = 0x21B1,
                                     .l2 = 0x21B2,
                                     .r2 = 0x21B3,
                                     .start = 0x2208,
                                     .select = 0x2206};
constexpr FamilyGlyphs kNintendo{.family = PadFamily::Nintendo,
                                 .l1 = 0x219C,
                                 .r1 = 0x219D,
                                 .l2 = 0x219A,
                                 .r2 = 0x219B,
                                 .start = 0x21FE,
                                 .select = 0x21FD};

const FamilyGlyphs &familyGlyphs(SDL_GamepadType type) {
  switch (type) {
  case SDL_GAMEPAD_TYPE_PS3:
    return kPlayStation3;
  case SDL_GAMEPAD_TYPE_PS4:
    return kPlayStation4;
  case SDL_GAMEPAD_TYPE_PS5:
    return kPlayStation5;
  case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
  case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
  case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
  case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
    return kNintendo;
  default:
    return kXbox;
  }
}

// The face button's printed label, which SDL knows per pad type.
std::optional<char32_t> faceGlyph(SDL_GamepadType type, SDL_GamepadButton button) {
  switch (SDL_GetGamepadButtonLabelForType(type, button)) {
  case SDL_GAMEPAD_BUTTON_LABEL_A:
    return 0x21D3;
  case SDL_GAMEPAD_BUTTON_LABEL_B:
    return 0x21D2;
  case SDL_GAMEPAD_BUTTON_LABEL_X:
    return 0x21D0;
  case SDL_GAMEPAD_BUTTON_LABEL_Y:
    return 0x21D1;
  case SDL_GAMEPAD_BUTTON_LABEL_CROSS:
    return 0x21E3;
  case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE:
    return 0x21E2;
  case SDL_GAMEPAD_BUTTON_LABEL_SQUARE:
    return 0x21E0;
  case SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE:
    return 0x21E1;
  default:
    return std::nullopt;
  }
}

std::optional<char32_t> gamepadGlyph(SDL_GamepadType type, std::uint16_t bit) {
  const std::optional<GamepadBinding> binding = gamepadBinding(bit);
  if (!binding) {
    return std::nullopt;
  }
  const FamilyGlyphs &glyphs = familyGlyphs(type);
  switch (binding->button) {
  case SDL_GAMEPAD_BUTTON_SOUTH:
  case SDL_GAMEPAD_BUTTON_EAST:
  case SDL_GAMEPAD_BUTTON_WEST:
  case SDL_GAMEPAD_BUTTON_NORTH:
    return faceGlyph(type, binding->button);
  case SDL_GAMEPAD_BUTTON_DPAD_UP:
    return 0x219F;
  case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
    return 0x21A0;
  case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
    return 0x21A1;
  case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
    return 0x219E;
  case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
    return glyphs.l1;
  case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
    return glyphs.r1;
  case SDL_GAMEPAD_BUTTON_START:
    return glyphs.start;
  case SDL_GAMEPAD_BUTTON_BACK:
    return glyphs.select;
  case SDL_GAMEPAD_BUTTON_LEFT_STICK:
    return 0x21BA;
  case SDL_GAMEPAD_BUTTON_RIGHT_STICK:
    return 0x21BB;
  default:
    break;
  }
  return binding->trigger == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? glyphs.l2 : glyphs.r2;
}

std::optional<char32_t> keyGlyph(SDL_Scancode key) {
  if (key >= SDL_SCANCODE_A && key <= SDL_SCANCODE_Z) {
    return 0xFF21 + static_cast<char32_t>(key - SDL_SCANCODE_A);
  }
  if (key >= SDL_SCANCODE_1 && key <= SDL_SCANCODE_9) {
    return 0xFF11 + static_cast<char32_t>(key - SDL_SCANCODE_1);
  }
  switch (key) {
  case SDL_SCANCODE_UP:
    return 0x23F6;
  case SDL_SCANCODE_RIGHT:
    return 0x23F5;
  case SDL_SCANCODE_DOWN:
    return 0x23F7;
  case SDL_SCANCODE_LEFT:
    return 0x23F4;
  case SDL_SCANCODE_RETURN:
    return 0x242E;
  case SDL_SCANCODE_RSHIFT:
    return 0x2450;
  default:
    return std::nullopt;
  }
}

std::optional<char32_t> keyboardGlyph(std::uint16_t bit) {
  for (const KeyBinding &binding : kKeyboardBindings) {
    if (binding.bit == bit) {
      return keyGlyph(binding.key);
    }
  }
  return std::nullopt;
}

#endif

} // namespace

std::string_view padFamilyName(PadFamily family) {
  switch (family) {
  case PadFamily::Keyboard:
    return "keyboard";
  case PadFamily::Xbox:
    return "xbox";
  case PadFamily::PlayStation:
    return "playstation";
  case PadFamily::Nintendo:
    return "nintendo";
  }
  return "keyboard";
}

char32_t GlyphSet::glyph(std::uint16_t button) const {
#ifdef PSXPORT_SDL
  const std::optional<char32_t> codepoint = family_ == PadFamily::Keyboard
                                                ? keyboardGlyph(button)
                                                : gamepadGlyph(static_cast<SDL_GamepadType>(sdlType_), button);
  return codepoint.value_or(kNoGlyph);
#else
  (void)button;
  return kNoGlyph;
#endif
}

void GlyphSet::noteKeyboard() {
  setFamily(PadFamily::Keyboard, sdlType_);
}

void GlyphSet::noteGamepad(int sdlType) {
#ifdef PSXPORT_SDL
  setFamily(familyGlyphs(static_cast<SDL_GamepadType>(sdlType)).family, sdlType);
#else
  (void)sdlType;
#endif
}

void GlyphSet::setFamily(PadFamily family, int sdlType) {
  if (family == family_ && sdlType == sdlType_) {
    return;
  }
  family_ = family;
  sdlType_ = sdlType;
  ++generation_;
  lucent::info("input", "button prompts show the {} glyphs", padFamilyName(family));
}

} // namespace psx::input
