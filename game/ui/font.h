// class Font — PC-native engine FONT / TEXT init subsystem.
//
// PROPER OOP: one instance per Core, owned by Engine (`eng(c).font`). Callers reach the
// init entry as `eng(c).font.init()`. Back-pointer `core` wired at Core construction time
// (same pattern as ScreenFade / GraphicsBind / etc.).
//
// SCOPE: the FUN_80075130 boot-time font/text init orchestrator + its three engine-state
// leaves it directly owns (font-bank select, font-bank2 store, glyph-class table fill). The
// libgpu/sound leaves (draw-env / FntLoad / FntOpen setup, sound bring-up) stay dispatched
// through rec_dispatch in-context — see font.cpp for the full call chain.
//
// Was the free functions ov_font_init / ov_font_bank_select / ov_font_bank2_store /
// ov_font_glyphclass_fill in font.cpp — each taking its argument via MIPS taxi
// parameter c->r[4]. Now real instance methods with explicit typed arguments.
#pragma once
#include <stdint.h>
class Core;

class Font {
public:
  Core* core = nullptr;

  // init: FUN_80075130 — font / text system init orchestrator (no args, no return). Called
  //   once from native_boot.cpp's game_init prefix. Owns the direct engine-state writes +
  //   the 3 engine leaves; rec_dispatches the 8 libgpu/sound leaves in-order, in-context.
  //   The stack-struct dance (sp -= 48, sp+16..sp+26 populated for the FntOpen dispatches)
  //   is preserved verbatim — the dispatched callees read that struct.
  void init();

  // bankSelect(bank): FUN_800963a0 — font-bank selector. If ((bank-1)&0xff) < 24, stores the
  //   bank byte at 0x80105cec and returns the sign-extended low byte; otherwise returns -1.
  //   Returned via v0 (c->r[2]) so retained-PSX callers still work.
  void bankSelect(uint32_t bank);

  // bank2Store(bank): FUN_80096370 — font-bank2 store (single sb to 0x80105d28). Leaf; the
  //   recomp body did not set v0, so this doesn't either.
  void bank2Store(uint32_t bank);

  // glyphClassFill(cls): FUN_800752b4 — glyph-class table fill. Writes an exclusive cascade
  //   (i>=24-cls -> 4; i>=16-cls -> 1; i>=12-cls -> 3; i>=8-cls -> 2; else 0) into 24 entries
  //   at 0x800be238 + i*12 + 8 (u8). Returns the loop-exit count in v0 (caller ignores).
  void glyphClassFill(int32_t cls);

  // measureLineWidth(str): FUN_80073750 — string measurer. Walks a NUL-terminated guest C-string
  //   counting a "prefix" length (chars before the FIRST '\n') and a "suffix" length (the first
  //   '\n' itself + every char after it, cumulative — NOT reset on any later '\n'). If no '\n' was
  //   ever seen, returns the plain length (prefix count) as a POSITIVE value. If a '\n' was seen,
  //   returns -(max(prefix, suffix)) — a NEGATIVE value the caller (e.g. game/ai/
  //   beh_cube_text_spawn.cpp's "cube letters" text actor) uses as a multi-line signal.
  //   ABI NOTE: the guest body's loop cursor IS its a0 parameter (`addiu a0,a0,1` in the loop),
  //   so on return a0 == the address of the string's NUL terminator — a register LEFTOVER the
  //   caller relies on for its next call's implicit a0 (beh_cube_text_spawn.cpp's "GOTCHA" comment,
  //   FUN_8007aae8 carries a0 through unset). This native version reproduces that leftover by
  //   writing c->r[4] = end-of-string, exactly matching what a dispatched call would leave.
  //   Body from disas 0x80073750..0x80073798 (no sub-calls; a plain byte scan).
  static int32_t measureLineWidth(Core* c, uint32_t strAddr);

