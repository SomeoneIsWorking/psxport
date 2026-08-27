// test_memcard_file_api.cpp — the BIOS memory-card FILE API is ASYNCHRONOUS, and its return value
// says so.
//
// WHAT IS BROKEN. `runtime/recomp/memcard.cpp` `file_read` / `file_write` (B0:0x34 / B0:0x35) do the
// host I/O synchronously and return the byte COUNT. Sony's stock libmcrd does not accept a byte count
// there: its READ and WRITE op state machines retry the BIOS call until it returns ZERO, and only
// then move to the state that waits for the card completion EVENT. Transcribed from Spyro's own
// executable (`SCUS_942.28`, generated/shard_2.c gen_func_800671F0 case 0x14, generated/shard_0.c
// gen_func_80066F34 case 0x14 — Ghidra rendering in the game repo's docs/issues/0051):
//
//     case 0x14:                                   // "start the transfer"
//       clear_card_events();                       // 0x8006815C: TestEvent x8, clears the flags
//       do { v0 = read(fd, buf, len); } while (v0 != 0);
//       state = 0x1E;                              // …which WAITS for the completion event
//
// So a non-zero return is "busy, ask again" and the loop is unbounded. Returning `len` wedges the
// guest inside its own vblank callback and the port stops presenting entirely — the user-reported
// "SELECT MEMORY CARD" softlock.
//
// AND THE ERROR PATH HAS THE SAME SHAPE. Because ANY non-zero return re-runs the loop, a transfer
// the backend cannot perform must NOT be reported through the return value: it is accepted (0) and
// completed with the card ERROR spec (0x8000) instead of the I/O-end spec (0x0004). The guest's very
// next state reads exactly that distinction — 0x8006841C sums the four SwCARD flags and 0x80068264
// maps them to a code, where the error code drives a bounded retry and then an access code the game
// reports to the player. A silently-dropped frame that still announces I/O-END is a save the game
// believes succeeded.
//
// WHAT IS *NOT* CLAIMED HERE: that the transfer completes asynchronously. This backend is a host
// file and finishes inside the call; the return value and the event are the guest-visible contract,
// and that contract is what these cases pin.
//
// HERMETIC: a Game is constructed in-process and the card is a temp file in the CWD. No disc, no
// GPU or window. Most events use EvMdNOINTR; the directory-completion regression installs one
// synthetic guest handler through the shipping override/dispatch path so it can prove the exact
// EvMdINTR callback Crash Bash waits on.
#include "../runtime/recomp/game.h"
#include "../runtime/recomp/memcard.h"
#include "../runtime/recomp/override_registry.h"
#include "testutil.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { R_V0 = 2, R_A0 = 4, R_A1 = 5, R_A2 = 6, R_A3 = 7 };

// libmcrd's two event classes and the four specs the guest opens on each (Spyro 0x80067EA0).
static constexpr uint32_t kSwCard = 0xF4000001u;
static constexpr uint32_t kHwCard = 0xF0000011u;
static constexpr uint32_t kSpecIoEnd = 0x0004u; // EvSpIOE   — the transfer finished
static constexpr uint32_t kSpecError = 0x8000u; // EvSpERROR — the transfer failed

static const char *kCardPath = "test_memcard_file_api.mcr";

// A guest RAM window well clear of anything the framework touches, used as the transfer buffer.
static constexpr uint32_t kBuf = 0x80100000u;
static constexpr uint32_t kDirectoryCallback = 0x80001000u;

static GameConfig g_cfg{};
static uint32_t g_directory_callbacks = 0;

static void directory_callback(Core *) {
  g_directory_callbacks++;
}

static Game *gam() {
  static Game *g = nullptr;
  if (g) {
    return g;
  }
  remove(kCardPath); // a fresh, unformatted image every run: init() formats it
  g = new Game();
  g_cfg.cardDefaultPath = kCardPath; // no env, no .env — the card path is stated here
  g->core.cfg = &g_cfg;
  // card_overrides_init, not memcard.init(): the card is a BIOS DEVICE as well as a backing file,
  // and publishing it in the kernel device table is part of bringing it up. Calling init() alone
  // tests a card the running game never has.
  card_overrides_init(g);
  return g;
}

