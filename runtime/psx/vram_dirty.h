#pragma once
#include <stdint.h>

// ---- WHICH PARTS OF GUEST VRAM A PRESENT MUST RE-UPLOAD INTO THE COMPOSITE --------------------------
//
// THE COMPOSITE IS A FRAMEBUFFER, NOT A PER-FRAME DRAWING. On the console, VRAM persists: the GPU
// rasterizes into it, the CPU DMAs into it, and the display scans a rect out of it every field. Nothing
// is "rebuilt". A double-buffered game therefore draws frame N into the buffer it is NOT displaying, and
// that drawing must still be there one frame later, when that buffer becomes the front buffer.
//
// The renderer used to break exactly that. `upload_vram()` memcpy'd ALL 1024x512 of guest CPU VRAM over
// the composite on every present — and under `vk_path()` the guest's POLYGONS never reach CPU VRAM (they
// go to the VK rasterizer; only GP0 uploads/fills/copies land in the CPU array). So every present erased
// every pixel the rasterizer had ever drawn, and the only geometry on screen was whatever had been
// re-submitted since the last present. MEASURED on Spyro (windowed, the user's own pad replay, presents
// 2200..2219): 20/20 consecutive presents alternated 3169-ish distinct colours / EXACTLY 2, where the
// 2-colour frames are the guest's raw uploaded clear fill (0xffdead) with no geometry at all. The queue
// y-ranges on those same presents show the two buffers plainly — y=[-93..332] and y=[147..572], 240
// apart — and the frames that DID show a scene only did so because RenderQueue::flush re-emits an
// already-consumed queue on the guest's idle field. Removing that re-emit alone turned 20/20 presents
// FLAT, which is what proved the composite has no persistence of its own.
//
// So: upload only what the GUEST ACTUALLY WROTE since the composite was last brought up to date. That is
// the same set of writes `gpu_vk_dirty()` has always been handed (GP0 0xA0 upload, GP0 0x02 fill,
// VRAM->VRAM copy, native load_image) and, until now, thrown away — it kept only a COUNT, because a
// blanket re-upload made the rects redundant. This class is that discarded rect.
//
// WHY A LIST AND NOT A BOUNDING BOX. A single bbox over "the fill of buffer B" and "a texture upload at
// the top of VRAM" spans both display buffers, so it would re-erase the front buffer — the exact bug,
// reintroduced by a data-structure choice. The list keeps writes apart.
//
// OVERFLOW IS CONSERVATIVE AND COUNTED, NEVER SILENT. Above CAP rects the two whose union grows the
// least are merged. That can only ever upload MORE than the guest wrote (stale CPU-VRAM pixels over
// rendered ones in the merged span) — never less, so no guest write is ever dropped — and `merges()`
// reports how often it happened so a port that lives in that regime is visible rather than mysterious.
// `all()` (the initial state, and after `markAll()`) means "the composite's contents are unknown, upload
// everything": that is what a freshly created GPU texture needs, and it is the safe direction.

struct VramDirtyRect {
  int x, y, w, h;
};

class VramDirty {
public:
  // Sized for the real per-present write volume, not for a hypothetical: Spyro issues 2 CPU->VRAM writes
  // per guest frame, Tomba!2's upload-only screens a handful. 64 leaves two orders of magnitude of head-
  // room before a single merge occurs, and a merge is correct anyway.
  static constexpr int CAP = 64;

  // The canvas every rect is clipped to. Until it is set, the object stays in the `all()` state — a
  // write it cannot clip is a write it must not silently narrow.
  void setCanvas(int w, int h) {
    if (w == cw_ && h == ch_) {
      return;
    }
    cw_ = w;
    ch_ = h;
    markAll(); // a canvas change invalidates whatever the composite held
  }

  // "The composite's content is unknown." Next upload is the whole canvas.
  void markAll() {
    all_ = true;
    n_ = 0;
  }

  // Record one guest CPU->VRAM write. Clipped to the canvas; a fully off-canvas or empty write is
  // dropped and COUNTED (dropped()), because "nothing was recorded" and "nothing was written" must not
  // look the same from the outside.
  void add(int x, int y, int w, int h) {
    ++adds_;
    if (cw_ <= 0 || ch_ <= 0) {
      markAll();
      return;
    } // no canvas: cannot clip, so cannot narrow
    if (w <= 0 || h <= 0) {
      ++dropped_;
      return;
    }
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    if (x0 < 0) {
      x0 = 0;
    }
    if (y0 < 0) {
      y0 = 0;
    }
    if (x1 > cw_) {
      x1 = cw_;
    }
    if (y1 > ch_) {
      y1 = ch_;
    }
    if (x0 >= x1 || y0 >= y1) {
      ++dropped_;
      return;
    }
    if (all_) {
      return; // already uploading everything
    }
    VramDirtyRect r{x0, y0, x1 - x0, y1 - y0};
    for (int i = 0; i < n_; i++) {
      if (contains(r_[i], r)) {
        return; // already covered
      }
    }
    if (n_ >= CAP) {
      mergeCheapestPair();
      ++merges_;
    }
    r_[n_++] = r;
  }

