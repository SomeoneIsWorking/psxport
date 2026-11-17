# Native/interpreter execution

The framework migration is break-first: obsolete product executors, generators, derived guest
source, selectors, compatibility dispatch, and generation-only tooling are absent before the
replacement executor is accepted. `runtime/cpu/` is the sole guest-execution boundary.

## Guest executor

`psx::cpu::GuestExecutor` in `runtime/cpu/guest_executor.*` runs every guest instruction no native override
claims; `Core::executor()` reaches it. It runs guest blocks on psxport's own run-time recompiler
(`runtime/cpu/recompiler/`) where the host has a backend, and on psxport's own R3000 interpreter
(`runtime/cpu/guest_interpreter.*`) everywhere else. The interpreter is the reference: the recompiler must leave
`Core` and memory exactly as it would. Both engines share `ExecutorState` (`runtime/cpu/executor_state.*`): loads
in flight, the segment clock and every memory, COP0 and GTE access. The executor:

- runs directly on `Core`'s registers, with no second register file to copy in or out;
- sends every store and every load outside main RAM and the scratchpad through `Core::mem_r*`/`mem_w*`, and
  commits the elapsed guest time before a hardware-register access;
- executes real branch and load delay slots;
- keeps the classic recompiler block shape: a block ends after an unconditional jump's delay slot, at a
  SYSCALL or an MTC0/CTC0 to Status/Cause, or at a taken branch that leaves it. Every block entry checks
  native dispatch, the requested guest return and pending work, and latches the writer, whichever engine ran
  the block;
- charges one guest clock tick per instruction; and
- returns a named fault for an unknown instruction or an instruction fetch outside main RAM and the
  scratchpad.

The recompiler translates a block from guest memory the first time it is entered and never saves a
translation. Every executable write (CPU stores, DMA, module loads, the debugger, savestates) reaches it through
the one invalidation owner (`runtime/cpu/invalidation.cpp`), which drops every block read from the changed bytes;
a block that overwrites itself finishes on the interpreter. A block whose shape the translator does not take
(for example a branch into a delay slot, or a load in flight to a register its first instruction uses) runs on
the interpreter, counted by reason in the run-end `[recompiler]` line. Main-RAM and scratchpad loads are read
inline unless the vertex places would note them (`VertexPlaces::LoadGate`).

Translated blocks link: a direct exit with no load in flight (a link site) jumps straight into the next
translated block instead of returning to the dispatcher. Linking is opt-in per backend (`block_cache.h`
`LinkSite`/`LinkedCode`, a translator that returns them, and the backend's `patchLink`/`unpatchLink`); a backend
without it leaves every block through the dispatcher. A site starts unlinked and is patched lazily the next
time the dispatcher resolves its target to a translated block; JR/JALR exits stay unlinked. The jump lands on
the target's linked entry, which makes the checks the dispatcher makes between blocks (segment budget, stop,
interrupt) and then calls the executor's own block entry (`kHelperEnterLinked`), so native dispatch, the guest
return, pending work, the writer latch and the trace run at every block entry exactly as unlinked. Whatever
stops there leaves for the dispatcher as an unlinked exit would. Dropping a block repoints every site linked to
it back at its unlinked exit; a flush of the code mapping forgets every link. `PSXPORT_EXEC_NOLINK=1` is a
run-only diagnostic that leaves every site unlinked, the before side of a linking measurement; the run-end
`[recompiler]` line counts `linked_entries` and `links_patched`.

