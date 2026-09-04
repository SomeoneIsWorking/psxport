---
id: 47
title: Pin a dynarec-default Lightrec product dependency per Core
status: open
symptom: the pinned Lightrec backend cannot yet own multiple live machines safely
tags: lightrec,dynarec,dependency,core,product
created: 2026-09-04
updated: 2026-09-04
---
state_items: S012, S016

## Root cause

The maintained fork is now consumed directly at revision
`b764c4c9f4bc425a56bfc4c32333ff8200ce8ab9`, and its Linux x86-64 runtime executes translated blocks
with classified fallback telemetry, pre-interpreter admission, and an exact block-boundary callback
used by psxport's image-qualified native/HLE dispatcher. GNU Lightning's process-wide
initialization/teardown still prevents two initialized Lightrec machines from living safely
together.

## Required outcome

Add a direct dependency on an exact maintained Lightrec revision. If upstream cannot expose the
required callbacks, invalidation, and bounded fallback telemetry, put those API changes in a
maintained fork and pin that commit; do not add a patch file. The gameplay library has no explicit
interpreter mode. Every `Core` owns and tears down its own Lightrec state and callback context.
Fallback is automatic only for a named allowed reason, is counted and bounded, and a threshold breach
returns a typed executor fault.

The discriminator must create two cores with different register/memory state, execute nonzero
translated blocks on both, and prove their Lightrec state, callbacks, counters, caches, and teardown
do not cross. Product inspection must name every gameplay source/selector scanned and find no
explicit interpreter mode or offline guest source. Positive tests exercise every fallback reason and
both threshold breaches.
