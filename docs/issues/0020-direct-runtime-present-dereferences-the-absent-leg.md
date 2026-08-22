---
id: 20
title: Direct-runtime present dereferences the absent legacy hook table
status: resolved
symptom: Enter Electro crashes before crt0 when the boot clear reaches GpuVkState::present with no legacy GameHooks table
tags: runtime,presentation,hooks,multi-title,spider
created: 2026-08-22
updated: 2026-08-22
---

Root cause: `GpuVkState::present` duplicated the optional fade-hook read instead of using the guarded
fade-state owner. It checked `hooks->renderFadeState` but dereferenced `hooks` first. Legacy consumers
always install a table, so only a genuinely direct runtime exposed the null-table case.

Resolved by moving the fade read into `game_render_fade_state`, alongside the other optional-hook
accessors, and routing every Vulkan fade consumer through it. The absent-table and absent-hook cases
return the zero/no-fade state; the present-hook control proves the callback and its exact values still
reach the renderer. A direct runtime no longer needs a fabricated empty compatibility table.
