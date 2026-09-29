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

## ROOT CAUSE — the hook resets the register cache while the branch emitter is holding a backup

Read from `emitter.c` in the shared Lightrec tree, this is no longer a hypothesis.

`rec_b` — the branch emitter — takes a **branch-mode snapshot of the register cache** and then
emits the delay slot *inside* that branch:

    lightrec_free_regs(reg_cache);
    regs_backup = lightrec_regcache_enter_branch(reg_cache);   /* regcache is now in BRANCH mode */
    ...
    if (op_flag_local_branch(op->flags)) {
        if (!op_flag_no_ds(op->flags) && ds->opcode) {
            state->no_load_delay = true;
            lightrec_rec_delay_slot(state, block, offset + 1); /* the delay slot is emitted HERE */
        }
        ...
        lightrec_clean_regs(reg_cache, _jit);
    }

and `rec_b` restores from `regs_backup` when it finishes the branch.

The delay slot reaches `lightrec_rec_opcode` through `lightrec_rec_delay_slot`, which sets
`state->in_delay_slot = true` and recurses. `lightrec_rec_opcode` computes the observer match
**per instruction**, from `block->pc + (offset << 2)` — so **the hook is correctly emitted for a
delay-slot store**, and `lightrec_rec_observed_store` then does what issue `0039` measured:
`lightrec_clean_regs` followed by `lightrec_regcache_reset`.

**So the observer resets the register cache while `rec_b` is mid-branch, and `rec_b` afterwards
restores a `regs_backup` that no longer describes the cache.** The generated code then assumes
registers are resident in the native register cache when they are not. **That is the segfault.**

This explains the whole control matrix at once, including the part that was hardest to explain:

- A store in a **normal** instruction boundary resets the cache where no branch backup is live, so
  the reset is harmless — `0x80012628` and the four `0x80015Fxx` stores all exit 0.
- A store in a **delay slot** resets the cache inside a live `regs_backup` window, so the branch's
  restore is applied to a cache it no longer matches — `0x800126A8` exits 139.
- It is not the store **width** and not the **number** of armed PCs: the harmless `0x80015F7C` and
  `0x80015F18` are the same `sh` that is fatal at `0x800126A8`.

## The fix, named at the location that owns it

`lightrec_rec_observed_store` must not reset the register cache while a branch backup is live. There
are three defensible shapes, and the choice belongs to the emitter, not to the observer's caller:

1. **Suppress the hook in a delay slot** — `state->in_delay_slot` is already set and already
   threaded to exactly the point that needs it, so this is the smallest change. It costs the ability
   to watch delay-slot stores, which is the capability that was wanted.
2. **Do the regcache reset without the branch restore** — let the observer keep its counters but
   have `rec_b` re-take `regs_backup` after an observed store. More faithful, and it touches the
   branch emitter's contract.
3. **Defer the store's bookkeeping to a normal boundary** — record the match, clean up at the next
   `op_flag_sync` boundary, which `lightrec_rec_opcode` already handles explicitly.

**Option 1 alone would re-create the failure this issue exists to record**: the instrument would
again silently not watch a common instruction class, and a log with no report would again be read as
"nothing happened". If option 1 is chosen, the suppression must be **announced at the same place the
armed list is printed**, naming the suppressed PCs, so an absent report is never ambiguous.

## What was NOT done here, deliberately

`shared/lightrec` is a shared, pinned dependency and the fix is a change to the JIT emitter's
branch contract. It is not patched in this commit: the root cause is now precise enough that the
change can be reviewed on its own merits, and changing a shared emitter's branch semantics on the
strength of one port's crash is the wrong order of work.

## Why this is recorded rather than worked around

The obvious workaround — "don't arm a delay-slot store" — is a **silent substitution**: the
investigation that needed this observer was trying to name the exact store instruction, and delay
slots are where stores live in ordinary compiled guest code. A tool that cannot watch a common
instruction class is not a tool, and the failure mode it produces (a log with no report, read as
"nothing happened") is the specific failure this project treats as worst. The correct fix is in the
framework, not in the caller's choice of address.
