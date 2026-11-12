---
id: 0201
title: Hot reload of title code
status: planned (not started)
created: 2026-11-12
---

# Hot reload of title code

## Request

The user, 2026-11-12: "I even thought about like 'what if I make a C++ interpreter' to cut down on
build resources". Offered hot reload instead, they asked "is this feasible?", then "Write a plan for
this but don't start it yet", and wanted both a plan for all C++ projects and one for psxport.

The shared mechanism (Lucent `lucent::reload`, the CMake split, the layout guard, host rules) is
re-harness `docs/plans/0001-cpp-hot-reload.md`. This plan is psxport's part: its safe point, what a
reload replaces and invalidates, and its fallback. Start after the build-footprint work lands
(line-tables debug info, lld, shared dependency prefix, one tree per purpose).

## Goal

In a maintainer build, a title's overrides and game logic are one reloadable library. An edit
rebuilds only that library, and the running game picks it up at the next frame boundary, on the
same frame. Player builds stay one static executable.

## psxport specifics

- **Split.** The title's `GameRuntime`, overrides, behaviour handlers and `TitleDebugMenu` build as
  the module. The framework (`runtime/`) and vendored libraries stay in the executable. The
  module's one entry point returns the title's runtime and registers its overrides through
  `runtime/cpu/native_dispatch.*`, the one entry for registration.
- **Safe point.** The swap runs only between logic frames, when the frame driver has returned and
  no override or guest call is on the stack. The `Core` executor contract already commits all
  guest state there.
- **The swap:**
  1. replace the image-scoped override table;
  2. advance the dispatch epoch (`runtime/cpu/dispatch_epoch.h`);
  3. invalidate every Lightrec path that captured an old dispatch decision, through
     `runtime/cpu/invalidation.*`;
  4. rebind the frame driver and debug menu.
- **State.**
  - Guest RAM, CPU, GTE and devices are runtime-owned and survive a reload.
  - Title host-side state, such as presentation producers, interpolation tracks, menu state and
    title settings, moves to framework-owned storage the title gets by handle, or is rebuilt from
    guest state. Tomba 2's owners are audited first.
- **Fallback.**
  - If the layout guard refuses a swap, the run snapshots through `runtime/cpu/machine_snapshot.*`
    plus device state.
  - It then restarts on the new build and restores the snapshot through the load-state path in
    `runtime/psx/host/title_session.*`.
- **Trigger.** A `reload` verb on the existing control channel, plus the Lucent file watch.

## Phases

1. Tomba 2 builds as a module in the maintainer configure and loads at startup; `tomba2_port`'s
   player build is unchanged.
2. Swap at the frame boundary. Proof: change one Tomba 2 override while the game runs, rebuild the
   module, and the next frame shows the change.
3. Tomba 2's host-side state moved out of the module. Proof: reload with fps60 interpolation and
   the debug menu active.
4. Layout guard and snapshot fallback. Proof: change a `runtime/psx/game/` header and land on the
   same frame after the automatic restart.
5. Spyro, then every title.

## Progress

Not started.
