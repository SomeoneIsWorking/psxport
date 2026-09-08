#include "fps60_legacy_scene_source.h"
#include "fps60.h"
#include "fps60_game_hooks.h"
#include "game.h"
#include <cstdlib>
#include <lucent/log.h>
#include <utility>

namespace {
void dumpProducerBounds(const RenderQueue &sink) {
  // DIAG (debug channel "tier1sc"): the aggregate screen bbox tier1Render actually re-rendered this
  // present, split by producer — used once to data-derive the scene-table's on-screen footprint for the
  // exactness gate (docs/fps60-rework.md), not load-bearing.
  // The bbox scan below walks every item in the sink (per PRESENT — hot), so the channel gate here
  // guards real non-logging work, not the print.
  static const lucent::Channel ch_tier1sc{"tier1sc"};
  if (ch_tier1sc) {
    float tminx = 1e9f, tmaxx = -1e9f, tminy = 1e9f, tmaxy = -1e9f, sminx = 1e9f, smaxx = -1e9f, sminy = 1e9f,
          smaxy = -1e9f;
    int tn = 0, sn = 0;
    for (int i = 0; i < sink.n; i++) {
      const RqItem &it = sink.items[i];
      float *mnx, *mxx, *mny, *mxy;
      int *cnt;
      if (it.dbg_node == kTerrainDbgNode) {
        mnx = &tminx;
        mxx = &tmaxx;
        mny = &tminy;
        mxy = &tmaxy;
        cnt = &tn;
      } else if (it.dbg_node == kSceneTableDbgNode) {
        mnx = &sminx;
        mxx = &smaxx;
        mny = &sminy;
        mxy = &smaxy;
        cnt = &sn;
      } else {
        continue;
      }
      (*cnt)++;
      for (int k = 0; k < 4; k++) {
        if (it.xsf[k] < *mnx) {
          *mnx = it.xsf[k];
        }
        if (it.xsf[k] > *mxx) {
          *mxx = it.xsf[k];
        }
        if (it.ysf[k] < *mny) {
          *mny = it.ysf[k];
        }
        if (it.ysf[k] > *mxy) {
          *mxy = it.ysf[k];
        }
      }
    }
    lucent::debug(ch_tier1sc,
                  "terrain n={} bbox=[{:.0f},{:.0f},{:.0f},{:.0f}] sceneTable n={} bbox=[{:.0f},{:.0f},{:.0f},{:.0f}]",
                  tn,
                  tn ? tminx : 0.f,
                  tn ? tminy : 0.f,
                  tn ? tmaxx : 0.f,
                  tn ? tmaxy : 0.f,
                  sn,
                  sn ? sminx : 0.f,
                  sn ? sminy : 0.f,
                  sn ? smaxx : 0.f,
                  sn ? smaxy : 0.f);
  }
}

class LegacyTemporalSceneSource final : public TemporalSceneSource {
public:
  explicit LegacyTemporalSceneSource(Game &game) : game_(game) {}

  bool eligible(const Core &) const override {
    return fps60(game_).mTier1EligibleCur;
  }

  bool owns(const RqItem &item) const override {
    // Existing hook consumers reconstruct all float-projected world producers, but only the
    // specifically tagged native backdrop. Guest-time world records and other backgrounds survive.
    return item.layer == RQ_BACKGROUND ? item.dbg_node == kBackdropDbgNode : item.layer == RQ_WORLD && item.has_xyf;
  }

  bool requiresEndpointReconstruction() const override {
    return true;
  }