Measured 2026-11-17 on an M2, Tekken 3's gameplay window: about half of all block entries go through a link,
and executor time is unchanged within the noise. The trip linking removes (`runBlock`, the cache lookup, the
`runSegment` loop) was the cheap part; `enterBlock` itself is about 40% of executor time either way (the writer
latch's call-word read through `Core::host_ptr` alone about 7%), and a linked entry still runs it. Linking pays
once `enterBlock` is cheaper.

There is no player-selectable engine. Independent reference emulators remain separately built test
tools and are never linked into gameplay.

WebAssembly is part of the migration release contract: the project must also provide a browser-
capable runtime path that uses the same guest executor, boundary identity, and telemetry semantics as
the native hosts. A release claim stays blocked until web parity is represented in the acceptance
gates.

The differential check holds the recompiler to the interpreter. One recipe runs twice, once with
`PSXPORT_EXEC_INTERPRET=1` and once on the recompiler, each with `PSXPORT_EXEC_TRACE=<file>`, which records the
committed CPU state (GPRs, HI/LO, COP0, loads in flight, a GTE hash, and at intervals a RAM and scratchpad hash)
at every `PSXPORT_EXEC_TRACE_EVERY`-th block entry. `tools/exec_trace_compare.py` names the first disagreeing
checkpoint and the `PSXPORT_EXEC_TRACE_FROM`/`_TO` window that, rerun, names the first divergent block. Run the
two sides one after the other: runs sharing a memory card see each other's saves. These knobs are run-only
diagnostics; no player setting picks an engine.

## Hosts

The recompiler's backend is ARM64 (macOS on Apple silicon, Android, Linux on ARM64). Every other host, and a
host that refuses executable memory, runs the interpreter; an x86-64 backend is next. Translated code lives in
one bounded mapping per executor (`recompiler/code_memory.*`): 32 MiB, bump-allocated, dropped whole when full.
On macOS it is `MAP_JIT`, written only inside `pthread_jit_write_protect_np` scopes; the release build is
signed ad hoc without the hardened runtime, so it needs no JIT entitlement (a notarized build would need
`com.apple.security.cs.allow-jit`). Elsewhere the mapping is read-write-execute, and each sealed block has its
instruction cache synchronized.

## Framework contract

Each `Core` owns one executor. `Core` state is authoritative at entry; guest-visible GPRs, HI/LO,
PC/delay state, CP0, GTE, interrupts, and cycles stay in `Core` throughout, and a stop lands every load
still in flight before host callbacks run. `ExecutionResult` carries the reason, PC, cycles, and
detail. Nested native calls propagate the full pending result; C++ exceptions never unwind through a
guest segment (`SideEffectJournal::GuestExecutionScope`).

`ImageIdentity` combines authenticated image/module identity, load generation, and guest address.
A normal call honors its override. A scoped original call suppresses only the current complete key,
so nested calls still use their own overrides. Every executable writer routes its normalized range
through the central invalidation owner after bytes are visible.

`ExecutorCounters` is the observable product contract. Its ledger prints `guest:` (calls, executed
blocks and instructions, host dispatches, memory accesses, device clock commits and the instructions
they committed, invalidations, faults), `invalidations_by_source:` and `budget_exit:`. No environment
read belongs in the executor.

## Acceptance gates

The executor milestone requires all of the following:

1. Clang/Ninja product builds and source/selector scans on x86-64 Linux, Apple Silicon macOS, and
   Android `arm64-v8a`, with no derived guest source or engine selector.
2. Two simultaneous `Core` instances execute nonzero guest blocks without state crossover.
3. Deliberately distinct state survives each typed exit and resume direction.
4. Resident and colliding-overlay normal/original calls pass, including generation reuse and
   registration-change invalidation.
5. Executable writes pass overlap, non-overlap, alias, DMA/load, restore, and override-policy tests.
6. An unknown instruction and an out-of-range instruction fetch return named faults. Telemetry
   denominators are nonzero and exact.
7. The independent oracle demonstrates both a matched checkpoint and a seeded divergence.
8. A reference consumer reaches representative interactive gameplay with native services active,
   and nonzero executed guest blocks.

The current Linux x86-64 tree builds with Clang for item 1. Items 2
through 8 remain open at representative-title scope, as do Apple Silicon and Android AArch64
builds. Complete executable-writer coverage remains open.

## Direct-runtime service contracts

A title's disc environment key is a disc fact in its `psx::host::TitleIdentity::discEnvVar`;
`TitleSession::boot` hands it to the shared disc resolver (`DiscState::env_key`) before anything reads
the disc. An absent key retains generic disc resolution. A
missing boot executable is extracted through the same resolver by `psx::host::provisionExecutable`. Title
code must not introduce another disc-open, extraction or environment-parsing path.

Measured stock CD bindings use the shared command body `cd_command_stock_sync` for the low-level
zero-on-success ABI and `cd_control_sync` for the public one-on-success ABI. Both apply the same
command effects. The distinction belongs at the measured SDK boundary, never in a title-local
copy of drive policy.

Host-turn callbacks execute only outside guest IRQ handlers, masked critical sections, native
dispatch, pending redirects, and another host-turn callback. All deferred requests retain
`Core::PW_HOST`; at an eligible boundary the bit is acknowledged for delivery, or retired if that
Core has no registered handler. A request never dispatches another Core's callback. A request raised
during a callback remains pending for the next eligible boundary. 