  bool all() const {
    return all_;
  }
  bool empty() const {
    return !all_ && n_ == 0;
  }
  int count() const {
    return all_ ? 0 : n_;
  }
  VramDirtyRect at(int i) const {
    return r_[i];
  }
  // The whole canvas as one rect — what a caller uploads when all() is set.
  VramDirtyRect whole() const {
    return VramDirtyRect{0, 0, cw_, ch_};
  }

  // Whether any pending guest write can change the specified display rectangle. Offscreen
  // texture/CLUT writes remain pending for the next composite build but cannot change scanout.
  bool intersects(int x, int y, int w, int h) const {
    if (w <= 0 || h <= 0 || cw_ <= 0 || ch_ <= 0) {
      return false;
    }
    const VramDirtyRect target{x, y, w, h};
    if (all_) {
      return overlaps(whole(), target);
    }
    for (int i = 0; i < n_; ++i) {
      if (overlaps(r_[i], target)) {
        return true;
      }
    }
    return false;
  }

  // Called once the composite has been brought up to date with everything recorded so far.
  void clear() {
    all_ = false;
    n_ = 0;
  }

  // Accounting, so any log line about this can carry its denominator.
  uint64_t adds() const {
    return adds_;
  }
  uint64_t merges() const {
    return merges_;
  }
  uint64_t dropped() const {
    return dropped_;
  }

private:
  static bool contains(const VramDirtyRect &a, const VramDirtyRect &b) {
    return b.x >= a.x && b.y >= a.y && b.x + b.w <= a.x + a.w && b.y + b.h <= a.y + a.h;
  }
  static bool overlaps(const VramDirtyRect &a, const VramDirtyRect &b) {
    return a.x < b.x + b.w && a.x + a.w > b.x && a.y < b.y + b.h && a.y + a.h > b.y;
  }
  static long area(const VramDirtyRect &a) {
    return (long)a.w * (long)a.h;
  }
  static VramDirtyRect unite(const VramDirtyRect &a, const VramDirtyRect &b) {
    int x0 = a.x < b.x ? a.x : b.x, y0 = a.y < b.y ? a.y : b.y;
    int x1 = a.x + a.w > b.x + b.w ? a.x + a.w : b.x + b.w;
    int y1 = a.y + a.h > b.y + b.h ? a.y + a.h : b.y + b.h;
    return VramDirtyRect{x0, y0, x1 - x0, y1 - y0};
  }
  // Merge the pair whose union adds the least area, freeing one slot. O(CAP^2) but only on overflow.
  void mergeCheapestPair() {
    int bi = 0, bj = 1;
    long best = -1;
    for (int i = 0; i < n_; i++) {
      for (int j = i + 1; j < n_; j++) {
        long grow = area(unite(r_[i], r_[j])) - area(r_[i]) - area(r_[j]);
        if (best < 0 || grow < best) {
          best = grow;
          bi = i;
          bj = j;
        }
      }
    }
    r_[bi] = unite(r_[bi], r_[bj]);
    r_[bj] = r_[--n_];
  }

  VramDirtyRect r_[CAP] = {};
  int n_ = 0;
  bool all_ = true; // a composite nobody has uploaded into yet holds nothing we know about
  int cw_ = 0, ch_ = 0;
  uint64_t adds_ = 0, merges_ = 0, dropped_ = 0;
};

// THE DECISION ITSELF, as one pure function, so the renderer and the test run the SAME code rather
// than the test running a transcription of it. `out` receives the regions a present must copy from
// guest CPU VRAM into the composite; the return value is how many (0 = nothing to upload, and that is
// a real answer, not a failure). `all()` yields exactly one whole-canvas region.
static inline int vram_upload_regions(const VramDirty &d, VramDirtyRect *out, int cap) {
  if (cap <= 0) {
    return 0;
  }
  if (d.all()) {
    out[0] = d.whole();
    return 1;
  }
  int n = d.count() < cap ? d.count() : cap;
  for (int i = 0; i < n; i++) {
    out[i] = d.at(i);
  }
  return n;
}
