// class Memcard — the host-backed MEMORY CARD device.
//
// One per Game (`c->game->memcard.method()`); 128 KB backing file. In SBS with two Games each has
// its own memcard instance (only one actually opens the host file — the other is inert). See
// memcard.cpp for the docstring on how Tomba!2 accesses the card via the BIOS libcard/libmcrd and
// file APIs, and why every I/O completes synchronously against a real host file (no SIO IRQ = no
// spin).
#pragma once
#include <cstdint>
#include <cstdio>
struct Core;
struct GameConfig;
class Game;

struct McFd {
  int used;
  int block;
  uint32_t pos;
  uint32_t size;
};

class Memcard {
public:
  Game *game = nullptr; // back-pointer wired by Game()

  static constexpr uint32_t kFrameSize = 128u;
  static constexpr uint32_t kFrames = 1024u;              // 16 blocks × 64 frames
  static constexpr uint32_t kSize = kFrameSize * kFrames; // 128 KB
  static constexpr uint32_t kBlocks = 16u;
  static constexpr uint32_t kBlockFrames = 64u;
  static constexpr uint8_t kDirFree = 0xA0u;
  static constexpr uint8_t kDirUsedFirst = 0x51u;

  // Physical-layer card I/O — host-file backed.
  void init();
  bool present() const {
    return mCard != nullptr;
  }
  // Return TRUE only if the frame was actually moved. A card image that failed to open, or a frame
  // index off the end of the card, used to be a silent no-op that returned zeros — so a transfer the
  // backend could not perform was indistinguishable from one that worked, and the BIOS file API
  // above it announced I/O-END for a save that never landed.
  bool readFrame(uint32_t frame, uint8_t *out128);
  bool writeFrame(uint32_t frame, const uint8_t *in128);

  // PSX card-filesystem helpers (directory scan + free-block allocation).
  int dirFind(const char *name);
  int dirCreate(const char *name, uint32_t size);

  // Directory ENUMERATION — the BIOS firstfile/nextfile pair (B0:0x42 / B0:0x43), which is how the
  // save/load browser discovers what is on the card. `dirScanBegin` arms a scan for a shell pattern
  // (the part after "bu00:", `*` and `?` supported); `dirScanNext` writes the next match into a
  // guest DIRENTRY and returns false once the directory is exhausted. The cursor lives here, per
  // card, because nextfile carries no state of its own beyond the DIRENTRY it is handed.
  //
  // DIRENTRY (Sony libapi, 40 bytes): +0x00 char name[20], +0x14 attr, +0x18 size, +0x1C next,
  // +0x20 head, +0x24 char system[4].
  static constexpr uint32_t kDirEntNameLen = 20u;
  static constexpr uint32_t kDirEntAttr = 0x14u, kDirEntSize = 0x18u, kDirEntNext = 0x1Cu, kDirEntHead = 0x20u,
                            kDirEntBytes = 0x28u;
  void dirScanBegin(const char *pattern);
  bool dirScanNext(Core *c, uint32_t direntVa);

  // BIOS file-API descriptor table (native).
  static constexpr int kFdBase = 3;
  static constexpr int kFdMax = 11;
  int fdAlloc(int block, uint32_t size);
  bool fdValid(int fd) const {
    return fd >= kFdBase && fd < kFdMax && mFd[fd].used;
  }
  McFd *fdAt(int fd) {
    return fdValid(fd) ? &mFd[fd] : nullptr;
  }
  void fdFree(int fd) {
    if (fd >= kFdBase && fd < kFdMax) {
      mFd[fd].used = 0;
    }
  }

  // Diagnostics
  bool verbose() const {
    return mVerbose;
  }
  void setVerbose(bool v) {
    mVerbose = v;
  }

  // Deliver the libcard I/O-complete event (SwCARD/HwCARD EvSpIOE) so callers waiting on TestEvent
  // fall through immediately. Static — routes to the Core's per-Game `class Hle`.
  static void deliverComplete(Core *c);
  // …and its opposite. An operation the backend could not perform completes with the ERROR spec
  // instead, which is the ONLY channel a libmcrd consumer can see a card failure through: the BIOS
  // call's return value is a "busy, retry" flag, so it cannot carry an error (see memcard.cpp).
  static void deliverError(Core *c);

private:
  FILE *mCard = nullptr;
  char mPath[1024] = {0};
  bool mVerbose = false;
  McFd mFd[kFdMax] = {};
  char mScanPat[64] = {0};     // firstfile/nextfile pattern, device prefix already stripped
  uint32_t mScanBlk = kBlocks; // next directory block to examine; kBlocks = scan exhausted/unarmed

  static char *resolvePath(const struct GameConfig *cfg);
  static void mkParents(const char *path);
};

// BIOS dispatch entry points (called from hle.cpp `class Hle`'s dispatchBios via C linkage).
extern "C" int card_hle_a0(uint32_t fn, Core *c);
extern "C" int card_hle_b0(uint32_t fn, Core *c);
void card_overrides_init(Game *game);
