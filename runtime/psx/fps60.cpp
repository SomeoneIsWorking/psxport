#include "fps60.h"
#include "cfg.h"
#include "core.h"
#include "fps60_gpu_present.h"
#include "fps60_sequence_runs.h"
#include "game.h"        // Game-owned optional temporal product and RenderQueue
#include "mods.h"        // Mods (game->mods.fps60)
#include "proj_params.h" // ProjParams — the camera's projection constants + Snapshot save/restore
#include "render_mode.h" // DisplayPassGuard — display-pass FAIL-FAST guard (framework)
#include "render_queue.h"
#include <lucent/log.h>
#include <stdint.h>
#include <stdlib.h>
#include <utility>
#include <vector>

extern "C" {
uint32_t GTE_ReadDR(unsigned);
} // Beetle GTE (mednafen gte.c) — RTP result regs (rate tap)

Fps60::Fps60(Game &owner, std::unique_ptr<TemporalSceneSource> source)
    : game(&owner), sceneSource_(std::move(source)) {}

Fps60::~Fps60() {
  delete mSink;
}

Fps60 &fps60(Game &game) {
  auto *temporal = dynamic_cast<Fps60 *>(game.temporalPresentation.get());
  if (!temporal) {
    lucent::error("fps60", "temporal interpolation was requested from a Game that did not create it");
    std::abort();
  }
  return *temporal;
}

// ---- logic-rate detector (validated lrate_proto) -----------------------------------------------------
void Fps60::fold(uint32_t v) {
  uint64_t h = mFrameHash;
  for (int i = 0; i < 4; i++) {
    h ^= (v & 0xFF);
    h *= 1099511628211ull;
    v >>= 8;
  }
  mFrameHash = h;
  mFrameGeom++;
}
// gte RTP tap (fps60 gate): fold this vertex's projected SXY into the frame fingerprint. RTPS(0x01) writes
// one SXY (DR14); RTPT(0x30) writes three (DR12/13/14). This is the ONLY remaining GTE tap — it feeds the
// rate detector so the tier knows the logic rate (Tomba2 = 30fps → one in-between per frame).
// See the declaration in fps60.h: the user's toggle AND the render path must both allow it.
bool Fps60::active() const {
  return game && game->mods.fps60 && game->core.rsub.mode.enhancementsAllowed();
}

void Fps60::rtp(uint32_t op) {
  if (!active()) {
    return;
  }
  unsigned lo = (op == 0x30) ? 12 : 14;
  for (unsigned r = lo; r <= 14; r++) {
    fold(GTE_ReadDR(r));
  }
}
static void rate_tick(RateDet *d, uint64_t set_hash) {
  if (set_hash == d->last_hash) {
    d->held++;
    return;
  }
  int p = d->held + 1;
  if (p >= 1 && p <= 8) {
    d->votes[p]++;
  }
  int best = 0, bp = 2;
  for (int i = 1; i <= 8; i++) {
    if (d->votes[i] > best) {
      best = d->votes[i];
      bp = i;
    }
  }
  d->period = bp;
  d->last_hash = set_hash;
  d->held = 0;
  d->changes++;
}

namespace {
const lucent::Channel sequenceChannel{"fps60seq"};

void dumpSequenceRuns(CapturedFrameView frame, float t, const TemporalSceneSource *source) {
  lucent::debug(sequenceChannel, "f{} t={:.3f} captured n={}", frame.fence, t, frame.items.size());
  // The painter object is part of the run key, so a verbatim run names the producer that emitted
  // it. Grouping by layer alone made "verbatim n=478" span every producer drawing into RQ_WORLD and
  // name none of them, which is the one thing a reader needs before choosing what to reconstruct
  // next.
  std::vector<psxport::fps60::SequenceRun> runs;
  psxport::fps60::groupSequenceRuns(
      frame.items,
      [source](const RqItem &item) {
        return source != nullptr && source->owns(item);
      },
      runs);
  for (const auto &run : runs) {
    lucent::debug(sequenceChannel,
                  "  rqcur layer={} {:<9} n={} seq=[{}..{}] producer={:08X} node0={:08X}",
                  run.layer,
                  run.owned ? "TIER1" : "verbatim",
                  run.count(),
                  frame.items[run.begin].seq,
                  frame.items[run.end - 1].seq,
                  (uint32_t)run.painterObject,
                  frame.items[run.begin].dbg_node);
  }
}

class ReconstructionScope {
public:
  ReconstructionScope(Core &core, RenderQueue &sink)
      : core_(core), projection_(core.rsub.projParams.snapshot()),
        redirect_(std::exchange(core.game->rqRedirect, &sink)), display_(core.rsub.mode) {}
  ~ReconstructionScope() {
    core_.game->rqRedirect = redirect_;
    core_.rsub.projParams.restore(projection_);
  }

private:
  Core &core_;
  ProjParams::Snapshot projection_;
  RenderQueue *redirect_;
  DisplayPassGuard display_;
};
} // namespace

void Fps60::tier1Render(Core *core, float t) {
  if (!mSink) {
    mSink = new RenderQueue();
    mSink->game = game;
  }
  mSink->reset();
  {
    ReconstructionScope scope(*core, *mSink);
    sceneSource_->reconstruct(*core, t);
  }
  // Apply the same authored ordering resolution as the real queue before merging.
  mSink->finalize(core, "fps60-tier1");
  for (int i = 0; i < mSink->n; ++i) {
    if (mSink->items[i].layer == RQ_BACKGROUND) {
      ++mBackdropPrimsThisFrame;
    }
  }
  mTier1PrimsThisFrame = mSink->n - mBackdropPrimsThisFrame;
}

