---
id: C040
kind: claim
status: holds
created: 2026-08-26
tags: runtime,architecture,platform-hle,projection
depends: runtime/psx/platform_hle.h#PlatformHlePlan, runtime/psx/sync_overrides.cpp#PlatformHle::initBuiltins, tests/test_platform_hle_direct_runtime.cpp#test_direct_runtime_plan_binds_standard_libgte_projection_leaves
---

## Claim

A direct PlatformHlePlan binds framework-owned libgte SetGeomOffset and SetGeomScreen handlers from typed guest addresses under the same declared-window guard as legacy consumers.

## Evidence

Clang test_platform_hle_direct_runtime: 6/6 cases, 33 checks, 0 failed; the production lookup invoked both private framework handlers, observed SetGeomScreen preserve a0 and record H, observed SetGeomOffset shift a0/a1 and complete ProjParams, and the out-of-window SetGeomScreen opposite control was REFUSED. Full Clang CTest passed 105/105 including cpp_style.

## What would falsify it

Falsified if either typed direct-plan address does not resolve the framework handler, an out-of-window typed address registers, the handler register effects/projection record diverge, or the legacy mapping stops sharing the same registration owner.