  // drawText(x, y, w, str, color): FUN_80079374 — WIDE-RE TIER DRAFT (2026-07-09), re-verified and
  //   WIRED. A thin arg-packing wrapper around the still-unowned font/glyph emitter FUN_80078CA8
  //   (see docs/engine_re.md "FUN_80078ca8" section — full string-draw engine with cursor state at
  //   scratchpad 0x1F800000..0x1F80001F; NOT ported here, only reached via rec_dispatch, same as
  //   font.cpp's other KEPT libgpu/sound callees).
  //   Disas 0x80079374..0x800793C0 (tools/disas.py 0x80079374 --all 40) / gen_func_80079374
  //   (generated/shard_7.c:11490):
  //     a0' = (int16)x | (y << 16)              — packed {y:hi16, x:lo16, x sign-extended} vertex
  //     a1' = 0x00100008                          — CONSTANT arg (original a1/w is DISCARDED — the
  //                                                  guest code never uses the caller's w for this
  //                                                  slot; confidence: CONFIRMED from the gen body,
  //                                                  the incoming a1 is entirely overwritten before
  //                                                  use — semantic ROLE of 0x100008 unconfirmed,
  //                                                  likely a packed 16/8 draw-attribute pair)
  //     a2' = (int16)w                            — sign-extended low16(w) ONLY.
  //   BUG FIX (verify pass, 2026-07-10): the original wide-RE draft invented a 6th "h" parameter and
  //   OR'd `h << 16` into a2' ("packed size {w:lo16, h:hi16}"). Traced every call site
  //   (generated/shard_0.c:11403, shard_6.c:13960/13987/14010/14031, shard_7.c:11935/12062/12102/
  //   12132/12143, shard_2.c:10751/10776, shard_5.c:13279/13303/13363/13371/13394) — the REAL guest
  //   ABI is 5 args: a0=x, a1=y, a2=w, a3=str (a pointer, e.g. shard_0.c:11405 `r7 =
  //   mem_r32(r16+12)`), stack[+16]=color. `a3`/r7 is NEVER referenced inside gen_func_80079374's
  //   body at all — it passes through to the tail call UNTOUCHED, consistent only with a3 being the
  //   string pointer, not an "h" register the body would fold into a2'. There is NO h parameter in
  //   the real function; the fabricated one corrupted a2' whenever nonzero. Fixed at both the
  //   signature and the a2' computation.
  //     a3' = str (unchanged)
  //     stack[16] = color (5th arg, passed on the caller's stack at +48)
  //     *0x1F800180 (u16) = 32                    — scratchpad write BEFORE the call (role
  //                                                  unconfirmed; a fixed constant, not derived
  //                                                  from any argument)
  //     tail-calls FUN_80078CA8(a0', a1', a2', a3', color) via rec_dispatch, ra = 0x800793B4
  //   x/y/w are taken as raw guest register values (already packed by the CALLER into low16 of
  //   each 32-bit arg — this wrapper does NOT decode them into separate ints, it reproduces the
  //   exact bit operations the guest performs, since sign-extension order matters for negative
  //   on-screen coordinates).
  static void drawText(Core* c, int32_t x, int32_t y, int32_t w, uint32_t str, uint32_t color);

  // drawTextSmall(x, y, w, str, color): FUN_80079324 — SIBLING of drawText (0x80079374). Identical
  //   arg-packing wrapper around the same font/glyph emitter FUN_80078CA8, differing ONLY in the two
  //   fixed constants it feeds the emitter (confirmed byte-for-byte against gen_func_80079324,
  //   generated/shard_6.c, vs gen_func_80079374):
  //     a1'          = 0x00080008 instead of 0x00100008 — {w:8, h:8}; low16=8 lands in the glyph
  //                    scratch struct's +16 field (per-glyph width scale), high16=0x08 is the glyph
  //                    HEIGHT (drawText passes 0x10=16). So this entry draws HALF-HEIGHT (8x8) text.
  //     *0x1F800180  = -32 instead of +32 — the fixed per-call horizontal-advance scratchpad value the
  //                    emitter reads back in its glyph-draw arm (role unconfirmed; a fixed constant,
  //                    not derived from any argument).
  //   Everything else is identical: a0' = (int16)x | (y<<16) packed vertex, a2' = (int16)w, a3' = str,
  //   stack[+16] = color; mirrors the guest frame (sp-=32, ra spilled at sp+24) and tail-calls
  //   FUN_80078CA8 with ra = 0x80079364. See drawText's doc above for the full ABI trace.
  static void drawTextSmall(Core* c, int32_t x, int32_t y, int32_t w, uint32_t str, uint32_t color);

