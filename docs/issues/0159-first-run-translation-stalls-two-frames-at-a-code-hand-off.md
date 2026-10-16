# 0159: first-run translation stalls two frames at a code hand-off

Status: open

## Observed
Tomba! 1 real-window play check (2026-10-16): after the attract movie, at the New Game hand-off, two consecutive
presents take 37-43 ms (`longest_miss_run=2`) in every run (three runs, 04:38:15, 04:40:32, 04:41:35 UTC).
The pacer shows four frames with `sleep=0` and two `RESYNC`s there; the guest tasks finish inside their field
budgets. Tomba! 2 shows the same at its area-load-to-field hand-off (40.8 ms, run 2).

## Cause
`perf record -k CLOCK_MONOTONIC` over the 200 ms window: 52% of samples in GNU lightning
(`_jit_optimize`, `_jit_reglive`, `_jit_update`) under `lightrec_emit_code`, plus Lightrec's own optimizer
passes (`lightrec_consts_propagate`, `lightrec_early_unload`). `runtime/cpu/lightrec_executor` translates each
newly reached guest block synchronously on the frame thread, so a burst of first-run code costs a frame its
budget.

## Gap
Moving translation off the frame (Lightrec's threaded compiler runs the block on its interpreter meanwhile,
which the product rules allow only as reason-coded fallback) or translating loaded code ahead of its first
call is an executor design change, not made here.
