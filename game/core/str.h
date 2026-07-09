// game/core/str.h — PC-native generic guest-string utility leaves.
//
// WIDE-RE TIER DRAFT (2026-07-09) — UNWIRED / UNVERIFIED. No override registration, no SBS run.
// Faithful hand-transcription of the recompiled body only; the wiring step (moving this to the
// frontier tier) must diff it line-by-line against `gen_func_80079528` again before trusting it
// (see docs/fleet-workflow.md §9).
//
// PROPER OOP: static leaf (no per-Core state needed — a pure guest-memory scan), called as
// `Str::length(c, addr)`. Mirrors the pattern of `Font::measureLineWidth` (game/ui/font.h).
#pragma once
#include <stdint.h>
class Core;

class Str {
public:
  // length(c, addr): FUN_80079528 — plain NUL-terminated C-string length. One of the two hottest
  //   unowned leaves in the game (~4235 dispatches / 600 frames of free-roam) — a generic strlen()
  //   called from all over the overlay set (menu/UI/text/asset-name code), not subsystem-specific.
  //   Disas 0x80079528..0x8007954C (tools/disas.py 0x80079528 --all 20): a plain byte scan, no
  //   sub-calls, no stack frame (leaf, sp untouched). Ghidra garbled this range (folded a second,
  //   UNREACHABLE function's bytes — 0x80079554 onward, no dispatch entry, no caller anywhere in
  //   generated/ — into the same symbol; recomp `gen_func_80079528` is instruction-exact ground
  //   truth and confirms only the strlen loop is reachable via the func_80079528 entry point).
  static uint32_t length(Core* c, uint32_t addr);
};
