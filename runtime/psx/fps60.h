// Per-instance temporal presentation. Titles own scene reconstruction through TemporalSceneSource.
#pragma once

#include "frame_presenter.h"
#include "render_queue.h"
#include "temporal_scene_source.h"
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

class Core;
class Game;
struct Fps60;
Fps60 &fps60(Game &game);

// Projected-geometry hold-period telemetry.
struct RateDet {
  uint64_t last_hash;
  int held;
  int period;
  int votes[9];
  long changes;
};

struct Fps60 final : TemporalFramePresentation {
  explicit Fps60(Game &owner, std::unique_ptr<TemporalSceneSource> source = {});
  ~Fps60();

  // Interpolation is enabled only when requested on this Core's native presentation path.
  bool active() const;
  void present(FramePresentationBackend &backend, Core &core, CapturedFrameView frame, int guestFields) override;
  void frame_commit(Core *core, int guestFields = 0);
  void present_vk(FramePresentationBackend &backend, Core *core, CapturedFrameView frame);

  // Both slots use the same reconstruction/merge. t=1 is the real endpoint. Only primitives owned
  // by an eligible source are replaced; every other captured item is emitted verbatim.
  void presentPass(Core *core, float t, CapturedFrameView frame);
  void presentRotate(); // source history advances after both slots, including disabled frames

  Game *game = nullptr;
  RenderQueue *mSink = nullptr;               // lazy isolated reconstruction queue; never the next guest frame's queue
  std::vector<const RqItem *> mPresentStream; // synchronous merge references frame/sink-owned items
  int mHavePrev = 0;
  float mT = 0.5f;
  long mTier1PrimsThisFrame = 0;
  long mBackdropPrimsThisFrame = 0;
  int mCommitGuestFields = 0;

  void fold(uint32_t value);
  void rtp(uint32_t op);
  uint64_t mFrameHash = 1469598103934665603ull;
  long mFrameGeom = 0;
  RateDet mRd = {0, 0, 2, {}, 0};

  // The explicit GameHooks adapter owns these established capture/override chokes. Direct sources
  // own their endpoint state themselves and do not use this latch or the guest-record layouts.
  bool mTier1EligibleCur = false;
  void sceneCam(Core *core, float R[3][3], float T[3], float &ofx, float &ofy, float &H);
  struct Fps60Cam {
    float R[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
    float T[3] = {0, 0, 0};
    float ofx = 0, ofy = 0, H = 0;
  };
  Fps60Cam mCamCur, mCamPrev, mCamOverride;
  bool mCamOverrideOn = false;

  void bgScroll(Core *core, uint32_t address, int &scrollX, int &scrollY);
  struct Fps60Bg {
    int scrollX = 0, scrollY = 0;
  };
  Fps60Bg mBgCur, mBgPrev, mBgOverride;
  bool mBgOverrideOn = false;

  void projObj(Core *core, uint32_t command, float Robj[3][3], float Tobj[3]);
  struct Fps60Obj {
    float R[3][3];
    float T[3];
  };
  std::unordered_map<uint32_t, Fps60Obj> mObjCur, mObjPrev;
  bool mObjOverrideOn = false;
  // WHAT THE INTERP PRESENT COULD ACTUALLY LERP, with its denominator. projObj has three outcomes and
  // two of them draw the object at its CURRENT transform, which is the next real frame's position a
  // whole frame early. Without these counts a scene reconstructing hundreds of prims and a scene
  // lerping none of them print the same `tier1=` number, and the picture cannot tell them apart
  // either (measured: Tomba! 2's opening narration, 233 prims reconstructed per present, every
  // changed pixel at the next endpoint whatever t is).
  struct ObjLerpCensus {
    uint32_t lerped = 0;     // prev and cur both present: the object genuinely interpolated
    uint32_t noPrev = 0;     // captured this frame, but the previous frame never drew this cmd
    uint32_t uncaptured = 0; // not in mObjCur at all — fell through to a live guest read
    uint32_t total() const {
      return lerped + noPrev + uncaptured;
    }
    void reset() {
      lerped = noPrev = uncaptured = 0;
    }
  };
  ObjLerpCensus mObjLerp;

  // Capture-only producers omit their guest-time draw even with interpolation disabled. The adapter
  // therefore requests a current-endpoint reconstruction for those frames as well.
  bool mWorldCaptureOnly = false;

private:
  void tier1Render(Core *core, float t);
  std::unique_ptr<TemporalSceneSource> sceneSource_;
};
