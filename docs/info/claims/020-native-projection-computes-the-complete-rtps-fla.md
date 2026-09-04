---
id: C020
kind: claim
status: holds
created: 2026-08-22
tags: native-projection,gte
depends: runtime/psx/native_projection.cpp, tests/test_native_projection.cpp
---

## Claim

native_projection computes the complete RTPS FLAG word from explicit fixed-affine and projection inputs without bound GTE state

## Evidence

Clang 22 standalone build plus ctest 83/83; test_native_projection 5/5 suites and 5,402 checks differentially compare 1,792 RTPS cases including sf/lm modes, zero/overflow FLAG controls, and a corrupted-FLAG negative control

## What would falsify it

an isolated vendor RTPS case disagrees on FLAGS for the same FixedAffine, ProjectionParams, ModelVertex, sf, and lm inputs
