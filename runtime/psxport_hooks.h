// psxport hook & override layer (C API: included by the imported mednafen C
// sources). Hooks fire by PC inside the CPU interpreter; each hook carries an
// expected instruction word so overlay-swapped code at the same address can
// never misfire (same principle as the conditional RAM pokes).
//
// A hook returning PSXPORT_HOOK_REDIRECT acts as a native override: the
// interpreter skips the original code and resumes at *redirect_pc (typically
// the caller, GPR[31], with results placed in GPR by the hook).

#ifndef PSXPORT_HOOKS_H
#define PSXPORT_HOOKS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PSXPORT_HOOK_CONTINUE 0 /* run the original instruction normally */
#define PSXPORT_HOOK_REDIRECT 1 /* skip original code, jump to *redirect_pc */

/* gpr = s_cpu.GPR_full (32 GPRs + LO/HI at 32/33). */
typedef int (*psxport_hook_fn)(uint32_t pc, uint32_t* gpr, uint32_t* redirect_pc);

/* Nonzero while any hooks are registered; the interpreter's fast-path gate. */
extern uint32_t psxport_hook_count;

/* Called from the interpreter for every instruction while hooks exist.
   instr = fetched instruction word (signature check). */
int psxport_on_pc(uint32_t pc, uint32_t instr, uint32_t* gpr, uint32_t* redirect_pc);

/* PC coverage (RE aid): while enabled, every executed PC in RAM is marked in
   a 512Kbit bitmap (one bit per word address). */
extern uint8_t* psxport_cov_bitmap; /* NULL = disabled */

/* Instant-CD: data reads/seeks at PC speed (CDDA/XA streaming stays native).
   Consumed by the imported cdc.c. */
extern int psxport_cd_instant;

/* Scoped PC trace ring (PSXPORT_PCTRACE="lo-hi"). Captures executed PCs in range
   for path-diffing a state machine. cpu.c pushes; REPL `pctrace <path>` dumps. */
extern uint32_t psxport_pctrace_hi;
void psxport_pctrace_init(void);
void psxport_pctrace_push(uint32_t pc);
void psxport_pctrace_dump(const char* path);

/* Streaming call trace (PSXPORT_CALLTRACE="lo-hi[:path]"): logs jal/jalr targets in
   range with "# frame N" markers, so the boot call path can be read end-to-end. */
extern uint32_t psxport_calltrace_on;
void psxport_calltrace_init(void);
void psxport_calltrace(uint32_t pc, uint32_t instr, const uint32_t* gpr);
void psxport_calltrace_frame(int frame);

/* CDC command/IRQ logging to stderr (PSXPORT_CDC_LOG=1). */
extern int psxport_cdc_log;

/* GTE transform tap: when psxport_gte_capture != 0, GTE_WriteCR forwards
   control-register writes 0..7 (rotation matrix CR0-4, translation CR5-7) to
   psxport_on_gte_cr. This is the per-object/camera transform used to project
   geometry — the basis for object-state capture and the reprojecting renderer. */
extern int psxport_gte_capture;
void psxport_on_gte_cr(unsigned which, uint32_t value);

/* GTE opcode histogram (RE aid): when psxport_gte_capture != 0, GTE_Instruction
   counts executed ops by (instr & 0x3F). Indexed 0..63. */
extern unsigned psxport_gte_op[64];

/* RTP vertex tap: when psxport_rtp_capture != 0, RTPS/RTPT report each
   projected vertex (input local + output screen) to psxport_on_rtp_vertex. */
extern int psxport_rtp_capture;
void psxport_on_rtp_vertex(int32_t vx, int32_t vy, int32_t vz, int32_t sx, int32_t sy, uint32_t sf);

/* MVMVA tap: when psxport_mvmva_capture != 0, each MVMVA (GTE matrix*vector+
   vector) reports the input local vertex (v-selected) and the view-space result
   MAC1/2/3. Tomba 2 transforms terrain with MVMVA then perspective-divides on
   the CPU, so reprojecting these covers the non-RTPS geometry. */
extern int psxport_mvmva_capture;
void psxport_on_mvmva(int32_t vx, int32_t vy, int32_t vz, int32_t mac1, int32_t mac2, int32_t mac3);

