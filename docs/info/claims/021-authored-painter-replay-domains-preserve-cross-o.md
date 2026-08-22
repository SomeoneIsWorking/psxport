---
id: C021
kind: claim
status: holds
created: 2026-08-22
tags: gpu,renderer,painter,order
depends: runtime/recomp/painter_object_layer.cpp#planPainterItemStream, runtime/recomp/render_queue.cpp#RenderQueue::emitItemStream, runtime/recomp/fps60.cpp#Fps60::presentPass
reconfirmed: 2026-08-22
verified_at: 2026-08-22 16:34:08
---

## Claim

Authored painter replay domains preserve cross-object guest OT order through the shipping presentation path within each physical flush

## Evidence

Framework planner tests merge objects from distinct captured queues, enforce the shared replay comparator, and falsify duplicate keys, unordered world faces, mixed policies, object/domain aliasing, and mixed flush epochs; GPU staging proves object transitions do not coalesce; full Clang CTest passed 83/83; a real Spyro stage-13 presentation reported one 782-command authored range across three objects and restored the guest foreground/background ordering.

## What would falsify it

A shipping frame bypasses planPainterItemStream, items from different physical flushes are regrouped, or a guest/reference OT stream disagrees with the planned cross-object command order.

## Re-confirmed 2026-08-22

Reverified after per-flush local-rank correction: planner/staging tests and full Clang framework CTest pass 83/83; touched 7/7 compile-backed C++ TUs pass clang-tidy; real Spyro stage-13 output has coherent cross-object foreground/background ordering.