  // glyphEmit(): FUN_80078CA8 — the font/glyph EMITTER drawText() tail-calls. WIDE-RE TIER DRAFT
  //   (2026-07-10, disjoint band), UNWIRED/UNVERIFIED (docs/fleet-workflow.md §6/§9 — no override
  //   registration, no SBS run; needs the line-by-line re-verify §9 requires before wiring).
  //
  //   A prior wave filed this as "large, separate scope, not drafted" at 403 gen-C lines. Re-read
  //   this wave: gen_func_80078CA8 (generated/shard_5.c:12298) has a REAL `return` at gen-C line 210
  //   with NO label between there and the closing brace — everything from line 211 to the end (192
  //   more lines, a hand-unrolled 20-word struct copy + two more OT-chain packet builds) is
  //   UNREACHABLE dead code (same "shard-grouping artifact" pattern documented elsewhere in this
  //   file's history, e.g. channelEnvelopeRampTick's trailing func_800922A0). The REAL body is ~180
  //   live lines: a null-terminated string walk with a per-CHARACTER-BYTE switch over a FIXED SCRATCH
  //   STRUCT at guest 0x800C0000 (NOT scratchpad — corrects an earlier doc note that placed "cursor
  //   state" at 0x1F800000..0x1F80001F; the ONLY scratchpad touch in the live body is a single read
  //   of 0x1F800180, the fixed per-CALL horizontal-advance value drawText() writes as a constant 32
  //   before every call — used inside the glyph-draw arm's width/height compute, NOT as the cursor
  //   step itself; the shared "advance cursor" tail below always steps by a literal 8, independent of
  //   that scratchpad value).
  //
  //   ABI: a0(r4)=packed vertex {x:lo16, y:hi16} (drawText's a0'), a1(r5)=drawText's 0x00100008
  //   constant (LOW16=8 lands in the scratch struct's +16 field and is later read back as a per-
  //   glyph width-scale factor — this is the "role" a prior pass left unconfirmed; HIGH16=0x10 is
  //   never independently read in the live body), a2(r6)=packed size {w:lo16, h:hi16}, a3(r7)=str
  //   pointer, stack[+16 of THIS frame after its own sp-=56, i.e. the caller's stack[+16] per o32
  //   convention]=color (matches drawText's `mem_w32(c->r[29]+16, color)`).
  //
  //   Per-byte dispatch (scratch struct base = 0x800C0000, offsets below are +that; register-literal
  //   transcription revealed the byte/table mapping is carried through MIPS delay-slot-merged
  //   compare chains — worth stating explicitly since it's easy to misread from the gen C alone):
  //     0x20 (' ')  -> falls straight to the shared "advance cursor by literal 8" tail (struct+8 +=8)
  //     0x0A ('\n') -> LINE BREAK: struct+8 reset to (vertex-arg x & 0xFFF), struct+10 (cursor-y) +=
  //                    struct+18 (a "line height" field — never WRITTEN anywhere in this function, so
  //                    it must be initialized by a caller-adjacent leaf this wave did not trace; role
  //                    inferred from usage, not independently confirmed)
  //     0x01        -> calls still-unowned FUN_80078988 with tablePtr = 0x80010000+28072, then the
  //                    shared "advance cursor by 8" tail
  //     0x02        -> same, tablePtr = 0x80010000+28076
  //     0x03        -> same, tablePtr = 0x80010000+28068
  //     0x04        -> same, tablePtr = 0x80010000+28064
  //     any other non-zero byte (every ordinary printable glyph) -> the GLYPH-DRAW arm: computes
  //                    per-glyph pixel width/height into struct+12/+13 from the scratchpad advance
  //                    value (0x1F800180) and struct+16/+18, then PREPENDS a 4-word GP0 packet at the
  //                    packet-pool bump pointer (0x800BF544, same pool every other render leaf in
  //                    game/render/ uses — see PKT_POOL_PTR in game/render/submit.cpp) into the OT
  //                    bucket at *(0x800F0000-10040 + colorArg*4), tagged with draw-mode bit
  //                    0x04000000, then falls into the same shared "advance cursor by 8" tail.
  //     FUN_80078988 calls (byte 0x01/0x02/0x03/0x04 arms) each pass (cursorX, cursorY, w, tablePtr)
  //       — role: draws a box/rule primitive (underline / box variants selected by which literal
  //       table pointer is passed); STILL UNOWNED, stays rec_dispatch (out of this wave's band).
  //   Tail (after the string NUL): builds a FINAL OT-chained packet via the ALREADY-owned
  //     func_80083DE0 (0x80083DE0, game/render/wide_re_libgpu_leaves.cpp — draw-mode/texwin packet-
  //     header builder, dispatched here since it's process-globally wired) plus a second packet
  //     tagged 0x02000000, both linked into the same OT bucket, then advances the pool pointer by
  //     12 more bytes and returns `(int16)size.w - color` in r2 (a value the caller — drawText() —
  //     currently discards; CONFIRMED from drawText's own body, it never reads r2 after the call).
  //
  //   Guest-stack frame MIRRORED (sp-56, spill ra/s0-s5 at their RE'd offsets: r16..r21 = s0..s5).
  //   Kept register-literal with goto/labels named after the guest addresses, same rationale as the
  //   sequencer leaves this wave also drafted — dense character-class branching with a shared tail
  //   (L_80078F70/L_80078F78 reached from multiple arms).
  //
  //   LOW-MEDIUM confidence: control flow + the scratch-struct BASE ADDRESS correction are solid
  //   (direct re-read of the gen body); individual field ROLES beyond what's stated above (esp.
  //   struct+18 "line height", which is never WRITTEN anywhere in this function — it must be
  //   initialized by a caller-adjacent leaf this wave did not trace) are inferred, not confirmed.
  static void glyphEmit(Core* c);

  // Host half of glyphEmit's dual-emit: push the just-built glyph SPRT to the native render queue
  // (RQ_HUD) so pc_render draws text without transcribing the guest OT. Reads the completed scratch
  // struct at 0x800C0000; writes no guest byte; no-op under psx_render/oracle.
  static void glyphQueuePush(Core* c);
};