/* GPU polygon tap: when psxport_gpu_capture != 0, each GP0 polygon command
   (cc 0x20-0x3F) is reported with its raw command buffer (cb) and the active
   drawing offset, before rasterization. cb encodes verts/uv/color/clut/texpage
   per the GP0 polygon format. */
extern int psxport_gpu_capture;
void psxport_on_gpu_poly(uint32_t cc, const uint32_t* cb, int32_t off_x, int32_t off_y);

/* GP1(0x05) display-flip tap (renderer frame boundary). Always forwarded when a
   flip consumer is registered. */
void psxport_on_gpu_flip(uint32_t value);

/* Last emulated PC seen by the hook layer (watchdog diagnostics). */
extern uint32_t psxport_last_pc;

/* PC sampling profiler (RE aid): when psxport_prof != 0, every dispatched
   instruction PC is counted into a histogram (one entry per distinct PC).
   Use to find where the CPU actually spends time during a load/dwell so a fix
   targets the real hotspot (BIOS event path vs. game decode loop) instead of a
   guess. Requires hooks to be installed (the per-instruction dispatch gate). */
extern int psxport_prof;
void psxport_prof_reset(void);
void psxport_prof_report(int top);

/* Generic disc primitive: read `count` 2048-byte Mode2/Form1 user-data sectors
   from filesystem LBA `lba` into `dst` at host speed. Implemented in the imported
   cdc.c (needs the CDIF). Returns count on success, -1 on failure. The psxport
   runtime builds its native loads/HLE BIOS on top of this. */
int psxport_cd_read_sectors(int32_t lba, int count, uint8_t* dst);

/* Generic disc primitive: 1 if the drive is actively seeking/reading/playing,
   0 if idle (paused/standby/stopped). Lets the runtime distinguish a real load
   from an idle "Loading..." spin-dwell. From cdc.c. */
int psxport_cd_drive_busy(void);

/* Generic disc primitive: 1 if CD-XA ADPCM streaming (STRSND) is enabled in the
   current Setmode. Distinguishes a silent streamed clip (logo still, STRSND off)
   from a real FMV with audio (STRSND on). From cdc.c. */
int psxport_cd_strsnd_on(void);

/* Generic disc primitive: set the CD-ROM interrupt-enable mask (the PSX CD
   1F801802.idx1 register the BIOS CdInit sets). A pure-HLE BIOS that skips the
   ROM CdInit must call this (v=0x1F) so the CDC asserts IRQ_CD on command/data
   completion — otherwise the game's CD Sync waits on an interrupt that never
   fires. From cdc.c. */
void psxport_cd_set_irq_enable(int v);

/* BIOS-call tracer (RE aid): when psxport_bios_log != 0, every PSX BIOS call
   vector hit (A0/B0/C0 at phys 0x000000A0/B0/C0) logs its function number
   (in $t1) and a0..a3. Consecutive identical calls are coalesced so polling
   loops (TestEvent) don't flood. Used to find the loader's real entry points
   for native HLE. Stamped with psxport_frame. */
extern int psxport_bios_log;

/* Write-watchpoint (RE aid): when psxport_watch_addr != PSXPORT_WATCH_OFF, every
   CPU store whose target word (masked to 2MB) equals it reports the writing PC,
   value and width to psxport_on_write. Finds the code that owns a variable — e.g.
   the scene orchestrator that writes the scene-phase word. psxport_last_pc is the
   store instruction's PC (on_pc runs per-instruction whenever hooks exist). */
#define PSXPORT_WATCH_OFF 0xFFFFFFFFu
extern uint32_t psxport_watch_addr; /* physical byte addr & 0x1FFFFC, or OFF */
extern const uint8_t* psxport_ram_ptr; /* main RAM, for watchpoint backtraces */
void psxport_backtrace(void);           /* heuristic MIPS stack trace to stderr */
void psxport_on_write(uint32_t addr, uint32_t val, uint32_t pc, int width);

/* Host frame counter (set by the frontend; stamps debug logs). */
extern unsigned psxport_frame;

/* Live GPR file (32 GPRs + LO/HI), from the imported cpu.c. */
uint32_t* psxport_cpu_gpr(void);

