---
id: C041
kind: claim
status: holds
created: 2026-08-26
tags:
depends: runtime/psx/render_capabilities.h#RenderCapabilities, runtime/psx/render_path.cpp#render_path_apply, runtime/psx/mods.cpp#Mods::init, runtime/ui/menu_pane.cpp#MenuPane, runtime/ui/render_path_control.cpp#RenderPathControl::cycle
---

## Claim

A GameRuntime's RenderCapabilities profile governs unsupported Native and temporal selections across startup, RmlUi, settings persistence, REPL, and debug-server controls

## Evidence

Clang full build plus CTest 105/105: test_game_runtime drives capable/incapable shipping bindings, live validator, reference lock, and widescreenOnly startup native->gte CVar rewrite; test_render_path_cycle drives both player/diagnostic capability cycles and MenuPane removal contract; test_ui_mod_row_model drives fail-closed availability, no-op unsupported mutation, stale fps60 omission, and capable retention

## What would falsify it

Any user-facing render-path or fps60 selection surface bypasses RenderCapabilities; a widescreenOnly runtime installs/selects Native, activates or persists fps60, or exposes either row; or an interpolatedNative/legacy runtime loses those options
