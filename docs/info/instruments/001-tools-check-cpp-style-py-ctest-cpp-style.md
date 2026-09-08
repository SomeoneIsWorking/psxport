---
id: I001
kind: instrument
status: trusted
created: 2026-08-21
---

## Instrument

tools/check_cpp_style.py / CTest cpp_style

## Validated by

Its hermetic --selftest passes 24/24: the clean fixture lints 1/1 TU and excludes a tracked
worktree-deleted C++ file, while a seeded external-game architecture reference, a deleted framework
path, missing .clang-tidy, a missing compile database, a touched TU absent from the database,
line-cap growth, a stale non-ratcheted cap, and clang-format drift all produced the other answer.

## Known failure modes

An unsupported `-hide-progress` option made older LLVM runners print help and exit zero without
linting. The runner invocation now omits that cosmetic option. The negative self-test exercises
older argument parsing and requires a real undeclared-identifier diagnostic from a GCC compile
command; compiler names do not determine whether a compile database is accepted.

The reference scanner distinguishes a declared forbidden-token tuple from a dependency reference.
Its discriminator rejects an actual reference elsewhere in the same policy file.