// EVERY call below goes through `Hle::dispatchBios`, the SAME entry the guest's B0 stub reaches —
// never `card_hle_b0` directly. That is not a stylistic choice: dispatchBios has its own switch that
// claims some B0 numbers before the card module ever sees them, and calling the card module directly
// tests a function the running game does not reach. This test was written the wrong way round first
// and was GREEN against a build in which every card WRITE was silently discarded by an earlier
// `case 0x35` in that switch — the bug it exists to catch.
static uint32_t bios_b0(Game *g, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2) {
  Core *c = &g->core;
  c->r[R_A0] = a0;
  c->r[R_A1] = a1;
  c->r[R_A2] = a2;
  c->r[R_V0] = 0xDEADBEEFu; // so "the handler never wrote v0" cannot read as a plausible answer
  const bool handled = g->hle.dispatchBios('B', fn);
  return handled ? c->r[R_V0] : 0xDEADBEEFu;
}

// Open + enable one BIOS event through the ordinary BIOS entry points, exactly as the guest does.
// EvMdNOINTR (0x2000): the slot is MARKED on delivery and no handler runs, so a test needs no guest
// code. Returns the handle.
static uint32_t open_event(Game *g, uint32_t cls, uint32_t spec) {
  g->core.r[R_A3] = 0; // no handler: EvMdNOINTR marks the slot and runs nothing
  const uint32_t h = bios_b0(g, 0x08, cls, spec, 0x2000u);
  bios_b0(g, 0x0C, h, 0, 0); // EnableEvent — deliverEvent ignores a disabled slot
  return h;
}

// EvMdINTR (0x1000): delivery dispatches the registered guest handler immediately. The synthetic
// address is installed through the same override interception point used by a real port, so this
// exercises Hle::deliverEvent's shipping callback path without linking a title substrate.
static uint32_t open_callback_event(Game *g, uint32_t cls, uint32_t spec) {
  static bool installed = false;
  if (!installed) {
    overrides::install(kDirectoryCallback, "MemcardDirectoryCompletionTest", directory_callback, directory_callback);
    installed = true;
  }
  g->core.r[R_A3] = kDirectoryCallback;
  const uint32_t h = bios_b0(g, 0x08, cls, spec, 0x1000u);
  bios_b0(g, 0x0C, h, 0, 0);
  return h;
}

// TestEvent: 1 if the event has fired since it was last tested, and CLEARS it (BIOS semantics).
static bool test_event(Game *g, uint32_t handle) {
  return bios_b0(g, 0x0B, handle, 0, 0) != 0;
}

static uint32_t card_open_create(Game *g, const char *name, uint32_t blocks) {
  Core *c = &g->core;
  uint32_t va = kBuf + 0x800u; // scratch for the name string
  for (uint32_t i = 0;; i++) {
    c->mem_w8(va + i, (uint8_t)name[i]);
    if (!name[i]) {
      break;
    }
  }
  return bios_b0(g, 0x32, va, (blocks << 16) | 0x0200u, 0); // 0x0200 = create
}

static uint32_t card_lseek(Game *g, uint32_t fd, uint32_t off) {
  return bios_b0(g, 0x33, fd, off, 0); // whence 0 = SEEK_SET
}

static uint32_t card_write(Game *g, uint32_t fd, uint32_t buf, uint32_t len) {
  return bios_b0(g, 0x35, fd, buf, len);
}

static uint32_t card_read(Game *g, uint32_t fd, uint32_t buf, uint32_t len) {
  return bios_b0(g, 0x34, fd, buf, len);
}

// ---------------------------------------------------------------------------------------------
// THE HEADLINE CASE. libmcrd loops `while (v0 != 0)`, so anything but 0 is an infinite loop in the
// guest. Both directions, because both state machines carry the same loop.
static void test_transfer_start_returns_zero(void) {
  Game *g = gam();
  const uint32_t fd = card_open_create(g, "bu00:PSXPORT-TEST", 1);
  CHECK(fd != 0xFFFFFFFFu);

  for (uint32_t i = 0; i < 128; i++) {
    g->core.mem_w8(kBuf + i, (uint8_t)(i * 7 + 3));
  }
  CHECK_EQ(card_lseek(g, fd, 0), 0u);
  CHECK_EQ(card_write(g, fd, kBuf, 128), 0u); // NOT 128: the call only STARTS the transfer
  CHECK_EQ(card_lseek(g, fd, 0), 0u);
  CHECK_EQ(card_read(g, fd, kBuf + 0x400u, 128), 0u);
}

