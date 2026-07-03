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
class  Game;

struct McFd { int used; int block; uint32_t pos; uint32_t size; };

class Memcard {
public:
  Game* game = nullptr;   // back-pointer wired by Game()

  static constexpr uint32_t kFrameSize   = 128u;
  static constexpr uint32_t kFrames      = 1024u;                          // 16 blocks × 64 frames
  static constexpr uint32_t kSize        = kFrameSize * kFrames;           // 128 KB
  static constexpr uint32_t kBlocks      = 16u;
  static constexpr uint32_t kBlockFrames = 64u;
  static constexpr uint8_t  kDirFree     = 0xA0u;
  static constexpr uint8_t  kDirUsedFirst= 0x51u;

  // Physical-layer card I/O — host-file backed.
  void init();
  bool present() const { return mCard != nullptr; }
  void readFrame (uint32_t frame, uint8_t* out128);
  void writeFrame(uint32_t frame, const uint8_t* in128);

  // PSX card-filesystem helpers (directory scan + free-block allocation).
  int  dirFind  (const char* name);
  int  dirCreate(const char* name, uint32_t size);

  // BIOS file-API descriptor table (native).
  static constexpr int kFdBase = 3;
  static constexpr int kFdMax  = 11;
  int   fdAlloc(int block, uint32_t size);
  bool  fdValid(int fd) const { return fd >= kFdBase && fd < kFdMax && mFd[fd].used; }
  McFd* fdAt   (int fd)        { return fdValid(fd) ? &mFd[fd] : nullptr; }
  void  fdFree (int fd)        { if (fd >= kFdBase && fd < kFdMax) mFd[fd].used = 0; }

  // Diagnostics
  bool verbose() const { return mVerbose; }
  void setVerbose(bool v) { mVerbose = v; }

  // Deliver the libcard I/O-complete event (SwCARD/HwCARD SUCCESS + EvSpIOE) so callers waiting on
  // TestEvent fall through immediately. Static — routes to the Core's per-Game `class Hle`.
  static void deliverComplete(Core* c);

private:
  FILE* mCard = nullptr;
  char  mPath[1024] = {0};
  bool  mVerbose = false;
  McFd  mFd[kFdMax] = {};

  static char* resolvePath();
  static void  mkParents(const char* path);
};

// BIOS dispatch entry points (called from hle.cpp `class Hle`'s dispatchBios via C linkage).
extern "C" int  card_hle_a0(uint32_t fn, Core* c);
extern "C" int  card_hle_b0(uint32_t fn, Core* c);
void card_overrides_init(Game* game);
