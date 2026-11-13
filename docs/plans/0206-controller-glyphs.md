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
- Glyph art is vector and permissive: PromptFont (SIL OFL 1.1, Shinmera/promptfont v1.10), an outline font
  with Xbox, PlayStation, Nintendo and keyboard glyphs, so RmlUi draws it through FreeType and the MSDF path of
  0207 draws it in game. No vendor art from other games.
- Spyro (RE in the forks): Spyro 1 has no icon prompts, its text is mesh letters; Spyro 2 and 3 draw `<` `>`
  `{` `}` as the Cross, Square, Circle and Triangle cells of their one 2D font (`spyro2.text-font`,
  `spyro3.text-font`), the cells 0207 replaces.

## Phases

1. `glyph_set` and the port's own UI on Tomba 2 and Spyro: built (menu prompts, pad navigation, guide opens
   the menu); ships with the PromptFont asset once it is vendored.
2. Tomba 2 and Spyro in-game prompts, then each title as it is worked.
