# The store observer segfaults when armed on a store in a branch delay slot

**Status:** open. Framework defect, reproduced with a control matrix, root cause not yet isolated.

## The defect

`PSXPORT_STORE_OBSERVE` accepts a list of guest **store-instruction PCs** and instruments them so
each execution is counted. Arming it on a store that sits in a **branch delay slot** crashes the
process with SIGSEGV, before any report is printed.

This was found while trying to use the observer to name the exact store that clobbers a guest
interrupt element in Mega Man X4 (`megamanx4/docs/issues/0037`). Every earlier use of the observer
in that investigation appeared to return *nothing*, and the reason was not a quiet instrument — it
was a **crashed run**. A killed or crashed process prints no teardown report, so "the observer saw
no stores" and "the observer destroyed the process" look identical in a log.

## Reproduction

All runs are Mega Man X4, headless, `PSXPORT_DEBUG=store-observe`, `PSXPORT_NATIVE_FRAMES=3000`,
against the same build; only `PSXPORT_STORE_OBSERVE` varies.

| armed store PC(s) | what it is | exit |
|---|---|---|
| *(none — control)* | observer facility off | **0** |
| `0x80012628` | `sw $v0, -0x7d00($at)`, ordinary store | **0** |
| `0x80015F04` | `sb $v1, 0x48($a0)` | **0** |
| `0x80015F7C` | `sh $s3, ($a1)` | **0** |
| `0x80015F18` | `sh $v0, ($a1)` | **0** |
| `0x80015F80` | `sh $s2, -4($a0)` | **0** |
| `0x80015F04,0x80015F7C` | two of the above together | **0** |
| **`0x800126A8`** | **`sh $s1, ($v0)` — delay slot of `jal 0x800EDdbc`** | **139** |
| `0x80012628,0x800126A8` | ordinary store + the delay-slot store | **139** |

**The discriminating variable is the delay slot, not the width and not the count.** `0x800126A8` is
a `sh`, and so are `0x80015F7C` and `0x80015F18`, which are harmless. `0x800126A8` is the delay slot
of the `jal` at `0x800126A4` — the `ChangeTh` call — and it alone reproduces the crash, alone or
alongside a working store.

## Why the mechanism is already suspected, not established

Issue `0039` established that the observer is invasive: arming it calls
`lightrec_invalidate_all` and `lightrec_free_all_blocks`, the emitter instruments every store in
every block, and `lightrec_rec_observed_store` calls `lightrec_clean_regs` followed by
`lightrec_regcache_reset`.

**A delay-slot store is the one case where those two behaviours can meet badly.** The register cache
is written by a block's instructions, and a delay-slot instruction is emitted as part of the
*branching* block while the branch target is the *next* block. Forcing a register clean and a regcache
reset from inside a delay slot means the clean happens on a code path the emitter did not expect to be
a normal instruction boundary. **This is a hypothesis with a mechanism, not a diagnosis** — the
emitted code has not been read, and the crash has not been reduced to a failing instruction.

**Named next step:** reduce the crash to a single emitted block by arming only `0x800126A8` and
capturing the JIT's block for `0x800126A4..0x800126AC`; then read whether the delay-slot store's
instrumentation is emitted into the wrong block, emitted twice, or emitted at all.

## Why this is recorded rather than worked around

The obvious workaround — "don't arm a delay-slot store" — is a **silent substitution**: the
investigation that needed this observer was trying to name the exact store instruction, and delay
slots are where stores live in ordinary compiled guest code. A tool that cannot watch a common
instruction class is not a tool, and the failure mode it produces (a log with no report, read as
"nothing happened") is the specific failure this project treats as worst. The correct fix is in the
framework, not in the caller's choice of address.
