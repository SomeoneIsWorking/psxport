// The A0/B0/C0 BIOS calls the titles reach, and EvMdINTR event delivery.
#include "bios_interrupt.h"
#include "bios_libc_string.h"
#include "bios_load_exec.h"
#include "core.h"
#include "execution_control.h"
#include "game.h"
#include "memcard.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <lucent/log.h>

namespace {

constexpr uint32_t EV_ID_BASE = 0xF1000000u;

// The guest terminates its own lines; the log is one line per call.
void logGuestLine(char *text) {
  const size_t n = std::strlen(text);
  if (n && text[n - 1] == '\n') {
    text[n - 1] = 0;
  }
  lucent::debug("guest", "{}", text);
}

} // namespace

int Hle::eventIndex(uint32_t id) const {
  uint32_t idx = id - EV_ID_BASE;
  return (idx < kEventSlots && ev[idx].open) ? (int)idx : -1;
}

void Hle::unDeliverEvent(uint32_t evClass, uint32_t spec) {
  for (int i = 0; i < kEventSlots; i++) {
    if (ev[i].open && ev[i].ev_class == evClass && (ev[i].spec & spec) && ev[i].mode == EV_MD_NOINTR) {
      ev[i].fired = 0;
    }
  }
}

void Hle::deliverEvent(uint32_t evClass, uint32_t spec) {
  Core *c = &game->core;
  for (int i = 0; i < kEventSlots; i++) {
    if (!(ev[i].open && ev[i].enabled && ev[i].ev_class == evClass && (ev[i].spec & spec))) {
      continue;
    }
    ev[i].fired = 1;
    if (ev[i].mode != EV_MD_INTR || !ev[i].func) {
      continue;
    }
    if (ev_depth) {
      lucent::warn("ev",
                   "nested delivery of class=0x{:08X} spec=0x{:08X} dropped (handler 0x{:08X}): the BIOS "
                   "runs these with interrupts masked, so re-entry is not faithful",
                   evClass,
                   spec,
                   ev[i].func);
      continue;
    }
    ev_depth++;
    const auto result = runAsInterrupt(*c, ev[i].func);
    ev_depth--;
    if (!psx::cpu::completeOrPropagate(*c, result)) {
      return;
    }
  }
}

// C0 keeps a callable `jr ra` at its base; B0[0x5B] publishes the native control area.
enum { HLE_C0TABLE = 0x8000F800u };
void Hle::workAreaInit() {
  Core *c = &game->core;
  if (work_ok) {
    return;
  }
  work_ok = 1;
  c->mem_w32(kB0Table + 0x16Cu, kWorkBase);
  c->mem_w32(HLE_C0TABLE + 0, 0x03E00008u); // jr $ra
  c->mem_w32(HLE_C0TABLE + 4, 0);           // nop
  c->mem_w32(HLE_C0TABLE + 0x06u * sizeof(uint32_t), 0x00000C80u);
}

// 80-byte DCBs, +0x00 name, +0x34 firstfile, +0x38 nextfile; entry slots stay 0 because the HLE services them.
enum { HLE_DCB_TABLE = 0x8000F900u, HLE_DCB_NAMES = 0x8000FA40u, HLE_DCB_NAME_STRIDE = 16u };

uint32_t Hle::deviceFind(const char *name) const {
  Core *c = &game->core;
  for (int i = 0; i < dcb_n; i++) {
    const uint32_t dcb = HLE_DCB_TABLE + (uint32_t)i * DCB_STRIDE;
    const uint32_t np = c->mem_r32(dcb + DCB_OFF_NAME);
    if (!np) {
      continue;
    }
    char got[HLE_DCB_NAME_STRIDE];
    c->readCString(np, got, sizeof got);
    if (std::strcmp(got, name) == 0) {
      return dcb;
    }
  }
  return 0;
}