void Fps60::frame_commit(Core *core, int guestFields) {
  game->presentation.commit(core, guestFields, this);
}

void Fps60::present(FramePresentationBackend &backend, Core &core, CapturedFrameView frame, int guestFields) {
  mCommitGuestFields = guestFields;
  if (active()) {
    uint64_t set_hash = (mFrameGeom > 0) ? mFrameHash : 0xFFFFFFFFFFFFFFFFull;
    rate_tick(&mRd, set_hash);
  }
  present_vk(backend, &core, frame);
  mFrameHash = 1469598103934665603ull;
  mFrameGeom = 0;
}

// Both slots use the source's same reconstruction and captured-queue merge; only t differs.
void Fps60::present_vk(FramePresentationBackend &backend, Core *core, CapturedFrameView frame) {
  Core *c = core;
  RenderQueue &q = c->game->rq;

  const int tforce = cfg_int("PSXPORT_FPS60_TFORCE", -1);
  const float tInterp = (tforce == 0) ? 0.0f : (tforce == 1) ? 1.0f : 0.5f;
  const bool extraFrame = active() && sceneSource_ && mHavePrev && sceneSource_->eligible(*c);

  if (extraFrame) {
    presentPass(c, tInterp, frame);
    gpu_fps60_present_pass(c);
    backend.captureDiagnostic(frame.fence, /*interpolated=*/true);
    // Was an info line behind a latched `fps60` channel test — a per-present line that only ever appeared
    // when the channel was asked for, so it is debug audience, not info.
    lucent::debug("fps60",
                  "f{} slotA: replay prev={} n={} tier1={} backdrop={} t={:.3f}",
                  frame.fence,
                  mHavePrev ? "Q[N-1]" : "Q[N] (first frame)",
                  frame.items.size(),
                  mTier1PrimsThisFrame,
                  mBackdropPrimsThisFrame,
                  mT);
    backend.pace(mCommitGuestFields, 2);
  }

  // ---- PASS 2 (slot B): the real frame. SAME call, t=1 — every lerped input resolves to its current
  // value, so this is the in-between at its near endpoint rather than a separate replay of the captured
  // queue. That is what makes the symmetry structural instead of a property two code paths happen to
  // share.
  q.mLedger.inRealPresent = true; // only the real present counts as "reached the screen"
  presentPass(c, 1.0f, frame);
  q.mLedger.inRealPresent = false;
  backend.presentReal();
  backend.captureDiagnostic(frame.fence, /*interpolated=*/false);
  // Pacing differs only BECAUSE the extra frame does: two half-frames when an in-between was inserted,
  // one whole frame when it was not.
  if (extraFrame) {
    backend.pace(mCommitGuestFields, 2);
  } else {
    backend.pace(mCommitGuestFields, 1);
  }

  presentRotate();
}

void Fps60::presentPass(Core *c, float t, CapturedFrameView frame) {
  RenderQueue &q = c->game->rq;
  mT = t;
  mTier1PrimsThisFrame = 0;
  mBackdropPrimsThisFrame = 0;
  const bool tier1 = sceneSource_ && c->rsub.mode.enhancementsAllowed() &&
                     (active() || (t == 1.0f && sceneSource_->requiresEndpointReconstruction())) &&
                     sceneSource_->eligible(*c);
  if (sequenceChannel) { // guards the queue scan, not a logging call
    dumpSequenceRuns(frame, t, tier1 ? sceneSource_.get() : nullptr);
  }
  if (tier1) {
    tier1Render(c, t);
  }
  // Only this source's producers are replaced; unrelated native world and backdrop items
  // retain their exact captured values and position in the authored layer/sequence ordering.
  const int sinkN = (tier1 && mSink) ? mSink->n : 0;
  mPresentStream.clear();
  const int currentCount = static_cast<int>(frame.items.size());
  mPresentStream.reserve(static_cast<size_t>(sinkN) + frame.items.size());
  int ia = 0, ib = 0;
  for (;;) {
    while (ib < currentCount && tier1 && sceneSource_->owns(frame.items[ib])) {
      ib++; // world prims come from mSink — skip in the captured current frame
    }
    const bool haveA = ia < sinkN, haveB = ib < currentCount;
    if (!haveA && !haveB) {
      break;
    }
    bool takeSink;
    if (!haveB) {
      takeSink = true;
    } else if (!haveA) {
      takeSink = false;
    } else {
      const RqItem &sa = mSink->items[ia];
      const RqItem &sb = frame.items[ib];
      takeSink = (sa.layer != sb.layer) ? (sa.layer < sb.layer) : (sa.seq <= sb.seq);
    }
    if (takeSink) {
      mPresentStream.push_back(&mSink->items[ia++]);
    } else {
      mPresentStream.push_back(&frame.items[ib++]);
    }
  }
  q.emitItemStream(c, mPresentStream);
}

void Fps60::presentRotate() {
  if (sceneSource_) {
    sceneSource_->rotate(game->core);
  }
  mHavePrev = 1;
}