  void reconstruct(Core &core, float t) override {
    Fps60 &temporal = fps60(game_);
    Fps60::Fps60Cam lerp;
    for (int i = 0; i < 3; i++) {
      for (int j = 0; j < 3; j++) {
        lerp.R[i][j] = temporal.mCamPrev.R[i][j] + (temporal.mCamCur.R[i][j] - temporal.mCamPrev.R[i][j]) * t;
      }
      lerp.T[i] = temporal.mCamPrev.T[i] + (temporal.mCamCur.T[i] - temporal.mCamPrev.T[i]) * t;
    }
    lerp.ofx = temporal.mCamPrev.ofx + (temporal.mCamCur.ofx - temporal.mCamPrev.ofx) * t;
    lerp.ofy = temporal.mCamPrev.ofy + (temporal.mCamCur.ofy - temporal.mCamPrev.ofy) * t;
    lerp.H = temporal.mCamPrev.H + (temporal.mCamCur.H - temporal.mCamPrev.H) * t;
    temporal.mCamOverride = lerp;
    lucent::debug(
        "terrpc",
        "[tier1dbg] t={:.3f} cur.T=({:.1f},{:.1f},{:.1f}) prev.T=({:.1f},{:.1f},{:.1f}) lerp.T=({:.1f},{:.1f},{:.1f}) "
        "cur.H={:.1f} prev.H={:.1f} lerp.H={:.1f} ofx={:.1f} ofy={:.1f}",
        t,
        temporal.mCamCur.T[0],
        temporal.mCamCur.T[1],
        temporal.mCamCur.T[2],
        temporal.mCamPrev.T[0],
        temporal.mCamPrev.T[1],
        temporal.mCamPrev.T[2],
        lerp.T[0],
        lerp.T[1],
        lerp.T[2],
        temporal.mCamCur.H,
        temporal.mCamPrev.H,
        lerp.H,
        lerp.ofx,
        lerp.ofy);

    struct OverrideScope {
      explicit OverrideScope(Fps60 &owner) : owner(owner), previous(std::exchange(owner.mCamOverrideOn, true)) {}
      ~OverrideScope() {
        owner.mCamOverrideOn = previous;
      }
      Fps60 &owner;
      bool previous;
    } overrideScope(temporal);
    if (!game_fps60_world_pass(&core, core.hooks, t)) {
      lucent::error("fps60", "eligible scene has no fps60WorldPass hook to replace its captured geometry");
      std::abort();
    }
    dumpProducerBounds(*game_.rqRedirect);
  }

  void rotate(Core &core) override {
    Fps60 &temporal = fps60(game_);
    std::swap(temporal.mCamCur, temporal.mCamPrev);
    std::swap(temporal.mBgCur, temporal.mBgPrev);
    std::swap(temporal.mObjCur, temporal.mObjPrev);
    temporal.mObjCur.clear();
    game_fps60_bb_swap_prev(&core, core.hooks);
    game_fps60_temporal_rotate(&core, core.hooks);
  }

private:
  Game &game_;
};
} // namespace

std::unique_ptr<TemporalSceneSource> makeLegacyTemporalSceneSource(Game &game) {
  return std::make_unique<LegacyTemporalSceneSource>(game);
}

void Fps60::sceneCam(Core *c, float R[3][3], float T[3], float &ofx, float &ofy, float &H) {
  // TIER 1 override (fps60.h "Object-tier attempt 2026-07-14"): while present_vk's tier1Render() is
  // re-invoking terrainRenderAll() at the interp present, hand back the LERPED camera instead of a guest
  // read — no guest reads at present time, matching the fps60 present-time invariant.
  if (mCamOverrideOn) {
    for (int i = 0; i < 3; i++) {
      for (int j = 0; j < 3; j++) {
        R[i][j] = mCamOverride.R[i][j];
      }
      T[i] = mCamOverride.T[i];
    }
    ofx = mCamOverride.ofx;
    ofy = mCamOverride.ofy;
    H = mCamOverride.H;
    return;
  }
  // The view matrix is GAME state. psxport used to decode Tomba!2's scratchpad layout here, which
  // made every future native producer silently read unrelated memory in another title. The game owns
  // that decode; the framework owns only the capture/lerp around its result.
  if (!game_fps60_read_scene_cam(c, c ? c->hooks : nullptr, R, T)) {
    lucent::error("fps60",
                  "Fps60::sceneCam REFUSED: this game supplied no fps60ReadSceneCam hook; "
                  "a native camera cannot be inferred from framework memory");
    abort();
  }
  // The projection constants come from the GAME'S SETTER (ProjParams::setGeomOffset/setGeomScreen,
  // recorded where libgte SetGeomOffset/SetGeomScreen run), NOT from gte_read_ctrl(24/25/26). Reading
  // them back out of the GTE was asking engine state, after the fact, for constants the game had
  // already stated — the banned pattern, and it coupled the camera to whatever the guest last left in
  // the control registers. Every native producer inherits this call, so this one site is the whole
  // camera path.
  //
  // NO FALLBACK — requireGeom aborts if the game never set a projection, because a fallback would
  // render a plausible picture over an RE gap and make it unfindable.
  c->rsub.projParams.requireGeom("Fps60::sceneCam", ofx, ofy, H);
  // TIER 1 capture: this is a REAL-frame call (mCamOverrideOn is false) — mirror it into mCamCur, the slot
  // that present_vk's end-of-frame rotation advances at the same fence as current-frame capture. Every
  // sceneCam() call this
  // logic frame reads the same unchanged guest camera, so overwriting on every call is idempotent.
  // UNCONDITIONAL. The world is built at PRESENT time in both configs (see presentPass), and this is the
  // camera it is built from — an input to the one renderer, not fps60 machinery. Gating it on active()
  // is what made fps60=0 a second renderer: with no camera captured, a present-time world build runs
  // against a zero camera and collapses the scene to its backdrop.
  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 3; j++) {
      mCamCur.R[i][j] = R[i][j];
    }
    mCamCur.T[i] = T[i];
  }
  mCamCur.ofx = ofx;
  mCamCur.ofy = ofy;
  mCamCur.H = H;
}

