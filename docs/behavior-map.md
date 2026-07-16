# Behavior-difference map — every INTENTIONAL divergence from recomp_path (managed by tools/behavior.py)

Durable ledger of SANCTIONED deviations from the byte-exact reference. Primary axis = GUEST-MEMORY AFFECT (how much canon guest state a deviation touches). One `## ` block per
deviation, grouped by affect. `tools/behavior.py` = view · `... <words>` = search · `... check` = gate (a canon-affecting change must be SBS-suppressed).

**By affect:** 4 none · 1 non-canon · 2 full
**By status:** 1 verified · 4 implemented · 2 planned

---

### **affect: none** — pure host-side overlay, writes NO guest memory. Any guest write is a BUG (SBS catches it).

## pc-render
- **class:** pc_render
- **affect:** none
- **status:** verified
- **flag:** default (pc_render is the default renderer; PSXPORT_RENDER_PSX=1 selects the substrate psx_render instead)
- **original:** PSX GTE compose + OT walk + GP0 packets draw the picture
- **altered:** native float matrices + real depth buffer draw from scene state in its own pass
- **guard:** READ-ONLY OVERLAY: reads guest RAM + engine classes, writes ONLY host memory. Any guest write is a bug (SBS catches it). DisplayPassGuard enforces the scope.
- **owner:** game/render/*
- **notes:** parity-map n/a class (never writes guest RAM). See docs/render-arch.md, CLAUDE.md "Render — reimplement, dont transcribe".

## widescreen
- **class:** widescreen
- **affect:** none
- **status:** implemented
- **flag:** aspect (psxport_settings.ini / F1 overlay): 0=4:3, 1=16:9, 2=21:9, 3=Auto
- **original:** 4:3 FOV
- **altered:** genuinely wider FOV — engine shifts projection center OFX to nw/2 (not a present stretch); culled edge nodes re-included read-only
- **guard:** forced OFF under PSXPORT_ORACLE and in SBS legs (run 4:3), so the byte-exact reference is untouched
- **owner:** game/render/widescreen_margin_quad.cpp; runtime/recomp/mods.c
- **notes:** guest-READ-ONLY re-include of culled nodes (docs/findings render "Widescreen margin renderer").

## fps60
- **class:** fps60
- **affect:** none
- **status:** implemented
- **flag:** 60fps (psxport_settings.ini / F1 overlay)
- **original:** game renders at its native guest frame rate (~30fps)
- **altered:** interpolated in-between frames on the actor-transform tier, one frame behind
- **guard:** interpolation layer only — no guest re-run, no guest writes; reads dbg_node object identity and lerps host transforms
- **owner:** game/render/fps60*.cpp; game/render/render_queue.cpp (Fps60::matchAndLerp)
- **notes:** verify per-object via preseqobj / fps60chk (docs/findings render). Anchor/stamp special-casing is debt.

## ires
- **class:** ires
- **affect:** none
- **status:** implemented
- **flag:** ires (psxport_settings.ini / F1 overlay): 0=Auto,1=1x,2=X2,3=X3,4=X4
- **original:** fixed 1024x512 VRAM render
- **altered:** 3D-world band renders to a VRAM*i target then box-filter downsamples back; 2D always native res
- **guard:** VRAM-space 2D ops + all readback/SBS stay on the fixed texture, untouched; scaled target is host-only
- **owner:** runtime/recomp/gpu_gpu.cpp (render_geom)
- **notes:** internal-resolution scale; ires_downsample.frag coverage-gated (bug #55).

---

### **affect: non-canon** — writes guest memory only to reach the SAME end-state faster. Must byte-match recomp_path at every rendezvous; SBS runs the faithful branch.

## pc-skip
- **class:** pc_skip
- **affect:** non-canon
- **status:** implemented
- **flag:** Game::mPcSkip — per-fork bool; default true (./run.sh shortcuts on); SBS forces false
- **original:** each collapsed init runs its full multi-step faithful sequence
- **altered:** the fork takes a single-step shortcut that lands the same end-state (load_in_one_step)
- **guard:** end-state byte-matches recomp_path at every skip-fork rendezvous; SBS runs the faithful branch (mPcSkip=false); collapse bumps guest tick counters (0x800abde0, 0x1F80017C) so phase-gate consumers hold
- **owner:** per-fork Game::mPcSkip sites
- **notes:** two CD readers: skip=cdlibcd_* ISO9660 direct, faithful=Ghidra-ported libcd chain. SPU register-stream divergences are non-canon (docs/findings audio).

---

### **affect: full** — DELIBERATELY changes canon guest state. MUST be force-suppressed under PSXPORT_ORACLE / SBS (`guard` required) so byte-compares stay clean by construction.

## expanded-load-range
- **class:** pc_enh
- **affect:** full
- **status:** planned
- **flag:** cfg_enh("expanded-load-range") via PSXPORT_ENH=<name,name|all>
- **original:** objects load/unload within the guest engine range window
- **altered:** expanded object load/unload range window (more objects resident)
- **guard:** force-suppressed under PSXPORT_ORACLE / SBS in cfg.c so byte-compares stay enhancement-free
- **owner:** -
- **notes:** PLANNED. Canon guest-state change. Name is the PSXPORT_ENH token; register in docs/config.md when landed.

## faster-transitions
- **class:** pc_enh
- **affect:** full
- **status:** planned
- **flag:** cfg_enh("faster-transitions") via PSXPORT_ENH=<name,name|all>
- **original:** fade / area-transition ramps advance at the guest rate
- **altered:** faster fade/transition ramps
- **guard:** force-suppressed under PSXPORT_ORACLE / SBS in cfg.c so byte-compares stay enhancement-free
- **owner:** -
- **notes:** PLANNED. Canon guest-state change. Register the PSXPORT_ENH token in docs/config.md when landed.
