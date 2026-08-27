---
id: 32
title: Direct-runtime pad service dereferences the absent legacy config
status: resolved
symptom: A title-owned FrameDriver calling Pad::serviceFrame with core.cfg null crashes before publishing controller input.
tags: runtime,pad,input,gameconfig,architecture,tekken3
created: 2026-08-27
updated: 2026-08-27
---

Root cause: Pad::serviceFrame owned standard Sony packet publication but resolved every guest buffer
only through the legacy GameConfig bag. Direct GameRuntime had no typed pad-buffer fact slice, so a
finite native title driver dereferenced a null compatibility config before it could publish input.

Resolved by the immutable `GuestPadBufferLayout` fact supplied through
`GameRuntime::guestPadBufferLayout()`. The resolver preserves legacy-config precedence during adapter
migration, consumes the direct-runtime layout otherwise, and returns a safe no-write layout when a
title has not declared one. `test_pad_buffer_layout` drives the shipping resolver/service path and
proves all three cases: legacy precedence, direct-runtime digital packet publication, and missing-
layout no-write behavior. MMX4 and Tekken now publish their measured fixed buffers through this typed
boundary; Tekken's values remain grounded by FUN_800B0B9C passing `0x800A9132` and `0x800A915C` to
linked libpad init FUN_800946A8.
