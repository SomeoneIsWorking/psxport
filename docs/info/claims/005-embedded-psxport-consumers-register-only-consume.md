---
id: C005
kind: claim
status: holds
created: 2026-08-21
tags: cmake,testing
depends: CMakeLists.txt, tools/oracle/CMakeLists.txt, tests/test_cmake_test_ownership.py
reconfirmed: 2026-08-25
verified_at: 2026-08-25 00:33:38
---

## Claim

Embedded psxport consumers register only consumer-owned CTests by default, while standalone and explicit opt-in configures register the complete framework suite.

## Evidence

2026-08-21: tests/test_cmake_test_ownership.py passed 8/8 across real default-OFF and explicit-ON embedded CMake configures; standalone Clang CTest registered 69 tests.

## What would falsify it

A real embedded default configure lists a psxport-owned test, or standalone/explicit opt-in omits a framework gate.

## Re-confirmed 2026-08-21

The generic embedded fixture passed both default-OFF and explicit-ON; a fresh Clang Crash Bash
scaffold listed exactly its two intended tests and passed 2/2. Post-integration standalone Clang
CTest passed cmake_test_ownership with 74 tests registered only in the standalone build.

## Re-confirmed 2026-08-22

Reverified after runtime/painter changes: full Clang framework CTest passes 83/83, including cmake_test_ownership and the embedded/standalone ownership classifiers.

## Re-confirmed 2026-08-22

Post-oracle integration Clang CTest 90/90 passed cmake_test_ownership and the complete standalone framework test inventory.

## Re-confirmed 2026-08-25

After oracle CMake composition and syscall integration, cmake_test_ownership passes in the real Clang build; embedded consumers remain isolated from framework CTests.
