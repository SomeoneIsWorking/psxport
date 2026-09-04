# Dynarec-default Lightrec integration

The framework migration is break-first: obsolete product executors, generators, derived guest
source, selectors, compatibility dispatch, and generation-only tooling are absent before the
replacement backend is accepted. `runtime/cpu/` is the sole guest-execution boundary.

## Maintained-fork requirements

psxport consumes `https://github.com/SomeoneIsWorking/lightrec.git` directly at
`c9f0a37dbbc7e24d841c84751d9619ad1bfcb7d8`. The dependency resolver validates the selected source
tree, its clean tracked state, and revision, including when a parent project already created the
`lightrec` target.
That fork provides synchronous native translation, a callback at every translated block boundary,
exact execution/cache/fallback counters, range/full-cache invalidation, and an x86-64 host proof. The
remaining product contract requires it and the psxport adapter together to:

- use native translation as the default for every supported block and host;
- preserve per-instance block dispatch so image-scoped native overrides intercept chained blocks;
- expose bounded range and whole-cache invalidation without process-global CPU state;
- report initialization failure as a stable typed fault; a missing or unsupported dynarec backend
  never enters interpreter execution;
- classify per-block fallback as exactly compilation failed, unsafe instruction fetch, rare
  unsupported block, or cache exhaustion;
- bound fallback instructions per call and as a fraction of total guest instructions;
- publish per-reason calls, interpreted instructions, total guest instructions, and threshold trips;
- return a typed fault when either threshold is exceeded; and
- be pinned directly by immutable URL/revision/upstream-base metadata, with changes as normal fork
  commits rather than patch files.

There is no player-selectable interpreter mode. A diagnostic interpreter mode remains a separately
built test tool. Automatic fallback is a rare product recovery path, not a second gameplay engine.

## Host backends

The maintained fork must support x86-64 Linux and AArch64. AArch64 qualification is separate for
Apple Silicon macOS and Android `arm64-v8a`. Each requires:

- native AArch64 code generation for the ordinary path;
- platform-correct writable/executable memory allocation and publication;
- instruction-cache synchronization after publication and rewrites;
- bounded invalidation and safe block-chain revocation; and
- ABI-correct transitions for normal entry, native/HLE callbacks, fallback, typed exits, faults,
  and teardown.

Fallback cannot stand in for missing AArch64 host code generation. A host that exceeds the fallback
threshold is unsupported and fails by name. ABI mismatch, corrupted IR, an unbalanced writable/
executable transition, or another executor invariant remains fatal rather than falling back.

The maintained GNU Lightning fork also needs two concrete AArch64 fixes. Android must reserve x18
rather than allocate it as a general register. macOS executable memory must use `MAP_JIT` and
`pthread_jit_write_protect_np` with per-thread writable windows around every arena mutation,
including TLSF create/allocate/reallocate/free and code emission. Lightrec alternate buffers must
participate in the same protection transitions, and instruction-cache invalidation occurs after the
write window closes. An option that merely enables unsafe writable/executable memory is rejected.

## Framework contract

Each `Core` owns one executor. `Core` state is authoritative at entry; guest-visible GPRs, HI/LO,
PC/delay state, CP0, GTE, interrupts, and cycles are committed before host callbacks or fallback and
restored before re-entry. `ExecutionResult` carries the reason, PC, cycles, and detail. Nested native
calls propagate the full pending result; C++ exceptions never unwind through generated host code.

`ImageIdentity` combines authenticated image/module identity, load generation, and guest address.
A normal call honors its override. A scoped original call suppresses only the current complete key,
so nested calls still use their own overrides. Every executable writer routes its normalized range
through the central invalidation owner after bytes are visible.

`ExecutorCounters` is the observable product contract. It exposes exact Lightrec
translated-versus-executed blocks, cache hit/miss counts, execution/fallback totals, and psxport
invalidation and fault totals. The migration remains incomplete until fallback limits are enforced
per call and per total guest-instruction share. Limits will enter through typed configuration; no
environment read belongs in the executor.

## Acceptance gates

The backend milestone requires all of the following:

1. Clang/Ninja product builds and source/selector scans on x86-64 Linux, Apple Silicon macOS, and
   Android `arm64-v8a`, with no derived guest source or explicit interpreter mode.
2. Two simultaneous `Core` instances execute nonzero translated blocks without state crossover.
3. Deliberately distinct state survives each typed exit, fallback, and resume direction.
4. Resident and colliding-overlay normal/original calls pass, including generation reuse and
   registration-change invalidation.
5. Executable writes pass overlap, non-overlap, alias, DMA/load, restore, and override-policy tests.
6. Each allowed fallback reason has a positive discriminator; unclassified requests and both
   threshold breaches return typed faults. Telemetry denominators are nonzero and exact.
7. The independent oracle demonstrates both a matched checkpoint and a seeded divergence.
8. A reference consumer reaches representative interactive gameplay with native services active,
   nonzero translated blocks, and fallback below its declared release threshold.

The current Linux x86-64 tree satisfies the synthetic backend portion of item 1. A focused contract
test proves translated call to native dispatch and translated resume, nested native context, an
original guest body stopping at the exact caller continuation, measured timing/pending work,
self-modifying-code retranslation, typed unsafe-fetch failure, and fallback telemetry. Items 2
through 8 remain open at representative-title scope, as do Apple Silicon and Android AArch64
qualification. Complete executable-writer coverage and fallback threshold enforcement remain open.