void Hle::deviceAdd(const char *name) {
  Core *c = &game->core;
  if (deviceFind(name)) {
    return; // idempotent: one DCB per device name
  }
  if (dcb_n >= (int)DCB_MAX) {
    lucent::warn("hle",
                 "device table full ({} entries) — '{}' NOT installed. Guest code resolving "
                 "that prefix will find no device and its open/firstfile will fail.",
                 (int)DCB_MAX,
                 name);
    return;
  }
  const int slot = dcb_n;
  const uint32_t dcb = HLE_DCB_TABLE + (uint32_t)slot * DCB_STRIDE;
  const uint32_t np = HLE_DCB_NAMES + (uint32_t)slot * HLE_DCB_NAME_STRIDE;
  for (uint32_t i = 0; i < DCB_STRIDE; i++) {
    c->mem_w8(dcb + i, 0);
  }
  uint32_t k = 0;
  for (; name[k] && k + 1 < HLE_DCB_NAME_STRIDE; k++) {
    c->mem_w8(np + k, (uint8_t)name[k]);
  }
  c->mem_w8(np + k, 0);
  c->mem_w32(dcb + DCB_OFF_NAME, np);
  dcb_n = slot + 1;
  // Base and length are published together; a stale length sends the guest walk past the table.
  c->mem_w32(KERNEL_DCB_ADDR, HLE_DCB_TABLE);
  c->mem_w32(KERNEL_DCB_SIZE, (uint32_t)dcb_n * DCB_STRIDE);
  lucent::info("hle",
               "BIOS device '{}' installed: DCB 0x{:08X} (table 0x{:08X}, {} device(s), "
               "{} bytes at kernel 0x150/0x154)",
               name,
               dcb,
               (uint32_t)HLE_DCB_TABLE,
               dcb_n,
               (uint32_t)dcb_n * DCB_STRIDE);
}

void Hle::deviceUnhook(uint32_t dcb) {
  if (!dcb) {
    return;
  }
  Core *c = &game->core;
  c->mem_w32(dcb + DCB_OFF_FIRSTFILE, 0);
  c->mem_w32(dcb + DCB_OFF_NEXTFILE, 0);
}

namespace {

constexpr uint32_t kBiosThreadHandle = 0xFF000000u;

} // namespace

