// render_debug_probes.cpp — diagnostic-only render-walk probes (no draw, no engine-owned behavior). Split
// out of engine_submit.cpp (2026-07 restructure) per the game-folder audit: the geometry-SUBMIT file
// should hold only the GT3/GT4/gt4_bp submitters it is named for, not counters/decoders used solely for
// PSXPORT_DEBUG channels.
#include "core.h"
#include "cfg.h"
#include <stdio.h>

// ProjVtx + EObjXform + eproj_* (world-coord projection) live in engine/engine_project.*.
#include "engine_project.h"

void rec_super_call(Core*, uint32_t);   // interpret the original PSX body (A/B oracle / super-call)
int gpu_frame_no(Core*);

// DIAGNOSTIC (later-234 ground blocker): decode the GROUND scene table 0x800F2418 through the EXACT same
// record layout the native GT3/GT4 submitters use, and log the first few entries' decoded model verts +
// their eproj projection — WITHOUT drawing (the PSX pass still draws the ground, so it stays visible). This
// isolates whether the record striding/offsets or the camera compose is the cause of the vanish. Gated by
// `debug groundprobe`; runs once.
void ov_ground_probe(Core* c) {
  if (!cfg_dbg("groundprobe")) return;
  static int done = 0; if (done >= 3) return;
  uint32_t es = 0x800F2418u;
  uint8_t count = c->mem_r8(es + 6);
  if (count == 0) return;   // table not populated this frame
  done++;
  uint32_t base = c->mem_r32(es + 0xC);
  EObjXform w; eproj_compose_camera(c, &w); eproj_set_active(&w);
  fprintf(stderr, "[groundprobe] es=%08x count=%u base=%08x T=(%.0f,%.0f,%.0f) H=%.0f R0=(%.3f,%.3f,%.3f) R2=(%.3f,%.3f,%.3f)\n",
    es, count, base, (double)w.T[0],(double)w.T[1],(double)w.T[2],(double)w.H,
    (double)w.R[0][0]/4096,(double)w.R[0][1]/4096,(double)w.R[0][2]/4096,
    (double)w.R[2][0]/4096,(double)w.R[2][1]/4096,(double)w.R[2][2]/4096);
  fprintf(stderr, "[groundprobe] OFX=%.1f OFY=%.1f\n", (double)w.ofx, (double)w.ofy);
  int logged = 0;
  uint32_t p = es + 0x10, end = es + 0x10 + (uint32_t)count * 2;
  for (; p < end && logged < 6; p += 2) {
    uint32_t idx = c->mem_r16(p);
    uint32_t cmd = base + idx * 4;
    uint32_t s0  = c->mem_r32(cmd);
    uint32_t gt3 = s0 & 0xFF, gt4 = (s0 >> 16) & 0xFF;
    fprintf(stderr, "[groundprobe] entry[%u] idx=%u cmd=%08x s0=%08x gt3=%u gt4=%u\n",
            (p - es - 0x10)/2, idx, cmd, s0, gt3, gt4);
    // first GT3 record (stride 36) — same offsets as submit_poly_gt3_native
    if (gt3) {
      uint32_t rec = cmd + 4;
      uint32_t vz01 = c->mem_r32(rec + 20);
      uint32_t xy0 = c->mem_r32(rec + 16), xy1 = c->mem_r32(rec + 24), xy2 = c->mem_r32(rec + 28);
      ProjVtx pv0, pv1, pv2;
      eproj_vertex_active((int16_t)xy0, (int16_t)(xy0>>16), (int16_t)vz01, &pv0);
      eproj_vertex_active((int16_t)xy1, (int16_t)(xy1>>16), (int16_t)(vz01>>16), &pv1);
      eproj_vertex_active((int16_t)xy2, (int16_t)(xy2>>16), (int16_t)c->mem_r32(rec+32), &pv2);
      fprintf(stderr, "   gt3 m0=(%d,%d,%d) -> px=%.1f py=%.1f pz=%.1f | m1=(%d,%d,%d)->(%.0f,%.0f) m2=(%d,%d,%d)->(%.0f,%.0f)\n",
        (int16_t)xy0,(int16_t)(xy0>>16),(int16_t)vz01, (double)pv0.px,(double)pv0.py,(double)pv0.pz,
        (int16_t)xy1,(int16_t)(xy1>>16),(int16_t)(vz01>>16), (double)pv1.px,(double)pv1.py,
        (int16_t)xy2,(int16_t)(xy2>>16),(int16_t)c->mem_r32(rec+32), (double)pv2.px,(double)pv2.py);
    }
    // first GT4 record (stride 44) after the GT3 block — same offsets as submit_poly_gt4_native
    if (gt4) {
      uint32_t rec = cmd + 4 + gt3 * 36;
      uint32_t vz01 = c->mem_r32(rec + 24);
      uint32_t xy0 = c->mem_r32(rec + 20), xy1 = c->mem_r32(rec + 28), xy2 = c->mem_r32(rec + 32);
      ProjVtx pv0, pv1, pv2;
      eproj_vertex_active((int16_t)xy0, (int16_t)(xy0>>16), (int16_t)vz01, &pv0);
      eproj_vertex_active((int16_t)xy1, (int16_t)(xy1>>16), (int16_t)(vz01>>16), &pv1);
      eproj_vertex_active((int16_t)xy2, (int16_t)(xy2>>16), (int16_t)c->mem_r32(rec+36), &pv2);
      fprintf(stderr, "   gt4 m0=(%d,%d,%d) -> px=%.1f py=%.1f pz=%.1f | m1->(%.0f,%.0f) m2->(%.0f,%.0f)\n",
        (int16_t)xy0,(int16_t)(xy0>>16),(int16_t)vz01, (double)pv0.px,(double)pv0.py,(double)pv0.pz,
        (double)pv1.px,(double)pv1.py,(double)pv2.px,(double)pv2.py);
    }
    logged++;
  }
  // FULL-TABLE texpage/depth scan (later-238): for EVERY GT4 record in the table, log its texpage and the
  // eproj depth (pz) of vert0 — to see (a) whether the tp(576,256) sky/sea backdrop quads are IN this table,
  // and (b) what depth eproj assigns them (near=covers world → eproj mis-projects the backdrop; far=behind).
  { struct TP { int tx, ty, n; float zmin, zmax; } tps[16]; int ntp = 0;
    uint32_t q = es + 0x10, qend = es + 0x10 + (uint32_t)count * 2;
    for (; q < qend; q += 2) {
      uint32_t cmd = base + (uint32_t)c->mem_r16(q) * 4;
      uint32_t s0 = c->mem_r32(cmd);
      uint32_t gt3 = s0 & 0xFF, gt4 = (s0 >> 16) & 0xFF;
      uint32_t rec = cmd + 4 + gt3 * 36;
      for (uint32_t k = 0; k < gt4; k++, rec += 44) {
        uint16_t tp = (uint16_t)(c->mem_r32(rec + 12) >> 16);
        int tx = (tp & 0xF) * 64, ty = ((tp >> 4) & 1) * 256;
        uint32_t vz01 = c->mem_r32(rec + 24), xy0 = c->mem_r32(rec + 20);
        ProjVtx pv; eproj_vertex_active((int16_t)xy0, (int16_t)(xy0 >> 16), (int16_t)vz01, &pv);
        int s = -1; for (int j = 0; j < ntp; j++) if (tps[j].tx == tx && tps[j].ty == ty) { s = j; break; }
        if (s < 0 && ntp < 16) { s = ntp++; tps[s] = (TP){ tx, ty, 0, 1e9f, -1e9f }; }
        if (s >= 0) { tps[s].n++; if (pv.pz < tps[s].zmin) tps[s].zmin = pv.pz; if (pv.pz > tps[s].zmax) tps[s].zmax = pv.pz; }
      }
    }
    for (int j = 0; j < ntp; j++)
      fprintf(stderr, "[groundprobe-tp] tp=(%d,%d) gt4recs=%d eproj_pz=[%.0f .. %.0f]\n",
              tps[j].tx, tps[j].ty, tps[j].n, (double)tps[j].zmin, (double)tps[j].zmax);
  }
  eproj_clear_active();
}

