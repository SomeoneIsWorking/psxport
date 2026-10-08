# 0131 — the pool diagnostic's OT node count is one MORE than the walk read, when the walk was cut short

**State:** recorded, deliberately NOT fixed. The readability refactor that named the walk
(`runtime/psx/gpu/ordering_table.cpp`, 2026-09-28) reproduced the arithmetic rather than correcting it,
because a diagnostic that changes its own numbers during a refactor has an unreadable history.

## What it says

`PSXPORT_DEBUG=pool` prints one line per `DrawOTag`:

```
f{} madr=0x{:08X} nodes={} pool=0x{:08X} hi=0x{:08X}
```

`nodes` is meant to be "the OT entries this walk traversed".

## What it actually reports

The walk counted with a loop guard and reported `guard + 1`:

- a well-formed table that ends at a sentinel breaks out of the loop with `guard` at the index of the
  last node, so `guard + 1` IS the number of nodes traversed — correct;
- a table that does not terminate inside the cap exits on the loop condition with `guard == kOtNodeLimit`,
  having traversed exactly `kOtNodeLimit` nodes, so the line reports `kOtNodeLimit + 1`.

So the number is one too large in exactly the case where the walk also emits its
`WARN: OT walk hit N-node cap` line, and correct in every other case.

## Why the number still matters

It is the denominator of the fixed-buffer-overflow hypothesis the line exists for (later-124). That
argument compares `nodes` against the pool high-water, and the one frame where the count is wrong is
the frame where the table is malformed — i.e. exactly the frame a reader is most likely to be checking
this number on. A reader who trusts `nodes` there is reading a figure for a walk that read one node
fewer than it claims.

## What would fix it

`OrderingTableCursor::nodesEntered()` is the honest count and the call site already has it; the extra
`+ (truncated ? 1 : 0)` is there to preserve the old figure. Dropping that term makes the line agree
with its own label.

It is left in place because:
- the change is inside a `PSXPORT_DEBUG=pool` diagnostic, so it is low-risk, but it is still a change
  to a number someone may have a run log for; and
- the honest fix should land with a note in the line's own comment, which is a separate edit.

## Where

- `runtime/psx/gpu/gpu_native.cpp` — the `pool` channel block in `GpuState::gpu_dma2_linked_list`, whose
  comment names this issue.
- `runtime/psx/gpu/ordering_table.h` — `nodesEntered()`.
- `tests/test_ordering_table.cpp` — `node_count_is_the_number_entered`, which asserts the CURSOR's count
  is the number entered. It does not assert the pool line's figure, because the pool line's figure is
  the thing under question.