// …and 0 must not be reachable by doing nothing. The bytes have to be there.
static void test_transfer_actually_moves_the_bytes(void) {
  Game *g = gam();
  const uint32_t fd = card_open_create(g, "bu00:PSXPORT-TEST", 1);
  CHECK(fd != 0xFFFFFFFFu);
  for (uint32_t i = 0; i < 128; i++) {
    g->core.mem_w8(kBuf + i, (uint8_t)(i * 7 + 3));
  }
  card_lseek(g, fd, 0);
  card_write(g, fd, kBuf, 128);
  for (uint32_t i = 0; i < 128; i++) {
    g->core.mem_w8(kBuf + 0x400u + i, 0xEE); // poison the target
  }
  card_lseek(g, fd, 0);
  card_read(g, fd, kBuf + 0x400u, 128);

  int compared = 0, same = 0;
  for (uint32_t i = 0; i < 128; i++) {
    compared++;
    if (g->core.mem_r8(kBuf + 0x400u + i) == (uint8_t)(i * 7 + 3)) {
      same++;
    }
  }
  CHECK_EQ(compared, 128); // the denominator, so "0 mismatches" cannot mean "0 compared"
  CHECK_EQ(same, 128);
}

// The completion the guest's NEXT state waits on. Delivering the wrong spec here is not a cosmetic
// difference: 0x0004 means "done", 0x8000 means "failed, retry" — the guest branches on which.
static void test_success_delivers_io_end_and_not_error(void) {
  Game *g = gam();
  const uint32_t sw_io = open_event(g, kSwCard, kSpecIoEnd);
  const uint32_t sw_err = open_event(g, kSwCard, kSpecError);
  const uint32_t hw_io = open_event(g, kHwCard, kSpecIoEnd);
  const uint32_t fd = card_open_create(g, "bu00:PSXPORT-TEST", 1);
  CHECK(fd != 0xFFFFFFFFu);

  test_event(g, sw_io);
  test_event(g, sw_err);
  test_event(g, hw_io); // clear anything pending
  card_lseek(g, fd, 0);
  card_read(g, fd, kBuf, 128);
  CHECK(test_event(g, sw_io));   // SwCARD EvSpIOE fired
  CHECK(test_event(g, hw_io));   // HwCARD completion fired
  CHECK(!test_event(g, sw_err)); // and the ERROR spec did NOT
}

// THE FAILURE THE GUEST MUST BE ABLE TO SEE. A transfer the backend cannot perform still has to
// terminate the guest's retry loop (return 0) and then report the failure the only way the guest
// reads it — the ERROR spec instead of I/O-end. Induced by seeking past the end of the card, so
// every frame index the transfer computes is out of range; today those frames are silently skipped
// and the call announces I/O-END anyway, which is a save the game believes succeeded.
static void test_unperformable_transfer_fails_visibly(void) {
  Game *g = gam();
  const uint32_t sw_io = open_event(g, kSwCard, kSpecIoEnd);
  const uint32_t sw_err = open_event(g, kSwCard, kSpecError);
  const uint32_t fd = card_open_create(g, "bu00:PSXPORT-TEST", 1);
  CHECK(fd != 0xFFFFFFFFu);

  test_event(g, sw_io);
  test_event(g, sw_err);
  card_lseek(g, fd, 0x00100000u);            // 1 MB into a 128 KB card: no such frame exists
  CHECK_EQ(card_read(g, fd, kBuf, 128), 0u); // still ACCEPTED — a non-zero return spins forever
  CHECK(test_event(g, sw_err));              // …and reported as a card ERROR
  CHECK(!test_event(g, sw_io));              // never as a completed I/O
}

// The boundary that must NOT move: a bad file descriptor is a BIOS ARGUMENT error, synchronous and
// -1, and it announces nothing. libmcrd never reaches read() with one (it aborts the op when open
// fails), so this is the case where the async contract does not apply.
static void test_invalid_fd_is_a_synchronous_error(void) {
  Game *g = gam();
  const uint32_t sw_io = open_event(g, kSwCard, kSpecIoEnd);
  const uint32_t sw_err = open_event(g, kSwCard, kSpecError);
  test_event(g, sw_io);
  test_event(g, sw_err);

  CHECK_EQ(card_read(g, 99, kBuf, 128), 0xFFFFFFFFu);
  CHECK_EQ(card_write(g, 99, kBuf, 128), 0xFFFFFFFFu);
  CHECK(!test_event(g, sw_io));
  CHECK(!test_event(g, sw_err));
}

