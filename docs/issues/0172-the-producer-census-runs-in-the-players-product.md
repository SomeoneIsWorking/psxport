# 0172: the producer census runs in the player's product

Status: open, the operator's call. Owner: `runtime/psx/config/config.cpp` `cv_producers`, `runtime/psx/debug/ot_attr.h`.

## Observed
Review of psx-spyro issue 0196 pass 3: the boot-handover field's profile (issue 0171) spent samples in
`OtAttr::trackStoreSlow` in a run with no diagnostic channel enabled.

## Cause
`PSXPORT_PRODUCERS` defaults to true (`cv_producers`), and `Game::wireRuntimeMembers` arms
`g_producer_census_armed` from it, so `OtAttr::trackStore` books every guest store in ordinary play. `ot_attr.h`
records its price on an unpaced 3D field: 18% of the run (`resolveClaimedFrame` 13.3% + `trackStoreSlow` 3.9%).
Issue 0171 only made a block store pay it once per span.

## Proper fix
Arm the census only where it is read: diagnostic runs and the census tools (`agent_environment` or the tool that
reads it sets `PSXPORT_PRODUCERS=1`), with the player's default off. `ot_attr.h` notes the census was armed by
default on purpose because ordinary play is the data source that matters, so this is left to the operator.
