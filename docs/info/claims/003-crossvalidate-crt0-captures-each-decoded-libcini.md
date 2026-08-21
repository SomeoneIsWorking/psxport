---
id: C003
kind: claim
status: holds
created: 2026-08-21
tags:
depends: tools/oracle/crossvalidate_crt0.py#main, tools/oracle/oracle_trace.c#main
reconfirmed: 2026-08-21
verified_at: 2026-08-21 11:09:16
---

## Claim

crossvalidate_crt0 independently captures the first executed jal boundary and verifies that it is the decoded libcInit call instead of assuming the first mapped-text exit is that call.

## Evidence

Real Crash 1 SCUS_949.00 (SHA-256 `aabf1464f90b2e0b81e712b77aebbdb88f303b16ce830535e2b0cd886ee280f2`) reached in-image libcInit 0x80011A18 at oracle step 57910 and agreed 6/6; real CTR SCUS_944.26 (SHA-256 `7b4aac0bf2f6310984e599295df17b457da5a23b270c20200cefef6079efb838`) retained the stock A(39h) control at steps 92375/92378 and agreed 7/7; a 50000-step Crash run refused exit 2; crossvalidate_crt0.py --selftest passed 5/5.

## What would falsify it

A real executable reaches its decoded libcInit but the trace captures another call/argument state, an unreached target returns success, or the CTR A(39h) control ceases to agree.

## Re-confirmed 2026-08-21

Real Crash1 independently captured first jal target 0x80011A18 at step 57910 and agreed 6/6; real
CTR captured 0x80080620 at step 92375 and A(39h) at 92378, agreeing 7/7; the short Crash run refused
exit 2. After the canonical ordinal-capture integration, standalone Clang CTest passed oracle_spike,
crossvalidate_crt0_selftest, and oracle_trace_selftest as part of 74/74.