/* Generic COP0 register access (cpu.c): read/write the CP0 register file by
   index (CP0REG_SR=12, CAUSE=13, EPC=14, TAR=6). A write recomputes the cached
   interrupt-pending state. Used by the runtime's native BIOS exception handler
   (runtime/hle_irq.cpp) to read CAUSE/EPC and pop SR (RFE) on return. */
uint32_t psxport_cpu_cop0(int reg);
uint8_t psxport_scratch8(uint32_t off);          /* read CPU scratchpad byte */
void psxport_scratch_poke8(uint32_t off, uint8_t v); /* poke CPU scratchpad byte */
void psxport_cpu_set_cop0(int reg, uint32_t v);

/* Generic IRQ-controller access (irq.c): I_STAT (status), I_MASK, and an
   ack-bits operation (Status &= ~bits; recompute CPU IRQ line). Pending IRQs =
   status & mask; bit 0=VBLANK,1=GPU,2=CD,3=DMA,4..6=TIMER0..2,7=SIO. */
uint16_t psxport_irq_status(void);
uint16_t psxport_irq_mask(void);
void psxport_irq_ack(uint16_t bits);

/* Generic CPU primitive (sibling to psxport_cpu_gpr): set the program counter,
   clearing branch-delay state, so execution resumes at `pc`. From cpu.c. The
   frontend uses gpr+set_pc to redirect the CPU; any boot/BIOS policy lives in
   the psxport runtime, not here. */
void psxport_cpu_set_pc(uint32_t pc);

/* Generic emulator primitive: advance the core by exactly one PSX video field
   (the body of libretro's retro_run, minus the frontend loop). Emits the frame
   via the registered video/audio callbacks. The psxport runtime calls this from
   its own play loop so it owns frame pacing (and can later insert interpolated
   in-between frames for wide60), rather than delegating the loop to retro_run.
   From libretro.c. */
void psxport_emulate_frame(void);

/* Diagnostics: registers + heuristic MIPS stack trace (scans the emulated
   stack for plausible return addresses: RAM text address preceded by a
   jal/jalr). ram = 2MB main RAM. */
void psxport_dump_cpu_state(const uint8_t* ram);

#ifdef __cplusplus
} /* extern "C" */

/* C++ registration side (frontend / per-game modules). expected_instr = 0
   skips the signature check (use only for non-overlay addresses). */
void psxport_add_hook(uint32_t pc, uint32_t expected_instr, psxport_hook_fn fn);
void psxport_clear_hooks();

/* Register a consumer for GTE control-register writes (0..7, plus 24/25/26 =
   OFX/OFY/H). Setting a non-null fn enables capture; null disables. */
typedef void (*psxport_gte_cr_fn)(unsigned which, uint32_t value);
void psxport_set_gte_cr_hook(psxport_gte_cr_fn fn);

/* Register a consumer for projected vertices (RTPS/RTPT). For each projected
   vertex: the input local vertex (vx,vy,vz, s16) and the game's output screen
   coords (sx,sy, s16), plus the shift flag sf (0 or 12). The transform in
   effect is whatever the CR hook last reported. Used to build/verify the
   reprojecting renderer. Setting a non-null fn enables capture. */
typedef void (*psxport_rtp_fn)(int32_t vx, int32_t vy, int32_t vz, int32_t sx, int32_t sy, uint32_t sf);
void psxport_set_rtp_hook(psxport_rtp_fn fn);

/* Register a consumer for MVMVA results (input local vertex + view-space MAC). */
typedef void (*psxport_mvmva_fn)(int32_t vx, int32_t vy, int32_t vz, int32_t mac1, int32_t mac2, int32_t mac3);
void psxport_set_mvmva_hook(psxport_mvmva_fn fn);

/* Register a consumer for GP0 polygon commands. Setting a non-null fn enables
   capture. cb points to a 16-word command buffer (valid only during the call). */
typedef void (*psxport_gpu_poly_fn)(uint32_t cc, const uint32_t* cb, int32_t off_x, int32_t off_y);
void psxport_set_gpu_poly_hook(psxport_gpu_poly_fn fn);

/* Register a consumer for GP1(0x05) display-area writes (the display flip /
   frame boundary). value = the raw GP1 parameter (DisplayFB X/Y start). */
typedef void (*psxport_gpu_flip_fn)(uint32_t value);
void psxport_set_gpu_flip_hook(psxport_gpu_flip_fn fn);
#endif

#endif
