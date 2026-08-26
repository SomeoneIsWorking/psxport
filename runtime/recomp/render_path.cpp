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
// Design + the decisions this encodes: docs/plans/render-path-tristate.md.
#include "cfg.h"
#include "config_vars.h" // psx::config::render_path() — the CVar ladder
#include "core.h"
#include "game.h"
#include "game_runtime.h"
#include "render_capabilities.h"
#include "render_substrate.h"
#include <lucent/log.h>
#include <stdlib.h>

RenderPathSelectionResult render_path_apply(Game &game, RenderPath requested, RenderPathAudience audience) {
  if (game.oracle || game.sbs) {
    return RenderPathSelectionResult::ReferenceLocked;
  }
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
  psx::config::cv_render_path.set(psx::config::Layer::Runtime, render_path_name(requested));
  return RenderPathSelectionResult::Applied;
}

void render_path_install(Core *c) {
  const RenderCapabilities capabilities = c->runtime->renderCapabilities();
  // 1. The CVar: Default < Value (settings file) < Override (PSXPORT_RENDER_PATH) < Runtime (REPL).
  RenderPath p = psx::config::render_path(capabilities.defaultPath);

  // 2. PSXPORT_RENDER_PSX — COMPATIBILITY ALIAS, and it does NOT mean what it used to. It selected the
  //    guest's geometry with the PC enhancements STILL LIVE; that configuration is exactly what
  //    Tomba2 kanban #78 warns people not to mistake for a reference, and it no longer exists (USER
  //    2026-08-11: "GTE/OT should stay pure"). It maps to `gte` and WARNS — a flag whose meaning
  //    changed silently is how the headless-pacing class of bug happened.
  if (const char *r = cfg_str("PSXPORT_RENDER_PSX")) {
    if (*r) {
      p = (atoi(r) != 0) ? RenderPath::Gte : RenderPath::Native;
      lucent::warn("render",
                   "PSXPORT_RENDER_PSX={} is a DEPRECATED alias -> render path '{}'. It no longer keeps "
                   "the PC enhancements live (fps60 / widescreen / ires / deferred are native-only now). "
                   "Use PSXPORT_RENDER_PATH={}.",
                   r,
                   render_path_name(p),
                   render_path_name(p));
    }
  }

  // 3. PSXPORT_ORACLE is the BUNDLE (substrate gameplay + a pure picture) and outranks both: a run that
  //    asked for the reference must get the reference, not whatever the settings file had persisted.
  if (oracle_mode()) {
    p = RenderPath::Gte;
  }

  const RenderPath requested = p;
  p = render_path_resolve(requested, capabilities);
  if (p != requested) {
    lucent::warn("render",
                 "render path '{}' is UNSUPPORTED by this title — using its declared '{}' path. "
                 "Supported: {}gte | psx.",
                 render_path_name(requested),
                 render_path_name(p),
                 capabilities.nativeRenderPath ? "native | " : "");
    // Reflect the effective answer at the highest live layer. Otherwise `cvars` would keep reporting
    // the unsupported persisted/environment request even though the running Core uses the title's
    // declared path, which is the same misleading half-application this capability boundary removes.
    psx::config::cv_render_path.set(psx::config::Layer::Runtime, render_path_name(p));
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
