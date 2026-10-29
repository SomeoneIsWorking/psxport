// testprog.c — the framework test program: scenes of planted draws whose owner rows are known.
// Built freestanding for mipsel (tools/testprog/build_testprog.py); no SDK, no BIOS calls.

typedef unsigned int u32;
typedef int s32;
typedef unsigned short u16;
typedef short s16;

#define GP0 (*(volatile u32 *)0x1F801810)
#define GP1 (*(volatile u32 *)0x1F801814)
#define D2_MADR (*(volatile u32 *)0x1F8010A0)
#define D2_BCR (*(volatile u32 *)0x1F8010A4)
#define D2_CHCR (*(volatile u32 *)0x1F8010A8)
#define D6_MADR (*(volatile u32 *)0x1F8010E0)
#define D6_BCR (*(volatile u32 *)0x1F8010E4)
#define D6_CHCR (*(volatile u32 *)0x1F8010E8)
#define DPCR (*(volatile u32 *)0x1F8010F0)

#define OT_LENGTH 16
#define POOL_WORDS 512
#define SCENE_FRAMES 240
#define NO_SCENE 0xFFFFFFFFu

// LoopingOt ends the session with a refused draw-list cycle and runs only when requested.
enum Scene {
  OwnedSquare,
  UnownedMoving,
  MmioPort,
  BlockDmaPort,
  ReusedSlot,
  AmendedSquare,
  CycledSceneCount,
  LoopingOt = CycledSceneCount,
  SceneCount
};

// An owned object: what square_draw reads, and what the host producer saves.
struct Square {
  s32 x;
  s32 y;
  u32 angle; // 1/64 turns
  u32 color;
};

u32 ot[2][OT_LENGTH];
u32 pool[2][POOL_WORDS];
u32 *cursor;
u32 buffer;
u32 frame_counter;
u32 frame_fields;
u32 scene;
u32 scene_frames;
u32 requested_scene = NO_SCENE;
struct Square squares[2];
u32 dma_tile[4];
// LO and HI of each divide case, in testprog/host/debug_menu.h kDivideCases order.
u32 divide_results[7][2];

// Q12 sine of a 1/64 turn, the first quarter.
const s16 sine_quarter[17] = {
    0, 402, 799, 1189, 1567, 1931, 2276, 2598, 2896, 3166, 3406, 3612, 3784, 3920, 4017, 4076, 4096};

void vsync(u32 mode);
void square_draw(struct Square *square);

// The facts the host reads at the load address + 12 (testprog/host/facts.h): magic, VSync, the owned drawer, the OT,
// its length, the packet cursor, the sine table and the words the frame driver and the debug menu use.
__asm__(".section .text.0start,\"ax\",@progbits\n"
        ".set noreorder\n"
        ".globl _start\n"
        "_start:\n"
        "  lui $sp, 0x801F\n"
        "  j cstart\n"
        "  ori $sp, $sp, 0xFF00\n"
        "  .word 0x54505350\n"
        "  .word vsync\n"
        "  .word square_draw\n"
        "  .word ot\n"
        "  .word 16\n"
        "  .word cursor\n"
        "  .word sine_quarter\n"
        "  .word frame_counter\n"
        "  .word frame_fields\n"
        "  .word scene\n"
        "  .word requested_scene\n"
        "  .word buffer\n"
        "  .word divide_results\n"
        ".set reorder\n"
        ".text\n");

// Divides with the compiler's guard sequences the recompiler strips, and bare: (a0 / a1) -> LO, HI at a2.
void divide_guarded(s32 dividend, s32 divisor, u32 *out);
void divide_guarded_unsigned(u32 dividend, u32 divisor, u32 *out);
void divide_bare(s32 dividend, s32 divisor, u32 *out);
void divide_bare_unsigned(u32 dividend, u32 divisor, u32 *out);
__asm__(".text\n"
        ".set noreorder\n"
        ".set noat\n"
        ".globl divide_guarded\n"
        "divide_guarded:\n"
        "  .word 0x0085001A\n" // div a0, a1
        "  bnez $a1, 1f\n"
        "  nop\n"
        "  break 7\n"
        "1:\n"
        "  li $at, -1\n"
        "  bne $a1, $at, 2f\n"
        "  lui $at, 0x8000\n"
        "  bne $a0, $at, 2f\n"
        "  nop\n"
        "  break 6\n"
        "2:\n"
        "  mflo $t0\n"
        "  mfhi $t1\n"
        "  sw $t0, 0($a2)\n"
        "  jr $ra\n"
        "  sw $t1, 4($a2)\n"
        ".globl divide_guarded_unsigned\n"
        "divide_guarded_unsigned:\n"
        "  .word 0x0085001B\n" // divu a0, a1
        "  bnez $a1, 3f\n"
        "  nop\n"
        "  break 7\n"
        "3:\n"
        "  mflo $t0\n"
        "  mfhi $t1\n"
        "  sw $t0, 0($a2)\n"
        "  jr $ra\n"
        "  sw $t1, 4($a2)\n"
        ".globl divide_bare\n"
        "divide_bare:\n"
        "  .word 0x0085001A\n"
        "  mflo $t0\n"
        "  mfhi $t1\n"
        "  sw $t0, 0($a2)\n"
        "  jr $ra\n"
        "  sw $t1, 4($a2)\n"
        ".globl divide_bare_unsigned\n"
        "divide_bare_unsigned:\n"
        "  .word 0x0085001B\n"
        "  mflo $t0\n"
        "  mfhi $t1\n"
        "  sw $t0, 0($a2)\n"
        "  jr $ra\n"
        "  sw $t1, 4($a2)\n"
        ".set at\n"
        ".set reorder\n");

