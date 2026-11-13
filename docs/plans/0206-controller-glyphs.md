# 0206 — Controller glyphs (Xbox and PlayStation)

User, 2026-11-13: "Also probably should add xbox glyphs and svg fonts".

## What

Button prompts match the pad in the player's hand: Xbox A/B/X/Y and its shoulders and triggers on an
Xbox-layout pad, the PlayStation shapes on a PlayStation pad, the keyboard key on the keyboard. This
covers the port's own UI (picker, settings, bug report) and the game's own prompts (a ✕ or ○ drawn in
a dialogue, a "press START" card).

## Design

- One framework owner, `runtime/psx/input/glyph_set`, knows the active pad family (SDL gamepad type)
  and maps each PSX button to that family's glyph. The UI asks it; nothing else reads the pad type.
- Port UI: RmlUi draws the glyph from the active set; prompts in `.rml` name the PSX button, not an
  image.
- Game prompts are title-owned: RE each title's button-icon draws (its font cells or sprite records
  for ✕ ○ △ □ and the shoulder buttons) and swap the drawn cell for the active family's glyph at
  present time, at sink resolution. Guest memory and the guest's texture page stay untouched.
- Glyph art is vector and permissive: Kenney's Input Prompts (CC0) or our own drawn outlines. No
  vendor art from other games.

## Phases

1. `glyph_set` and the port's own UI on Tomba 2 and Spyro.
2. Tomba 2 and Spyro in-game prompts, then each title as it is worked.