// fd 1 and 2 ARE the console devices and keep the ordinary synchronous write semantics — the
// byte count, and no card event. This is the half of B0:0x35 that dispatchBios is right to own, and
// it is here so that "delegate card fds to the card" cannot be implemented by deleting the console
// path.
static void test_console_fds_keep_the_byte_count(void) {
  Game *g = gam();
  const uint32_t sw_io = open_event(g, kSwCard, kSpecIoEnd);
  test_event(g, sw_io);
  for (uint32_t i = 0; i < 4; i++) {
    g->core.mem_w8(kBuf + i, (uint8_t)"ok\n"[i]);
  }
  CHECK_EQ(card_write(g, 2, kBuf, 3), 3u); // stderr
  CHECK(!test_event(g, sw_io));            // a console write is not a card operation
}

// ---------------------------------------------------------------------------------------------
// THE DEVICE TABLE. Guest code resolves a path prefix by walking the kernel device array itself
// (kernel 0x150 = address, 0x154 = size in bytes) rather than through a BIOS vector. Nothing
// published those words, so the walk read a garbage base and a 1 GB length and dereferenced the
// first non-zero word it found as a `char*` — the memory model refused the read and the port
// aborted. That is the whole of the "saving crashes the game" report.
static void test_device_table_is_published(void) {
  Game *g = gam();
  Core *c = &g->core;
  const uint32_t base = c->mem_r32(Hle::KERNEL_DCB_ADDR);
  const uint32_t size = c->mem_r32(Hle::KERNEL_DCB_SIZE);
  CHECK(base != 0);                               // a base of 0 is what walked from address 0
  CHECK_EQ(size, (uint32_t)Hle::DCB_STRIDE);      // exactly one device, exactly one stride
  CHECK_EQ(size % (uint32_t)Hle::DCB_STRIDE, 0u); // the walk steps by DCB_STRIDE and compares <

  // …and the entry it points at really is the card, read the way the guest reads it.
  const uint32_t np = c->mem_r32(base + Hle::DCB_OFF_NAME);
  CHECK(np != 0);
  CHECK_EQ((char)c->mem_r8(np + 0), 'b');
  CHECK_EQ((char)c->mem_r8(np + 1), 'u');
  CHECK_EQ((char)c->mem_r8(np + 2), '\0');

  // The lookup must be able to say NO. A finder that matches everything would "find" the card for
  // any prefix and route a cdrom path into the card file.
  CHECK_EQ(g->hle.deviceFind("bu"), base);
  CHECK_EQ(g->hle.deviceFind("cdrom"), 0u);
  CHECK_EQ(g->hle.deviceFind("b"), 0u); // prefix, not a match
  CHECK_EQ(g->hle.deviceFind("bus"), 0u);
}

// firstfile on a card with nothing on it. Runs FIRST, before any test creates a file, so the "0"
// below is a measured empty directory rather than an unarmed scan.
static void test_firstfile_on_an_empty_card_finds_nothing(void) {
  Game *g = gam();
  const uint32_t hw_io = open_callback_event(g, kHwCard, kSpecIoEnd);
  CHECK(hw_io != 0xFFFFFFFFu);
  uint32_t va = kBuf + 0xA00u;
  for (uint32_t i = 0; "bu00:*"[i]; i++) {
    g->core.mem_w8(va + i, (uint8_t)"bu00:*"[i]);
  }
  g->core.mem_w8(va + 6, 0);
  g_directory_callbacks = 0;
  CHECK_EQ(bios_b0(g, 0x42, va, kBuf + 0xB00u, 0), 0u);
  CHECK_EQ(g_directory_callbacks, 1u); // completed empty scan, exactly once

  g_directory_callbacks = 0;
  CHECK_EQ(bios_b0(g, 0x43, kBuf + 0xB00u, 0, 0), 0u); // nextfile agrees
  CHECK_EQ(g_directory_callbacks, 1u);                 // completed end-of-scan, exactly once
}

