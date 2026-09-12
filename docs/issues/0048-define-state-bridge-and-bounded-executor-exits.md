---
id: 48
title: Define the Lightrec state bridge and bounded executor exits
status: open
symptom: current host boundaries assume generated C calls or exception unwinding instead of returning from JIT execution with synchronized PSX state
tags: lightrec,state,cycles,interrupts,executor
created: 2026-09-04
updated: 2026-09-12
---
state_items: S013

## Root cause

The existing runtime grew around C-callable generated guest functions. Native calls, BIOS/HLE,
interrupt return, frame boundaries, and cooperative task transitions can therefore rely on the host C++
call stack or private exception unwinds. JIT frames do not preserve that ownership model, and two
architectural-state copies would become ambiguous unless entry and exit authority are explicit.

An X4 STR-startup field wait demonstrated a narrower ownership failure in the shipping Lightrec
bridge: a Coro task parks inside a native callback with its nested `BoundarySession` live, while the
main host thread resumes an outer guest call. The single executor-wide return marker then belongs
to the parked task, so the main caller crosses its explicit return PC. Coro serializes Core access
but uses a distinct host thread, and the native field-wait continuation must remain on its C++
stack. Boundary state therefore belongs to the executing host thread and must stack for nested
calls on that thread. A canceled Coro can `longjmp` past C++ destructors; thread-local contexts
must die with their own thread and use a non-reused executor identity rather than retain a stale
pointer into a destroyed executor. The focused shipping-path regression was red on the shared
marker and passes with per-thread contexts, including resume and cancellation. Fallback admission
also belongs to each boundary: a task parked before its first fallback must retain its default
one-block allowance even if the main thread uses one while the task is parked. A two-thread synthetic
was red with a global Lightrec-statistics baseline (task fallback refused after the main fallback)
and passes when each `BoundaryContext` counts its own admissions. A bounded X4 retail run reached the
five-iteration native loop cap after the per-thread marker change; it preceded the allowance fix and
did not print JIT/fallback denominators, so final retail qualification remains open.

## Required outcome

Create one production state bridge for GPRs, HI/LO, PC plus branch-delay/next-PC state, CP0, GTE,
pending interrupts, and guest cycles. Create a typed bounded execution result for budget exhaustion,
native override, HLE/device service, interrupt/exception, frame/VSync, thread yield/exit, and fatal
translation/memory faults. Commit all guest-visible state before host handling and reload host changes
before re-entry. Do not unwind C++ exceptions through Lightrec frames.

Focused tests must exercise each exit reason through the shipping executor seam and prove both state
directions, cycle accounting, nested resume, and budget exhaustion. The negative cases must seed a
wrong register/cycle answer and show the comparator reports it.
