# 0162: a picker panel showed a retained copy refreshed on a gate

Status: fixed

## Observed
"Spyro game select runs at 1 fps; the game reports a normal fps ~50 but visually it is 1 fps."

A picker panel was a slideshow. Measured 2026-10-17 on the built port
(`psx-spyro/scratch/picker-fps-probe/probe.py`, paced headless, all three panels settled as `demo`,
the composited window sampled at ~35 Hz through `picker shot`):

| run | panel guest presents | selector loop | visible changes in panel 0 | median interval |
|---|---|---|---|---|
| `fps60=1` (shipping settings) | 59.8 Hz | 30.0 Hz | 2.15 Hz | 0.40 s |
| `fps60=0` | — | 30.0 Hz | 1.10 Hz | 0.80 s |

Every counter the product printed was honest; none of them was the one the eye reads. The `fps60=0` row
is the literal "1 fps".

## Cause
`TitleSession::presentedPicture` sampled `lastFilledPresented` — the RETAINED copy — whenever a hold
existed, falling back to the live image only when no hold had ever been taken. So a settled panel's
picture was whatever copy `retainFilledPresentImage` last refreshed, behind two gates that were both
bounded in the wrong unit:

- `refreshPicture`'s `kProbeEverySteps = 12` is a boot gate ("a boot runs for thousands of steps whose
  first ones are black anyway") and also applied to a SETTLED panel. A settled panel is given exactly one
  step per picker frame (`PanelSessions::advance` breaks after step 0), so 12 steps is 12 WINDOW FRAMES:
  0.40 s at the measured 30 Hz loop.
- `kHoldRefreshPresents = 15` is a present gate, and one picker frame is one present. At `fps60=0` the
  12-step ask arrives carrying 12 presents, under 15, and is refused — the cadence doubles.

Halving presents per step doubled the visible interval exactly (0.40 → 0.80 s), which is what identifies
the second gate refusing the first gate's ask. Guest speed, host pacing and the build/pace/show split
were not involved: the loop measured 30.0 Hz in both runs. 0151's note on this ("once per 24 steps")
held only at 30 Hz presents and is where this was first half-observed.

## Resolution
A pane shows the LIVE present image: the panel's own present rebuilt it this frame and sampling it is
free. `GpuVkState::newestPresentedPicture` owns the choice — the live image, or the retained copy when the
newest present was probed blank (`s_present_blank_serial`). The three call sites that each restated the old
rule (`TitleSession::presentedPicture`, `PaneCompositor::composite`, `PickerComposite::build`'s diagnostic)
now ask the owner. `lastFilledPresented` is `retainedPicture`: the fallback, not the pane's picture.

The gates stay, with their stated job unchanged — they bound how fresh the FALLBACK is, which is the
sink-sized GPU→CPU→GPU round trip, not what the player sees.

After, same probe, same binary path:

| run | selector loop | visible changes in panel 0 | median interval |
|---|---|---|---|
| `fps60=1` | 30.0 Hz | 16.90 Hz | 0.03 s |
| `fps60=0` | 30.0 Hz | 16.90 Hz | 0.03 s |

The `fps60=0` row no longer differs, which is the gate no longer deciding what the player sees. A capture
of the settled selector shows all three panels holding their own title picture.

## Price, measured
A fade is noticed only when the probe gate opens, so up to 15 presents of a panel can show a fade or a
load screen. That is the same duration of wrongness the staleness it replaced had — but during a title's own
fade instead of always. `tests/test_pane_picture.cpp` asserts the bound rather than assuming it, and is
red (3 of its 5 cases) against the old rule.