# OtAttr frame-loop verdict belongs at run end

Verified 2026-08-13 against Spyro's owned frame loop and the Core-alone smoke.

`OtAttr::stampFrame` used to warn on the first store made before `beginLogicFrame`. That cannot
distinguish a missing frame loop from normal crt0/boot activity before a healthy loop starts. A real
Spyro run printed the failure warning during boot, then advanced through frame 931 correctly.

The framework now counts pre-frame stamps and reports the contract at run end:

- Spyro: `SATISFIED`, frame 931 after 2,197,225 pre-frame stamps.
- Core-alone smoke: `FAILED`, 3 stamps and no declared frame—the intended negative.
- A run with neither stamps nor frames reports `NOT EXERCISED`, never a clean result.

`test_core_store_no_game` drives both classes through the shipping `OtAttr`: a watched store without
a frame is unsatisfied, then `beginLogicFrame(1)` makes the same history satisfied. The full framework
suite passes 45/45.
