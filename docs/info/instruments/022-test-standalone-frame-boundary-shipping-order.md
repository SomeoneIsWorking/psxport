---
id: I022
kind: instrument
status: trusted
created: 2026-08-25
---

## Instrument

`Tomba2Engine/tools/verify_native_frame_contract.py` title-owned frame-order gate

## Validated by

The validator names Tomba's measured frame-order tokens in
`Tomba2Engine/game/core/frame_driver.cpp`: presentation commit, capture reset, armed cold warp, then
the scheduler step. Its selftest accepts that order and produces the opposite answer for the reversed
fixture. The product check reads the shipping title driver, so it cannot certify a separate helper
while the title runs different code.

## Known failure modes

The static order gate proves source order, not that a runtime branch actually reaches every token.
The bounded consumer trace remains required to prove the pending old scene was presented before guest
state replacement. This instrument belongs to Tomba! 2; it makes no claim about other titles' frame
orders.
