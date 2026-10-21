# 0172: the producer census runs in the player's product

Status: fixed (psx-spyro 0196 pass 4). Owner: `runtime/psx/config/config.cpp` `cv_producers`, `runtime/psx/debug/ot_attr.h`.

## Observed
Review of psx-spyro issue 0196 pass 3: the boot-handover field's profile (issue 0171) spent samples in
`OtAttr::trackStoreSlow` in a run with no diagnostic channel enabled.

## Cause
`PSXPORT_PRODUCERS` defaults to true (`cv_producers`), and `Game::wireRuntimeMembers` arms
`g_producer_census_armed` from it, so `OtAttr::trackStore` books every guest store in ordinary play. `ot_attr.h`
records its price on an unpaced 3D field: 18% of the run (`resolveClaimedFrame` 13.3% + `trackStoreSlow` 3.9%).
Issue 0171 only made a block store pay it once per span.

## Fix
`cv_producers` defaults to false: the census runs only under `PSXPORT_PRODUCERS=1`, `census` says it is off
otherwise, and the run-end report is skipped unarmed. `OtAttr::trackStore` keeps the gate open while a watch is
registered, which the armed census used to do for it. Nothing a composed frame needs reads the census: only
`ProducerCensus` reads `OtAttr` spans, and no tool parses its report.

## Proof
Spyro 1, `warp 10`, `gate-teleport 0 0`, HOLD=left, 4 presents at 3840x2160 (psx-spyro
`scratch/judder-pass4/b-on` armed against `c-x4` unarmed): pixel-identical, 0 differing pixels in each. The 141 s
judgement run from boot unarmed (`run2`) composed 3618 in-betweens of 4830 presents with 2 without, against 3606 of
4818 and 2 armed (`base0`).
