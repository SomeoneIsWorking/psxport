// game/ui/dialog_text_stream.cpp — WIDE-RE DRAFT (2026-07-08, worktree agent-a53f252288693983d).
// UNWIRED, UNVERIFIED — compiles, no override registration, no SBS run. See dialog_text_stream.h
// for the struct-field map.
//
// RE SOURCE: Ghidra headless decompile (tools/decomp.sh, project scratch/ghidra/main_ram,
// scratch/decomp/ui_8007.c) cross-checked line-for-line against generated/shard_6.c's
// gen_func_8007C0D0 and generated/shard_1.c's gen_func_8007D0D0 (the raw recompiled MIPS — the
// CLAUDE.md-mandated ground truth). Every branch below was traced against both.
//
// FINDING (worth keeping — avoids re-deriving this): gen_func_8007C0D0's 0xF8/0xF9 "mode-marker"
// byte handling reads a 6-entry pointer table at 0x80016EBC (indexed by dialog subtype, obj+3) and
// hands the result to `rec_dispatch(c, addr); return;` UNCONDITIONALLY, with NO frame unwind before
// the call. That looks like a real indirect call to a separate function, but it is NOT: the sibling
// function gen_func_8007D0D0 has the EXACT same pattern (table at 0x80017384-ish, 6 entries) and
// there the recompiler DID statically resolve 2 of its 6 entries to local `goto`s within
// gen_func_8007D0D0 itself (`case 0x8007D100u: goto L_8007D100;` / `case 0x8007D13Cu: goto
// L_8007D13C;`), proving the table holds LOCAL CASE LABELS inside the same function, not distinct
// callable functions — the recompiler's static table-recognition just didn't cover every entry in
// FUN_8007C0D0's variant. Ghidra's high-level decompile independently shows the exact same 6
// per-subtype effects as an ordinary switch with NO call at all, which corroborates this. Root
// cause: MIPS jump-table-compiled switch statements look identical to a genuine computed call at
// the raw-instruction level our recompiler works from; it can only prove "local" when it happens to
// also see the label addresses appear elsewhere as branch targets. CONSEQUENCE for future wiring:
// do NOT `rec_dispatch` this table read (there is no real registered function at those raw
// addresses) — reproduce the per-subtype effect directly, as done below.
#include "core.h"
#include "dialog_text_stream.h"
#include <cstdint>

namespace {
constexpr uint32_t OFF_SUBTYPE    = 0x03;  // dialog box subtype (0-5 gated, >=6 plain)
constexpr uint32_t OFF_STATE      = 0x05;  // box top-level state (owned by FUN_8007D594)
constexpr uint32_t OFF_CURSOR     = 0x14;  // script byte-stream cursor
constexpr uint32_t OFF_COL        = 0x2A;  // glyph column counter on current line
constexpr uint32_t OFF_MODE_TIMER = 0x40;  // render-mode timer (applyRenderMode + 0xFC control byte)
constexpr uint32_t OFF_BOX_TIMER  = 0x42;  // secondary per-box timer (0xF8/0xF9 mode-marker, 0xFF term)
constexpr uint32_t G_TEXT_SPEED   = 0x800BF8A3u;  // DAT_800bf8a3 -- text-speed/language mode byte
enum { R_A0 = 4, R_A1 = 5, R_V0 = 2 };
}

// FUN_8007D0D0(obj a0) -- LEAF (gen_func_8007D0D0 has no `sp` descent). Cross-checked verbatim
// against generated/shard_1.c:gen_func_8007D0D0.
void DialogTextStream::applyRenderMode(Core* c) {
  const uint32_t obj = c->r[R_A0];
  uint8_t subtype = c->mem_r8(obj + OFF_SUBTYPE);
  if (subtype == 0 || subtype == 1) {
    // fall through to the text-speed/language check below
  } else if (subtype >= 2 && subtype <= 5) {
    c->mem_w16(obj + OFF_MODE_TIMER, 1);
    return;
  } else {
    return;  // subtype >= 6: no-op
  }
  uint8_t speed = c->mem_r8(G_TEXT_SPEED);
  if (speed == 0)      c->mem_w16(obj + OFF_MODE_TIMER, 3);
  else if (speed == 1) c->mem_w16(obj + OFF_MODE_TIMER, 2);
  else                 c->mem_w16(obj + OFF_MODE_TIMER, 1);
}

