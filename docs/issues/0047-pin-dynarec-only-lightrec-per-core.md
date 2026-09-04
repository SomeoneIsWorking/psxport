---
id: 47
title: Pin a dynarec-only Lightrec product dependency per Core
status: open
symptom: psxport has no product Lightrec executor and its available Lightrec copy includes interpreter fallback behavior
tags: lightrec,dynarec,dependency,core,product
created: 2026-09-04
updated: 2026-09-04
---
state_items: S012, S016

## Root cause

Lightrec is currently present only indirectly under the Beetle hardware-backend submodule. That does
not give psxport a direct immutable product pin, a maintained integration boundary, or a build that
excludes Lightrec's difficult-block and threaded-compilation interpreter fallbacks. The existing
Beetle integration also exposes process-global Lightrec state, while psxport requires one executor per
live `Core`.

## Required outcome

Add a direct dependency on an exact maintained Lightrec revision. If upstream cannot produce a
dynarec-only library, put the required build/API change in a maintained fork and pin that commit; do
not add a patch file. The gameplay library must link only the dynarec product configuration. Every
`Core` owns and tears down its own Lightrec state and callback context. Unsupported compilation must
return a named executor failure, never enter an interpreter.

The discriminator must create two cores with different register/memory state, execute nonzero
translated blocks on both, and prove their Lightrec state, callbacks, counters, caches, and teardown
do not cross. Link inspection must name every gameplay object scanned and find zero interpreter
objects/symbols.
