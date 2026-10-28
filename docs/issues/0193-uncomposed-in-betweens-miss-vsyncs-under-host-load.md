# 0193 — Uncomposed in-betweens miss vsyncs under host load

Status: open (2026-10-28). Owner: not traced; the in-between present path (`frame/frame_presenter.*`,
`present/guest_redraw.*`).

## Observed
Paced 2400-frame hidden runs from boot (fps60 1, ires 0, sink 3840x2160, `PSXPORT_DEBUG=presentseq`,
`scratch/player-bugs-8/`, `intervals.py` for the per-present attribution):

| run | load at start | missed | where |
|---|---|---|---|
| Crash 1, picker `pick crash1` (`r3-picker`) | 5.30 | 116 (2.42%), longest run 9 | in-between presents `t=0.5 composed=0`, fences ~2100-2133, 40-49 ms each |
| Crash 1, same binary (`r3-picker-quiet`) | 1.74 | 1 | boot |
| Crash Bash (`r3-quiet`) | ~1.5 | 7 | one at boot (0045), six `t=0.5 composed=0` in-betweens 26-41 ms after scene change #6 (`f1401`, scene `0x8009F720`) |
| Crash Bash (`r3-paced`) | 5.36 | 4 | boot (0045) and one guest call that outlived a host turn |

Every Crash 1 in-between in both runs is `composed=0` (2389), so the same work misses or not with the host's load.
Whole presents and composed in-betweens in the same runs (Spyro 1, Vagrant Story, X4, Tekken 3, C-12) stayed at
p99 17.0 ms at the same loads.

## Not yet known
What an uncomposed in-between costs and where (guest redraw, record raster, submit), measured per present on a quiet
host, and why Crash Bash's scene `0x8009F720` slows it without load. Trace before any change.