// …and the same call on a card that HAS files returns them, in the DIRENTRY layout the guest reads
// (name at +0). Without this, the empty-card case above proves nothing: a firstfile hard-wired to 0
// passes it. 0x42 is firstfile and 0x43 is nextfile — they were wired the other way round, so
// firstfile was not handled at all and the BIOS-miss path left the guest a stale $v0.
static void test_firstfile_enumerates_the_directory(void) {
  Game *g = gam();
  CHECK(card_open_create(g, "bu00:PSXPORT-ONE", 1) != 0xFFFFFFFFu);
  CHECK(card_open_create(g, "bu00:PSXPORT-TWO", 1) != 0xFFFFFFFFu);

  const uint32_t path = kBuf + 0xA00u, dirent = kBuf + 0xB00u;
  for (uint32_t i = 0; "bu00:PSXPORT-*"[i]; i++) {
    g->core.mem_w8(path + i, (uint8_t)"bu00:PSXPORT-*"[i]);
  }
  g->core.mem_w8(path + 14, 0);

  char seen[4][24] = {{0}};
  int n = 0;
  uint32_t v0 = bios_b0(g, 0x42, path, dirent, 0);
  while (v0 && n < 4) {
    CHECK_EQ(v0, dirent); // firstfile/nextfile return the DIRENTRY
    for (uint32_t i = 0; i < 20; i++) {
      seen[n][i] = (char)g->core.mem_r8(dirent + i);
    }
    n++;
    v0 = bios_b0(g, 0x43, dirent, 0, 0);
  }
  CHECK_EQ(n, 2); // both files, and the walk terminated
  const bool one = !strcmp(seen[0], "PSXPORT-ONE") || !strcmp(seen[1], "PSXPORT-ONE");
  const bool two = !strcmp(seen[0], "PSXPORT-TWO") || !strcmp(seen[1], "PSXPORT-TWO");
  CHECK(one);
  CHECK(two);
  // The size field the browser reads back, from the directory entry rather than from thin air.
  CHECK(g->core.mem_r32(dirent + Memcard::kDirEntSize) != 0u);

  // A pattern that matches nothing must come back empty on the SAME populated card — otherwise
  // "enumerates correctly" is indistinguishable from "returns everything".
  for (uint32_t i = 0; "bu00:NOSUCH-*"[i]; i++) {
    g->core.mem_w8(path + i, (uint8_t)"bu00:NOSUCH-*"[i]);
  }
  g->core.mem_w8(path + 13, 0);
  CHECK_EQ(bios_b0(g, 0x42, path, dirent, 0), 0u);
}

// Guest code installs a restore-trampoline in the DCB's firstfile slot before calling B0:0x42 and
// expects the BIOS to run it. This port services B0:0x42 itself, so the slot has to come back
// unhooked — otherwise it keeps a guest address the HLE will never dispatch, and the next call
// saves THAT aside as the "original".
static void test_firstfile_restores_the_patched_dcb_slot(void) {
  Game *g = gam();
  Core *c = &g->core;
  const uint32_t dcb = g->hle.deviceFind("bu");
  CHECK(dcb != 0);
  c->mem_w32(dcb + Hle::DCB_OFF_FIRSTFILE, 0x80080ADCu);           // what FUN_80080940 writes
  CHECK_EQ(c->mem_r32(dcb + Hle::DCB_OFF_FIRSTFILE), 0x80080ADCu); // the poke landed

  const uint32_t path = kBuf + 0xA00u;
  for (uint32_t i = 0; "bu00:*"[i]; i++) {
    c->mem_w8(path + i, (uint8_t)"bu00:*"[i]);
  }
  c->mem_w8(path + 6, 0);
  bios_b0(g, 0x42, path, kBuf + 0xB00u, 0);
  CHECK_EQ(c->mem_r32(dcb + Hle::DCB_OFF_FIRSTFILE), 0u);
}

int main(void) {
  RUN(device_table_is_published);
  RUN(firstfile_on_an_empty_card_finds_nothing);
  RUN(firstfile_enumerates_the_directory);
  RUN(firstfile_restores_the_patched_dcb_slot);
  RUN(transfer_start_returns_zero);
  RUN(console_fds_keep_the_byte_count);
  RUN(transfer_actually_moves_the_bytes);
  RUN(success_delivers_io_end_and_not_error);
  RUN(unperformable_transfer_fails_visibly);
  RUN(invalid_fd_is_a_synchronous_error);
  return pt_summary();
}