// FUN_8007C0D0(obj a0, mode a1) -> v0. Guest frame MIRRORED per gen_func_8007C0D0: `sp-=32;
// sw s0,0x10(sp); sw ra,0x18(sp); sw s1,0x14(sp)` on entry, symmetric restore on every return.
void DialogTextStream::advanceByte(Core* c) {
  const uint32_t ra = c->r[31], sp = c->r[29], s0 = c->r[16], s1in = c->r[17];
  c->r[29] = sp - 32;
  c->mem_w32(c->r[29] + 16, s0);
  const uint32_t obj = c->r[R_A0];
  c->r[16] = obj;
  c->mem_w32(c->r[29] + 24, ra);
  c->mem_w32(c->r[29] + 20, s1in);
  const uint32_t mode = c->r[R_A1];
  c->r[17] = mode;

  auto epilogue = [&](uint32_t v0) {
    c->r[R_V0] = v0;
    c->r[31] = c->mem_r32(c->r[29] + 24);
    c->r[17] = c->mem_r32(c->r[29] + 20);
    c->r[16] = c->mem_r32(c->r[29] + 16);
    c->r[29] = c->r[29] + 32;
  };
  // 0xFF terminator (LAB_8007c27c): mark the box "done" and return 0.
  auto terminate = [&]() {
    c->mem_w8(obj + OFF_STATE, 2);
    c->mem_w16(obj + OFF_BOX_TIMER, 0xFFFF);
    epilogue(0);
  };
  // "skip this byte, keep scanning" -- the sole loop-continuation path (LAB_8007c24c). Returns
  // true if the caller should re-examine the next byte, false if it hit the 0xFF terminator
  // (in which case `terminate()` has already run the full epilogue).
  auto skipAndContinue = [&]() -> bool {
    c->mem_w32(obj + OFF_CURSOR, c->mem_r32(obj + OFF_CURSOR) + 1);
    c->mem_w8(obj + OFF_COL, (uint8_t)(c->mem_r8(obj + OFF_COL) + 1));
    if (c->mem_r8(c->mem_r32(obj + OFF_CURSOR)) == 0xFF) { terminate(); return false; }
    return true;
  };
  // "byte handled, advance cursor+col by 1, return 1" (LAB_8007c214) -- reached from the
  // glyph/0xC0-0xD0 commit branches and the 0xF8/0xF9 mode-marker dispatch.
  auto commitAdvance = [&]() {
    c->mem_w32(obj + OFF_CURSOR, c->mem_r32(obj + OFF_CURSOR) + 1);
    c->mem_w8(obj + OFF_COL, (uint8_t)(c->mem_r8(obj + OFF_COL) + 1));
    epilogue(1);
  };

  if (c->mem_r8(c->mem_r32(obj + OFF_CURSOR)) == 0xFF) { terminate(); return; }

  for (;;) {
    uint8_t byte = c->mem_r8(c->mem_r32(obj + OFF_CURSOR));
    if (byte < 0xC0) {
      // plain glyph byte -- only acted on on the committing pass (mode==1): arm the render-mode
      // timer via applyRenderMode and transition state to `mode`.
      if (mode != 1) { if (skipAndContinue()) continue; return; }
      c->r[R_A0] = obj; c->r[31] = 0x8007C12Cu; applyRenderMode(c);   // FUN_8007D0D0
      c->mem_w8(obj + OFF_STATE, (uint8_t)mode);
      commitAdvance();
      return;
    }
    if (byte < 250) {              // 0xC0..0xF9
      if (byte < 248) {            // 0xC0..0xF7
        if (byte >= 209) { if (skipAndContinue()) continue; return; }   // 0xD1..0xF7: unhandled here
        // 0xC0..0xD0: same render-mode-arm shape as the glyph-byte commit branch above.
        if (mode != 1) { if (skipAndContinue()) continue; return; }
        c->r[R_A0] = obj; c->r[31] = 0x8007C190u; applyRenderMode(c);   // FUN_8007D0D0
        c->mem_w8(obj + OFF_STATE, (uint8_t)mode);
        commitAdvance();
        return;
      }
      // byte == 0xF8 or 0xF9: box mode-marker, dispatched purely by subtype (obj+3) -- NOT gated
      // by `mode`; ground truth always executes this regardless of committing/peek pass. See the
      // file-header FINDING: this is a per-subtype effect, not a real indirect call.
      switch (c->mem_r8(obj + OFF_SUBTYPE)) {
        case 0: c->mem_w16(obj + OFF_BOX_TIMER, 0x0000); c->mem_w8(obj + OFF_STATE, 2); break;
        case 1: c->mem_w16(obj + OFF_BOX_TIMER, 0xFFFF); c->mem_w8(obj + OFF_STATE, 3); break;
        case 2: c->mem_w16(obj + OFF_BOX_TIMER, 0x003C); c->mem_w8(obj + OFF_STATE, 4); break;
        case 3: case 4: case 5:
          c->mem_w16(obj + OFF_BOX_TIMER, 0x003C); c->mem_w8(obj + OFF_STATE, 5); break;
        default: break;   // subtype >= 6: no writes, straight to the shared commit tail
      }
      commitAdvance();
      return;
    }
    // byte in {250,251,253,254} (0xFF handled at loop entry / inside skipAndContinue already).
    if (byte == 0xFC) {
      if (mode != 1) { if (skipAndContinue()) continue; return; }
      // Commit 0xFC: arm modeTimer=0xF, state=mode, advance cursor ONLY (col is NOT bumped --
      // matches Ghidra's early `return 1;` for this case, distinct from commitAdvance()).
      c->mem_w16(obj + OFF_MODE_TIMER, 0x000F);
      c->mem_w8(obj + OFF_STATE, (uint8_t)mode);
      c->mem_w32(obj + OFF_CURSOR, c->mem_r32(obj + OFF_CURSOR) + 1);
      epilogue(1);
      return;
    }
    if (skipAndContinue()) continue;   // 0xFA/0xFB/0xFD/0xFE: unhandled here, skip and keep scanning
    return;
  }
}
