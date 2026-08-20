---
id: C001
kind: claim
status: holds
created: 2026-08-21
tags:
depends: tools/check_cpp_style.py#check_tidy, tests/CMakeLists.txt, .clang-tidy
---

## Claim

The normal cpp_style CTest enforces Clang formatting, 1,200/default plus shrink-only legacy size caps, and clang-tidy over every first-party C++ TU represented in the real Clang compile database.

## Evidence

2026-08-21 Clang configure/build succeeded; tools/check_cpp_style.py reported 292 formatted/size-checked files and clang-tidy checked 153 of 156 first-party C++ TUs, naming the 3 non-CMake tools; full CTest passed 66/66.

## What would falsify it

Any cpp_style run that accepts format drift, cap growth, a non-Clang compile command, a missing
touched TU, attempts to lint a worktree-deleted path, or checks zero TUs on this non-empty clean tree
falsifies it.
