---
id: 28
title: Unsupported titles expose Native and temporal renderer controls
status: resolved
symptom: A widescreen-only GameRuntime can reject Native at startup yet the shared F1 menu, persisted fps60 setting, REPL, or debug server can still select or display unsupported Native/60fps options
tags: runtime,rendering,capabilities,ui,config,architecture
created: 2026-08-26
updated: 2026-08-26
---

## Root cause

`RenderPath` and `Fps60` were framework-wide implementations with no title-owned declaration of
whether a particular `GameRuntime` supplied their required producers/history. Startup checks could be
implemented game-side, but the shared RmlUi control, settings loader, REPL, and debug server each
selected the global implementation independently. A neutral temporal presenter made fps60 inert; it
did not remove or refuse the option.

## What was tried / dead ends

Game-local copies of `menu.rml` can hide rows, and game-local startup parsing can reject Native, but
both duplicate shared policy and leave the other live selection surfaces unchanged. The capability
must be one typed runtime fact consumed by every shared surface.

## Resolution

`GameRuntime::renderCapabilities()` is now required. `RenderCapabilities` owns the supported/default
render paths, player-selectable subset, and temporal-interpolation declaration. The legacy adapter
explicitly retains native+temporal behavior; direct runtimes must declare their profile.

Startup resolution, RmlUi, REPL, and the debug server consume the same support policy. Unsupported
Native requests are named and resolve to the title default; the live CVar is rewritten to that
effective answer. `Mods` fails closed before initialization, refuses unsupported saved/environment
fps60 requests, omits the key on the next save, and makes the fps60 binding unavailable. `MenuPane`
removes unavailable bindings from both layout and navigation rather than leaving inert rows.

### Resolution (2026-08-26)
Root cause was framework-global render/Fps60 implementations selected independently by startup, UI, settings, REPL, and debug server with no required title capability declaration. Added required typed RenderCapabilities and routed all shared selection/visibility/persistence through it; full Clang build and CTest 105/105 pass with capable/incapable controls.
