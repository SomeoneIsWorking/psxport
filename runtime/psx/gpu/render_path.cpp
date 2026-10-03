// render_path.cpp — resolve THE RENDER PATH for one Core, from configuration, in ONE place.
//
// WHY THIS IS ITS OWN FILE. The path has to be installed by every boot spine, and there is more than
// one: `native_boot_run` (native_boot.cpp) for the ports that boot through it, and the bootInit hook
// for the ports that do not — spyro reaches `main()` -> `dc_boot_init` -> hook and never touches
// native_boot_run (spyro C158). Before this existed, the flag was parsed inside native_boot_run, so
// spyro's own render_frame.cpp had to re-parse it and said so in a comment: *"The duplication is a
// framework wart: config parsing that belongs at Core setup lives inside one particular boot spine.
// Worth fixing upstream"*. This is that fix. Two parsers for one knob is two places for the knob's
// meaning to drift, and the ONLY reason the second one existed is that the first was in the wrong file.
//
// Renderer ownership and path selection are documented in docs/codemap.md.
#include "cfg.h"
#include "config_vars.h" // psx::config::render_path() — the CVar ladder
#include "core.h"
#include "game.h"
#include "game_runtime.h"
#include "render_capabilities.h"
#include "render_substrate.h"
#include <lucent/log.h>
#include <stdlib.h>

namespace {
// A live `render path ...` switch, and the Core it was addressed to. The CVar ladder's Runtime layer
// is PROCESS-global ("a REPL command. This run only."), but a render path is PER-CORE state: it lives
// in `Core::rsub.mode`, and a switch means "this Core, for the rest of this run". Writing only the
// ladder made the switch leak into every Core created afterwards, which in a process running several
// sessions at once meant the next title booted on the previous one's path — a Spyro 2 boot, whose
// declared path is the guest's, silently put Spyro 1's session on the guest renderer and into the
// reference leg that deliberately stops at the first frame driver call.
//
// So the Runtime slot still mirrors the switch (that is what the knob reports), and this remembers
// whom it was for: a Core installing its path adopts a switch addressed to ITSELF and otherwise
// reads the ladder with the Runtime layer excluded.
struct LiveSwitch {
  Core *core = nullptr;
  RenderPath path = RenderPath::Native;
};
LiveSwitch g_live_switch;
} // namespace

RenderPathSelectionResult render_path_apply(Game &game, RenderPath requested, RenderPathAudience audience) {
  if (!game.runtime) {
    return RenderPathSelectionResult::Unsupported;
  }

  const RenderCapabilities capabilities = game.runtime->renderCapabilities();
  const bool supported = audience == RenderPathAudience::Player ? capabilities.playerSelectable(requested)
                                                                : capabilities.supports(requested);
  if (!supported) {
    return RenderPathSelectionResult::Unsupported;
  }

  game.core.rsub.mode.setPath(requested);
  g_live_switch.core = &game.core;
  g_live_switch.path = requested;
  psx::config::cv_render_path.set(psx::config::Layer::Runtime, render_path_name(requested));
  return RenderPathSelectionResult::Applied;
}

void render_path_forget(const Core *core) {
  if (g_live_switch.core == core) {
    g_live_switch.core = nullptr;
  }
}

void render_path_install(Core *c) {
  const RenderCapabilities capabilities = c->runtime->renderCapabilities();
  // 1. The CVar: Default < Value (settings file) < Override (PSXPORT_RENDER_PATH) < Runtime (REPL) —
  // but only where the Runtime layer belongs to THIS Core. A live switch is addressed to one Core, so
  // for any other Core the ladder is read without it; otherwise the next session booted in this
  // process inherits the previous one's renderer.
  const bool switchIsForThisCore = g_live_switch.core == c;
  RenderPath p = switchIsForThisCore ? psx::config::render_path(capabilities.defaultPath)
                                     : psx::config::render_path_excluding_runtime(capabilities.defaultPath);

  const RenderPath requested = p;
  p = render_path_resolve(requested, capabilities);
  if (p != requested) {
    lucent::warn("render",
                 "render path '{}' is UNSUPPORTED by this title — using its declared '{}' path. "
                 "Supported: {}gte | psx.",
                 render_path_name(requested),
                 render_path_name(p),
                 capabilities.nativeRenderPath ? "native | " : "");
    // The effective path is recorded on this Core and printed on the line below, and the REPL's
    // `render path` reports it from there. It is deliberately NOT written back into the CVar ladder:
    // that slot is process-global, and one title's fallback there would become the next title's
    // request — in a process that runs several sessions, the wrong renderer for the wrong title.
  }

  c->rsub.mode.setPath(p);

  // ANNOUNCE IT, ALWAYS. Every capture, timing number and byte-compare from this run is a property of
  // this line; a measurement whose render path is unrecorded can be attributed to the wrong renderer,
  // which this workspace has already paid for twice (every headless timing number before psxport
  // 80e3d203, and the RENDER_PSX/ORACLE mixup). Not a debug channel — an info line on every run.
  lucent::info("render",
               "render path = {} — geometry from {}, rasterized by {}, PC enhancements {}",
               render_path_name(c->rsub.mode.path()),
               c->rsub.mode.psxRender() ? "the GUEST (its own GTE + ordering table)" : "PC-NATIVE producers",
               c->game->gpu.sw_path() ? "the PSX SOFTWARE rasterizer (s_vram)" : "the PC rasterizer (SDL_GPU)",
               c->rsub.mode.enhancementsAllowed() ? "ALLOWED" : "LOCKED OUT (the guest render stays pure)");
}
