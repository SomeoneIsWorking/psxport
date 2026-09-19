// picture_announce — the one-line, once-per-Core statement of the picture geometry a run RESOLVED.
//
// WHY IT EXISTS. An enhancement that silently fails to engage produces a run indistinguishable from
// one that engaged. Measured twice on 2026-09-19 while verifying widescreen through the state
// oracle: `tools/drive.py` overrides PSXPORT_SETTINGS from its own --settings flag, so a settings
// file naming aspect=1 never reached the product, and the run otherwise looked exactly like a pass.
// `aspect` alone does not answer the question either — widescreen also needs
// RenderMode::enhancementsAllowed() — which is why wide_engine is reported beside it.
//
// WHY IT IS THE FRAMEWORK'S. It lived in Spyro's game/render/render_frame.cpp until 2026-09-19, so
// every other title had no way to prove the same thing: Tomba! 2's widescreen oracle leg could show
// the settings file arriving and nothing at all about whether the picture actually widened.
//
// READ render_width, NOT wide_engine, TO ANSWER "DID IT WIDEN". `gpu_vk_wide_engine` asks only
// `aspect != ASPECT_4_3 && enhancementsAllowed()`, so ASPECT_AUTO reports wide_engine=1 while
// resolving to the sink's own aspect — measured 2026-09-19 on Tomba! 2, whose persisted settings
// carry aspect=3 (AUTO): its headless baseline printed `aspect=3 wide_engine=1 native_width=320
// render_width=320` and its aspect=1 leg printed `render_width=428`. Both fields are printed so the
// pair is decisive; neither is on its own.
//
// WHY PER-Core AND NOT PER-Game. One process holds a wide user core beside a 4:3 oracle core. The
// oracle core prints wide_engine=0 and is MEANT to; a per-Game flag would announce whichever core
// presented first and hide the difference that is the whole point.
//
// ON CHANGE, NOT ONCE. A once-per-run line reports whatever the FIRST present happened to be, which
// is a boot screen. Measured 2026-09-19: announcing once gave Spyro `native_width=320` because its
// first present is 320 wide, while the title-local line this replaced fired at the first NATIVE
// frame and said `native_width=512 render_width=684`. Both were true of the moment they sampled and
// neither described the run, so the geometry is reported every time it CHANGES. Changes are the
// interesting case, so they are never capped.
#pragma once

class Core;

namespace psx::picture {

// The four numbers that answer "what did this run actually render". Compared as a unit so a change
// in any one of them re-announces the whole tuple; the -1 initial state is what makes the FIRST
// present a change rather than a special case.
struct Geometry {
  int aspect = -1;
  int wideEngine = -1;
  int nativeWidth = -1;
  int renderWidth = -1;

  bool operator==(const Geometry &) const = default;
};

// Emits `[wide] native picture: aspect=... wide_engine=... native_width=... render_width=...`
// whenever this Core's picture geometry differs from the last line emitted for it, and nothing when
// it is unchanged. The previous tuple lives on `Core::rsub` (render_substrate.h) because it is
// per-Core host-only render state, like everything else there.
void announceOnChange(Core &core);

} // namespace psx::picture