// PSXPORT_DEBUG=rlist — dump the gen_func_8003C048 render-list node TYPES (node+0xb) + jump-table target
// (0x80014DB8[type]) for every node in the list, so we know the full type set the field's phase-2 flush
// walk must handle to be ownable natively. Walk: head *0x800F2624, skip node+1==0, advance node+36. Once
// per frame (first call), then super-calls the original walk.
void ov_rlist_probe(Core* c) {
  static int s_rl_last = -1;
  if (gpu_frame_no(c) != s_rl_last) {     // 1/frame gate (own latch)
    s_rl_last = gpu_frame_no(c);
    fprintf(stderr, "[rlist] f%d:", gpu_frame_no(c));
    uint32_t n = c->mem_r32(0x800F2624u); int guard = 0;
    for (; n && guard < 64; n = c->mem_r32(n + 36), guard++) {
      uint8_t live = c->mem_r8(n + 1), t = c->mem_r8(n + 0xB);
      uint32_t tgt = (t < 33) ? c->mem_r32(0x80014DB8u + t * 4) : 0;
      uint32_t rfn = c->mem_r32(n + 24);      // the node's own render fn ptr (used by the default case)
      fprintf(stderr, " [%08x l%u t%u→%08x rfn=%08x]", n, live, t, tgt, rfn);
    }
    fprintf(stderr, "\n");
  }
  rec_super_call(c, 0x8003C048u);
}

// PSXPORT_DEBUG=ccase — gen_func_8003CCA4 case histogram. The per-object render dispatch selects a
// secondary effect pass by idx = node[0xd]&0xb (idx>=9 = no render). Logs the idx + the jump-table
// target (0x80014ec8[idx]) so we know which cases (and which secondary-pass fns) actually fire at the
// field — i.e. whether owning 8003CCA4 natively needs the secondary passes or is flush-only. Super-calls.
static uint32_t s_cc[12], s_cctgt[12]; static int s_cc_lastf = -1;
void ov_ccase_probe(Core* c) {
  uint32_t node = c->r[4];
  uint32_t idx = c->mem_r8(node + 0xD) & 0xB;
  if (gpu_frame_no(c) != s_cc_lastf) {
    if (s_cc_lastf >= 0) { fprintf(stderr, "[ccase] f%d", s_cc_lastf);
      for (int i = 0; i < 12; i++) if (s_cc[i]) fprintf(stderr, " idx%d=%u(t=%08x)", i, s_cc[i], s_cctgt[i]);
      fprintf(stderr, "\n"); }
    s_cc_lastf = gpu_frame_no(c); for (int i = 0; i < 12; i++) s_cc[i] = 0;
  }
  if (idx < 12) { s_cc[idx]++; s_cctgt[idx] = (idx < 9) ? c->mem_r32(0x80014EC8u + idx * 4) : 0; }
  rec_super_call(c, 0x8003CCA4u);
}
