---
id: I001
kind: instrument
status: trusted
created: 2026-08-21
---

## Instrument

tools/check_cpp_style.py / CTest cpp_style

## Validated by

Its hermetic --selftest passed 8/8: the clean fixture linted 1/1 TU and excluded a tracked
worktree-deleted C++ file, while missing .clang-tidy, missing/non-Clang compile databases, a touched
TU absent from the database, line-cap growth, and clang-format drift all produced the other answer.

## Known failure modes

(none recorded yet)
