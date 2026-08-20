---
id: I006
kind: instrument
status: trusted
created: 2026-08-21
---

## Instrument

tools/oracle/crossvalidate_crt0.py decoded-call boundary comparator

## Validated by

Its 5-case selftest reports a wrong captured target as 5 agree/1 disagree, an unrelated A(39h) exit as unseen, and a missing target as no boundary; a real 50,000-step Crash run also refused exit 2 before the decoded call.

## Known failure modes

(none recorded yet)