// A MIPS divide never traps: n / 0 gives LO = -1 or 1 by n's sign and HI = n; INT_MIN / -1 gives INT_MIN, 0.
static void divide_cases(void) {
  divide_guarded_unsigned(0x2D6FFCu, 0u, divide_results[0]);
  divide_guarded(5, 0, divide_results[1]);
  divide_guarded(-5, 0, divide_results[2]);
  divide_guarded((s32)0x80000000u, -1, divide_results[3]);
  divide_bare(7, -2, divide_results[4]);
  divide_bare_unsigned(7u, 0u, divide_results[5]);
  divide_bare((s32)0x80000000u, -1, divide_results[6]);
}

static s32 sine(u32 angle) {
  const u32 a = angle & 63u;
  if (a <= 16u) {
    return sine_quarter[a];
  }
  if (a <= 32u) {
    return sine_quarter[32u - a];
  }
  if (a <= 48u) {
    return -sine_quarter[a - 32u];
  }
  return -sine_quarter[64u - a];
}

static u32 xy(s32 x, s32 y) {
  return ((u32)y << 16) | ((u32)x & 0xFFFFu);
}

static u32 *packet(u32 words) {
  u32 *p = cursor;
  cursor += words + 1u;
  return p;
}

static void link(u32 *p, u32 words, u32 depth) {
  u32 *entry = &ot[buffer][depth];
  p[0] = (words << 24) | (*entry & 0x00FFFFFFu);
  *entry = (u32)p & 0x00FFFFFFu;
}

u32 vsync_calls;

// The host binds libetc VSync here, ending the guest turn at each field; the body only runs without it.
__attribute__((noinline)) void vsync(u32 mode) {
  vsync_calls += 1u + mode;
}

// The owned drawer: a flat square turned by `angle`, one POLY_F4, the host producer's Arg::A0 object.
__attribute__((noinline)) void square_draw(struct Square *square) {
  static const s32 kCorners[4][2] = {{-24, -24}, {24, -24}, {-24, 24}, {24, 24}};
  const s32 s = sine(square->angle);
  const s32 c = sine(square->angle + 16u);
  u32 *p = packet(5);
  p[1] = 0x28000000u | square->color;
  for (u32 i = 0; i < 4u; i++) {
    const s32 cx = kCorners[i][0];
    const s32 cy = kCorners[i][1];
    p[2 + i] = xy(square->x + ((cx * c - cy * s) >> 12), square->y + ((cx * s + cy * c) >> 12));
  }
  link(p, 5, 4);
}

// Unowned and still: the same TILE every frame, so the interval composes.
__attribute__((noinline)) void backdrop_draw(void) {
  u32 *p = packet(3);
  p[1] = 0x60202040u;
  p[2] = xy(0, 200);
  p[3] = xy(320, 40);
  link(p, 3, OT_LENGTH - 1);
}

// Unowned and moving: a TILE written by guest code no producer declares.
__attribute__((noinline)) void drifter_draw(void) {
  u32 *p = packet(3);
  p[1] = 0x6000C0FFu;
  p[2] = xy(20 + (s32)(frame_counter * 3u % 260u), 40);
  p[3] = xy(32, 32);
  link(p, 3, 6);
}

// Drawn through GP0 itself after the list: no packet in RAM, so the Port owner names it.
__attribute__((noinline)) void mmio_bar_draw(void) {
  GP0 = 0x60FF8000u;
  GP0 = xy(20 + (s32)(frame_counter * 2u % 260u), 120);
  GP0 = xy(40, 16);
}

