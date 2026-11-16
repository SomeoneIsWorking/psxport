# Native/interpreter execution

The framework migration is break-first: obsolete product executors, generators, derived guest
source, selectors, compatibility dispatch, and generation-only tooling are absent before the
replacement executor is accepted. `runtime/cpu/` is the sole guest-execution boundary.

## Guest executor

psxport's own R3000 interpreter, `psx::cpu::GuestExecutor` in `runtime/cpu/guest_executor.*`, runs every
guest instruction no native override claims. `Core::executor()` reaches it. There is one executor on every
host and no engine choice: no recompiler, no translation cache, no fallback tier. The executor:

- runs directly on `Core`'s registers, with no second register file to copy in or out;
- reads code from guest memory as it runs, so an invalidation has nothing to flush and is only counted;
- sends every load and store through `Core::mem_r*`/`mem_w*`, and commits the elapsed guest time before a
  hardware-register access;
- executes real branch and load delay slots;
- keeps the classic recompiler block shape: a block ends after an unconditional jump's delay slot, at a
  SYSCALL or an MTC0/CTC0 to Status/Cause, or at a taken branch that leaves it. Every block entry checks
  native dispatch, the requested guest return and pending work, and latches the writer;
- charges one guest clock tick per instruction; and
- returns a named fault for an unknown instruction or an instruction fetch outside main RAM and the
  scratchpad.

There is no player-selectable engine. Independent reference emulators remain separately built test
tools and are never linked into gameplay.

WebAssembly is part of the migration release contract: the project must also provide a browser-
capable runtime path that uses the same guest executor, boundary identity, and telemetry semantics as
the native hosts. A release claim stays blocked until web parity is represented in the acceptance
gates.

## Hosts

The interpreter generates no host code. Every host runs the same executor, with no writable/executable
memory, instruction-cache synchronization or per-architecture backend to qualify.

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
