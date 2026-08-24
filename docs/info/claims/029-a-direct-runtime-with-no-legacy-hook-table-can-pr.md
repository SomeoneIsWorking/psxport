---
id: C029
kind: claim
status: holds
created: 2026-08-22
tags: runtime,presentation,hooks
depends: runtime/recomp/game_hooks_opt.cpp#game_render_fade_state, runtime/recomp/gpu_vk.cpp, tests/test_optional_hook_guards.cpp#test_fade_reader_handles_absent_table_and_calls_present_hook
reconfirmed: 2026-08-24
verified_at: 2026-08-24 19:59:56
---

## Claim

A direct runtime with no legacy GameHooks table can present the zero/no-fade state without a null dereference.

## Evidence

`test_optional_hook_guards` drives absent-table, absent-hook and present-hook answers through the
shipping accessor. Enter Electro supplies the real absent-table consumer that exposed the old direct
dereference before crt0.

## What would falsify it

A renderer call site dereferences `GameHooks` directly for fade state, an absent table does not return
zero/no-fade, or a present hook is swallowed or its values are rewritten.

## Re-confirmed 2026-08-22

Post-hook-fix Clang CTest 90/90 passed absent-table, absent-hook and present-hook fade controls through the shipping accessor; direct hook dereferences are confined to that owner.

## Re-confirmed 2026-08-24

Post-policy full 93/93 CTest passed optional-hook guards and complete renderer build; zero/no-fade behavior remains valid with no legacy hooks table
