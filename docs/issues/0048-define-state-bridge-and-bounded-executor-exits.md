---
id: 48
title: Define the Lightrec state bridge and bounded executor exits
status: open
symptom: current host boundaries assume generated C calls or exception unwinding instead of returning from JIT execution with synchronized PSX state
tags: lightrec,state,cycles,interrupts,executor
created: 2026-09-04
updated: 2026-09-04
---
state_items: S013

## Root cause

The existing runtime grew around C-callable generated guest functions. Native calls, BIOS/HLE,
interrupt return, frame boundaries, and cooperative task transitions can therefore rely on the host C++
call stack or private exception unwinds. JIT frames do not preserve that ownership model, and two
architectural-state copies would become ambiguous unless entry and exit authority are explicit.

## Required outcome

Create one production state bridge for GPRs, HI/LO, PC plus branch-delay/next-PC state, CP0, GTE,
pending interrupts, and guest cycles. Create a typed bounded execution result for budget exhaustion,
native override, HLE/device service, interrupt/exception, frame/VSync, thread yield/exit, and fatal
translation/memory faults. Commit all guest-visible state before host handling and reload host changes
before re-entry. Do not unwind C++ exceptions through Lightrec frames.

Focused tests must exercise each exit reason through the shipping executor seam and prove both state
directions, cycle accounting, nested resume, and budget exhaustion. The negative cases must seed a
wrong register/cycle answer and show the comparator reports it.
