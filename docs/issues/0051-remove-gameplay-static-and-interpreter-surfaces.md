---
id: 51
title: Remove gameplay static and interpreter execution surfaces
status: open
symptom: the framework and consumers can link or select generated C and interpreter engines at runtime
tags: migration,interpreter,generated,selector,product-link
created: 2026-09-04
updated: 2026-09-04
---
state_items: S016, S018, S021, S022

## Root cause

The current product library owns the offline translator interfaces, generated registry/dispatch,
flat interpreter, `PSXPORT_ENGINE` selector, and harness branches that choose execution engines inside
one process. That shape allows a gameplay run to select or silently retain a non-product engine, and
it makes a test oracle part of the product link rather than a separately built instrument.

## Required outcome

After the Lightrec executor, state bridge, image-scoped calls, and invalidation pass their focused and
reference-consumer gates, remove the generator, generated-code build rules/interfaces, seed/provenance
machinery that exists only for emission, gameplay engine enum/CVar/UI/CLI selection, and interpreter
objects from the product library. Move only still-useful interpreter diagnostics into a separate
test-oracle target. Configure Lightrec itself without interpreter fallback.

The landing gate must inspect the fresh gameplay build and source selector surfaces, name the object
and symbol counts scanned, prove nonzero Lightrec block execution, and find zero interpreter or
generated-body links. The test target separately proves that its oracle can produce both matching and
diverging answers.