// Drawn by a block DMA of a TILE: the Port owner again, through the other entry.
__attribute__((noinline)) void dma_bar_draw(void) {
  dma_tile[0] = 0x6080FF80u;
  dma_tile[1] = xy(20 + (s32)(frame_counter * 4u % 260u), 150);
  dma_tile[2] = xy(40, 16);
  D2_MADR = (u32)dma_tile;
  D2_BCR = 3u | (1u << 16);
  D2_CHCR = 0x01000201u;
}

// A full-screen semi-transparent TILE in a ring of buckets: bucket 3 links back to bucket 10, so the list never ends.
__attribute__((noinline)) void looping_ot_draw(void) {
  u32 *p = packet(3);
  p[1] = 0x62404040u;
  p[2] = xy(0, 0);
  p[3] = xy(320, 240);
  link(p, 3, 8);
  ot[buffer][3] = (u32)&ot[buffer][10] & 0x00FFFFFFu;
}

// Unowned code storing over the owned square's colour word before the list is submitted: the Overwrote case.
__attribute__((noinline)) void amend_draw(void) {
  u32 *p = (u32 *)(0x80000000u | (ot[buffer][4] & 0x00FFFFFFu));
  p[1] = 0x28000000u | ((frame_counter * 8u) & 0xFFu) << 16 | 0x40E0u;
}

static void clear_ot(void) {
  D6_MADR = (u32)&ot[buffer][OT_LENGTH - 1];
  D6_BCR = OT_LENGTH;
  D6_CHCR = 0x11000002u;
}

static void draw_env(void) {
  const u32 top = buffer * 256u;
  GP0 = 0xE3000000u | (top << 10);
  GP0 = 0xE4000000u | ((top + 239u) << 10) | 319u;
  GP0 = 0xE5000000u | (top << 11);
  GP0 = 0x02000000u;
  GP0 = top << 16;
  GP0 = (240u << 16) | 320u;
}

static void submit(void) {
  D2_MADR = (u32)&ot[buffer][OT_LENGTH - 1];
  D2_BCR = 0;
  D2_CHCR = 0x01000401u;
}

static void update(void) {
  if (requested_scene != NO_SCENE) {
    scene = requested_scene % SceneCount;
    requested_scene = NO_SCENE;
    scene_frames = 0;
  } else if (++scene_frames >= SCENE_FRAMES) {
    scene = (scene + 1u) % CycledSceneCount;
    scene_frames = 0;
  }
  // The owned square's scene alternates 2- and 3-field frames; the rest run at 2.
  frame_fields = scene == OwnedSquare ? 2u + (frame_counter & 1u) : 2u;
  squares[0].angle += 1u;
  squares[0].x = 100 + (s32)(frame_counter % 120u);
  squares[0].y = 100;
  squares[0].color = 0x0040E0u;
  // The reused slot: every 12 frames a different object takes slot 1, far from where the last one was.
  if (frame_counter % 12u == 0u) {
    squares[1].x = 60 + (s32)(frame_counter * 37u % 200u);
    squares[1].y = 60 + (s32)(frame_counter * 53u % 120u);
    squares[1].color ^= 0x00FFFFu;
  }
  squares[1].angle += 2u;
  squares[1].x += 1;
}

void cstart(void) {
  GP1 = 0x00000000u;
  GP1 = 0x08000001u; // 320x240 NTSC 15-bit
  GP1 = 0x06C60260u;
  GP1 = 0x07040010u;
  GP1 = 0x04000002u;
  GP1 = 0x03000000u;
  GP0 = 0xE1000400u;
  DPCR = 0x08000800u | DPCR;
  squares[1].color = 0xE0E000u;
  divide_cases();
  for (;;) {
    ++frame_counter;
    buffer ^= 1u;
    cursor = pool[buffer];
    update();
    clear_ot();
    backdrop_draw();
    switch (scene) {
    case OwnedSquare:
      square_draw(&squares[0]);
      break;
    case UnownedMoving:
      drifter_draw();
      break;
    case ReusedSlot:
      square_draw(&squares[1]);
      break;
    case AmendedSquare:
      square_draw(&squares[0]);
      amend_draw();
      break;
    case LoopingOt:
      looping_ot_draw();
      break;
    default:
      break;
    }
    draw_env();
    submit();
    if (scene == MmioPort) {
      mmio_bar_draw();
    } else if (scene == BlockDmaPort) {
      dma_bar_draw();
    }
    for (u32 field = 0; field < frame_fields; field++) {
      vsync(0);
    }
    GP1 = 0x05000000u | ((buffer * 256u) << 10);
  }
}