// ---- TIER 1 BACKDROP: game-logic-scroll layer-transform lerp (docs/fps60-rework.md) --------------------
void Fps60::bgScroll(Core *c, uint32_t t4, int &scrollX, int &scrollY) {
  if (mBgOverrideOn) {
    scrollX = mBgOverride.scrollX;
    scrollY = mBgOverride.scrollY;
    return;
  }
  scrollX = c->mem_r16s(t4 + 0x28u);
  scrollY = c->mem_r16s(t4 + 0x2au);
  // TIER 1 capture: this is a REAL-frame call — mirror it into mBgCur, rotated with the temporal
  // camera state after the present fence completes.
  mBgCur.scrollX = scrollX;
  mBgCur.scrollY = scrollY; // unconditional, same reason as sceneCam's capture
}

void Fps60::projObj(Core *c, uint32_t cmd, float Robj[3][3], float Tobj[3]) {
  if (mObjOverrideOn) {
    auto pc = mObjCur.find(cmd);
    if (pc != mObjCur.end()) {
      auto pp = mObjPrev.find(cmd);
      const Fps60Obj &C = pc->second;
      if (pp != mObjPrev.end()) {
        const Fps60Obj &P = pp->second;
        for (int i = 0; i < 3; i++) {
          for (int j = 0; j < 3; j++) {
            Robj[i][j] = P.R[i][j] + (C.R[i][j] - P.R[i][j]) * mT;
          }
          Tobj[i] = P.T[i] + (C.T[i] - P.T[i]) * mT;
        }
      } else { // new object this frame — no prev to lerp from, use cur
        for (int i = 0; i < 3; i++) {
          for (int j = 0; j < 3; j++) {
            Robj[i][j] = C.R[i][j];
          }
          Tobj[i] = C.T[i];
        }
      }
      return;
    }
    // cmd not captured this frame (shouldn't happen for a live-walked object) — fall through to a live read.
  }
  // Real frame: read live from guest RAM (the exact read projComposeObject used to do inline).
  for (int col = 0; col < 3; col++) {
    for (int row = 0; row < 3; row++) {
      Robj[row][col] = (float)c->mem_r16s(cmd + 0x18u + (uint32_t)col * 2u + (uint32_t)row * 6u);
    }
  }
  Tobj[0] = (float)c->mem_r16s(cmd + 0x2Cu);
  Tobj[1] = (float)c->mem_r16s(cmd + 0x30u);
  Tobj[2] = (float)c->mem_r16s(cmd + 0x34u);
  // Capture keyed by cmd (host memory only — the READ-ONLY OVERLAY invariant holds).
  Fps60Obj o;
  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 3; j++) {
      o.R[i][j] = Robj[i][j];
    }
    o.T[i] = Tobj[i];
  }
  mObjCur[cmd] = o;
}
