---
id: C001
kind: claim
status: holds
created: 2026-08-21
tags:
depends: tools/check_cpp_style.py#check_tidy, tests/CMakeLists.txt, .clang-tidy
reconfirmed: 2026-08-21
verified_at: 2026-08-21 11:09:16
---

## Claim

The normal cpp_style CTest enforces Clang formatting, 1,200/default plus shrink-only legacy size caps, and clang-tidy over every first-party C++ TU represented in the real Clang compile database.

## Evidence

2026-08-21 Clang configure/build succeeded; tools/check_cpp_style.py reported 293 formatted/size-checked files and clang-tidy checked 154 of 157 first-party C++ TUs, naming the 3 non-CMake tools; full CTest passed 67/67.

## What would falsify it

Any cpp_style run that accepts format drift, cap growth, a non-Clang compile command, a missing
touched TU, attempts to lint a worktree-deleted path, or checks zero TUs on this non-empty clean tree
falsifies it.

## Re-confirmed 2026-08-21

Post-integration standalone Clang CTest passed 74/74; cpp_style passed the full format, structure, and compile-backed clang-tidy corpus.
