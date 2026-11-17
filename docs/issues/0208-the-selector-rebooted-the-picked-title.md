# 0208 — The selector rebooted the picked title and was not the Slant Stage

Status: fixed (2026-11-17). Owner: `runtime/psx/host/panel_sessions.cpp` (`PanelSessions`), with
`picker_session.cpp`, `title_session.cpp`, `product_host.cpp`.

## Observed
"none of the title selectors work as they should." Walked on the Ally (Spyro, Crash, Tomba, Spider-Man; hidden,
`ires=0`, `fps60=1`, sink 3840x2160):

- a pick rebooted the title from power-on, so the player sat through every publisher card again;
- a panel first focused showed black for its whole boot (Spyro 3: 679 fields);
- no title name anywhere; Crash's three panels were indistinguishable;
- `session return` rebooted every panel;
- the confirm press reached the rebooted title (Spyro 1's first presentation hold ended on it);
- booting a panel wiped the previous panel's SPU.

## Cause
- `PanelSessions::confirm` destroyed every session and returned a new unbooted one, and
  `ProductHost::runSelector` rebuilt the whole selector on a return.
- `PanelSessions::advance` booted a panel only on its first focus, and `PickerPanes` drew nothing for a panel
  without a picture.
- A new `Pad` has no edge history, so a button held across the confirm was a fresh press.
- `title_session.cpp:bindPerCoreDevices` ran `SPU_Init`/`SPU_Power` on the SPU state still bound from the
  previous instance before binding its own.

## Fix
The selector is the Slant Stage (`title-host.md`, "The selector"): every title is booted when the selector
starts. Two sides are shown, with a ring for three titles or more. Each side shows a plain tile under its
logo until it has a frame. A bottom band carries the titles and the hint line. A pick slides the edge out and
hands the running instance over (`TitleSession::handOver`), and a return hands it back (`takeBack`).
`Pad::setPlayerInputSuppressed` hides a press held across the hand-over until it is released. Each instance's
SPU powers on at its own first bind. Logs: `picker: on <title>`, `picker: picked <title>`.
Every unfocused title runs unseen to its stage picture (its title screen where the title names its logo) before
the selector's first frame, and stays loaded through play, so no side is ever empty. A held side is pure
greyscale. Each title's existing Start skips act in preview (Spyro 1/2/3, Spider-Man 1; Tomba already did).