bool Hle::dispatchBios(char table, uint32_t fn) {
  Core *c = &game->core;
  uint32_t a0 = c->r[psx::cpu::kA0], a1 = c->r[psx::cpu::kA1], a2 = c->r[psx::cpu::kA2];
  // The A0/B0/C0 stubs tail-jump, so ra is the library call site.
  lucent::debug("bios",
                "{}0:0x{:02X}(0x{:08X}, 0x{:08X}, 0x{:08X}, 0x{:08X}) from 0x{:08X}",
                table,
                fn,
                a0,
                a1,
                a2,
                c->r[psx::cpu::kA3],
                c->r[psx::cpu::kRa]);
  if (table == 'A') {
    if (psx::hle::dispatchLibcString(c, fn)) {
      return true;
    }
    switch (fn) {
    // Sony libc rand/srand: 32-bit LCG, upper 15 bits.
    case 0x2F:
      rand_state = rand_state * 0x41C64E6Du + 0x3039u;
      c->r[psx::cpu::kV0] = (rand_state >> 16) & 0x7FFFu;
      return true;
    case 0x30:
      rand_state = a0;
      c->r[psx::cpu::kV0] = 0;
      return true;
    case 0x33:
      c->r[psx::cpu::kV0] = heapAlloc(a0);
      return true; // malloc
    case 0x34:
      heapFree(a0);
      c->r[psx::cpu::kV0] = 0;
      return true; // free
    case 0x37: {
      uint32_t n = a0 * a1, p = heapAlloc(n); // calloc
      if (p) {
        for (uint32_t i = 0; i < n; i++) {
          c->mem_w8(p + i, 0);
        }
      }
      c->r[psx::cpu::kV0] = p;
      return true;
    }
    case 0x38: {
      uint32_t old = a0, ns = a1; // realloc
      if (!old) {
        c->r[psx::cpu::kV0] = heapAlloc(ns);
        return true;
      }
      if (!ns) {
        heapFree(old);
        c->r[psx::cpu::kV0] = 0;
        return true;
      }
      uint32_t np = heapAlloc(ns), os = heapBlockSize(old);
      if (np) {
        psx::hle::guestCopy(*c, np, old, os < ns ? os : ns, psx::hle::CopyOrder::Forward);
        heapFree(old);
      }
      c->r[psx::cpu::kV0] = np;
      return true;
    }
    // Logged every boot: a wrong guest size only shows when an allocation near it fails.
    case 0x39:
      lucent::info("hle", "InitHeap(base=0x{:08X}, size=0x{:X} = {} bytes) from guest a0/a1", a0, a1, a1);
      heapInit({a0, a1});
      c->r[psx::cpu::kV0] = 0;
      return true; // InitHeap
    case 0x13: // setjmp(buf)
      psx::hle::saveJmpBuf(c, a0);
      c->r[psx::cpu::kV0] = 0;
      return true;
    // No guest unwind exists; a normal return would resume the wrong continuation.
    case 0x14: // longjmp(buf, val)
      lucent::error("hle",
                    "A0:0x14 longjmp(buf=0x{:08X}, val={}) — psxport has no guest unwind "
                    "mechanism. Faking a return here would resume the wrong continuation with "
                    "the failed operation's state live. Refusing.",
                    a0,
                    a1);
      abort();
    // printf: the guest's own diagnostics, on the `guest` channel.
    case 0x3F: {
      char out[1024];
      unsigned o = 0;
      uint32_t fmt = a0, argi = 0;
      const uint32_t argreg[3] = {a1, a2, c->r[psx::cpu::kA3]};
      // o32 varargs past a3 sit at sp+16.
      auto nextarg = [&]() -> uint32_t {
        if (argi < 3) {
          return argreg[argi++];
        }
        return c->mem_r32(c->r[psx::cpu::kSp] + 16u + 4u * (argi++ - 3));
      };
      for (uint32_t i = 0; o + 1 < sizeof out; i++) {
        const char ch = (char)c->mem_r8(fmt + i);
        if (!ch) {
          break;
        }
        if (ch != '%') {
          out[o++] = ch;
          continue;
        }
        // The host printf applies the guest's own conversion spec.
        char spec[32];
        unsigned sn = 0;
        spec[sn++] = '%';
        char conv = 0;
        for (uint32_t k = i + 1; sn + 1 < sizeof spec; k++) {
          const char cc = (char)c->mem_r8(fmt + k);
          if (!cc) {
            break;
          }
          spec[sn++] = cc;
          if (strchr("diouxXcspfgeEG%", cc)) {
            conv = cc;
            i = k;
            break;
          }
        }
        spec[sn] = 0;
        if (!conv) {
          break;
        }
        char tmp[256];
        if (conv == '%') {
          out[o++] = '%';
          continue;
        }
        if (conv == 's') {
          c->readCString(nextarg(), tmp, sizeof tmp);
          o += (unsigned)snprintf(out + o, sizeof out - o, "%s", tmp);
        } else if (conv == 'c') {
          o += (unsigned)snprintf(out + o, sizeof out - o, "%c", (char)nextarg());
        } else {
          snprintf(tmp, sizeof tmp, "%s", spec);
          o += (unsigned)snprintf(out + o, sizeof out - o, tmp, (int)nextarg());
        }
      }
      out[o < sizeof out ? o : sizeof out - 1] = 0;
      logGuestLine(out);
      c->r[psx::cpu::kV0] = 0;
      return true;
    }
    case 0x44: // FlushCache (no-op)
    case 0x49: // GPU_cw (GP0 word — harmless)
      c->r[psx::cpu::kV0] = 0;
      return true;
    // LoadAndExecute: on success the loaded entry is the continuation; the BIOS never returns.
    case 0x51:
      return psx::hle::dispatchLoadExec(*c, loadExecReader);
    case 0x70: // _bu_init no-op
    case 0x71: // _96_init no-op
    case 0x72: // _96_remove no-op
      c->r[psx::cpu::kV0] = 0;
      return true;
    default: {
      if (c->game->memcard.dispatchBiosA0(fn, c)) {
        return true;
      }
      return false;
    }
    }
  }
  if (table == 'B') {
    if (dispatchPadBios(fn)) {
      return true;
    }
    switch (fn) {
    case 0x07:
      deliverEvent(a0, a1);
      c->r[psx::cpu::kV0] = 0;
      return true; // DeliverEvent
    case 0x20:
      unDeliverEvent(a0, a1);
      c->r[psx::cpu::kV0] = 0;
      return true; // UnDeliverEvent
    case 0x08: {   // OpenEvent
      for (int i = 0; i < kEventSlots; i++) {
        if (!ev[i].open) {
          ev[i].open = 1;
          ev[i].enabled = 0;
          ev[i].fired = 0;
          ev[i].ev_class = a0;
          ev[i].spec = a1;
          ev[i].mode = a2;
          ev[i].func = c->r[psx::cpu::kA3];
          // The classes a frame driver must deliver, and the handle a spinning TestEvent names.
          lucent::debug("ev",
                        "OpenEvent class=0x{:08X} spec=0x{:08X} mode=0x{:08X} handler=0x{:08X} -> handle=0x{:08X}",
                        a0,
                        a1,
                        a2,
                        c->r[psx::cpu::kA3],
                        EV_ID_BASE + (uint32_t)i);
          c->r[psx::cpu::kV0] = EV_ID_BASE + (uint32_t)i;
          return true;
        }
      }
      c->r[psx::cpu::kV0] = 0xFFFFFFFFu;
      return true; // table full
    }
    case 0x09: {
      int i = eventIndex(a0); // CloseEvent
      if (i >= 0) {
        ev[i].open = 0;
        c->r[psx::cpu::kV0] = 1;
      } else {
        c->r[psx::cpu::kV0] = 0;
      }
      return true;
    }
    case 0x0A: {
      int i = eventIndex(a0); // WaitEvent (can't block)
      if (i >= 0) {
        ev[i].fired = 0;
        c->r[psx::cpu::kV0] = 1;
      } else {
        c->r[psx::cpu::kV0] = 0;
      }
      return true;
    }
    case 0x0B: {
      int i = eventIndex(a0); // TestEvent (read+clear)
      if (i >= 0 && ev[i].fired) {
        ev[i].fired = 0;
        c->r[psx::cpu::kV0] = 1;
      } else {
        c->r[psx::cpu::kV0] = 0;
      }
      return true;
    }
    case 0x0C: {
      int i = eventIndex(a0); // EnableEvent
      if (i >= 0) {
        ev[i].enabled = 1;
        c->r[psx::cpu::kV0] = 1;
      } else {
        c->r[psx::cpu::kV0] = 0;
      }
      return true;
    }
    case 0x0D: {
      int i = eventIndex(a0); // DisableEvent
      if (i >= 0) {
        ev[i].enabled = 0;
        c->r[psx::cpu::kV0] = 1;
      } else {
        c->r[psx::cpu::kV0] = 0;
      }
      return true;
    }
    case 0x3F: { // B(3Fh) puts
      char out[512];
      c->readCString(a0, out, sizeof out);
      logGuestLine(out);
      c->r[psx::cpu::kV0] = 0;
      return true;
    }
    case 0x15:
    case 0x16: // Legacy PAD_init2/PAD_dr remain outside this callback contract.
      c->r[psx::cpu::kV0] = 0;
      return true;
    case 0x17: // ReturnFromException — non-returning unwind to irqPoll's saved R3000 context
      psx::hle::returnFromException(custom_exit_active != 0);
    case 0x18: // ResetEntryInt — remove the custom exception-exit context
      exception_exit_buf = 0;
      c->r[psx::cpu::kV0] = 0;
      return true;
    case 0x19: // HookEntryInt — install the guest jmp_buf restored after the BIOS chain walk
      exception_exit_buf = a0;
      psx::hle::traceInterruptCustomExit(c, a0);
      c->r[psx::cpu::kV0] = 0;
      return true;
    // B0:0x35 FileWrite, console fds included, belongs to Memcard::dispatchBiosB0.
    // BIOS threads: no context switch; a title that runs guest threads binds its own.
    case 0x0E:
      c->r[psx::cpu::kV0] = kBiosThreadHandle;
      return true; // OpenThread
    case 0x0F:
      c->r[psx::cpu::kV0] = 1;
      return true; // CloseThread
    case 0x10:
      c->r[psx::cpu::kV0] = a0;
      return true; // ChangeThread
    case 0x4A:
      c->r[psx::cpu::kV0] = 0;
      return true; // stopCard / card no-op
    case 0x4B:
      c->r[psx::cpu::kV0] = 1;
      return true; // cardInfo -> present
    case 0x56:
      workAreaInit();
      c->r[psx::cpu::kV0] = HLE_C0TABLE;
      return true; // GetC0Table
    case 0x57:
      workAreaInit();
      c->r[psx::cpu::kV0] = kB0Table;
      return true; // GetB0Table
    case 0x5B:
      c->r[psx::cpu::kV0] = 0;
      return true; // ChangeClearPAD (no-op)
    default: {
      if (c->game->memcard.dispatchBiosB0(fn, c)) {
        return true;
      }
      return false;
    }
    }
  }
  if (table == 'C') {
    switch (fn) {
    // SysEnqIntRP / SysDeqIntRP return the element.
    case 0x02:
    case 0x03:
      if (lucent::channel_on("bios")) {
        const char *which = (fn == 0x02) ? "SysEnqIntRP" : "SysDeqIntRP";
        lucent::debug("bios",
                      "{} prio={} elem=0x{:08X} -> [+0]=0x{:08X} [+4]=0x{:08X} [+8]=0x{:08X} [+C]=0x{:08X}",
                      which,
                      a0,
                      a1,
                      c->mem_r32(a1),
                      c->mem_r32(a1 + 4),
                      c->mem_r32(a1 + 8),
                      c->mem_r32(a1 + 12));
      }
      if (fn == 0x02) {
        irqEnq(a0, a1);
      } else {
        irqDeq(a1);
      }
      c->r[psx::cpu::kV0] = a1;
      return true;
    case 0x00:
    case 0x01:
    case 0x07:
    case 0x08: // kernel-table
    case 0x0A:
    case 0x0C:
    case 0x12:
    case 0x1C: // installers + RCnt
      c->r[psx::cpu::kV0] = 0;
      return true;
    default:
      return false;
    }
  }
  return false;
}
