---
id: 49
title: Replace generated-symbol calls with image-scoped native and original dispatch
status: open
symptom: native title code names generated guest bodies and address-only overrides cannot distinguish overlays that reuse a guest range
tags: overrides,overlays,identity,original-call,dispatch
created: 2026-09-04
updated: 2026-09-04
---
state_items: S014, S019

## Root cause

The static pipeline gave every generated body and wrapper a unique host symbol, so native code could
call that implementation directly and override tables could assume the compiled module selected the
right body. Runtime execution removes that host-symbol identity. PSX resident code and overlays can
reuse one guest address, so address-only replacement or a process-global "disable overrides" flag
would call the wrong code or suppress unrelated nested native behavior.

A portfolio audit found 816 distinct unresolved generated-body/wrapper symbols across 109 Tomba! 2
game files when its generated corpus was absent. This is the measured consumer scale, not a reason to
add compatibility wrappers.

## Required outcome

Own a per-`Core` dispatch key of authenticated image/module identity, load generation, and guest
address. Normal calls honor the current key's override. An original call suppresses only that exact
override for one bounded dynamic extent and enters the guest body through Lightrec; nested calls and
same-address code from another image retain normal dispatch. Every consumer replaces generated C
symbol calls with one of these two runtime operations.

The first framework discriminator uses resident code and two overlay images that reuse an address,
then tests normal, disabled, nested, and original-call behavior. Any override decision captured in
translated code must be invalidated when registration changes.
