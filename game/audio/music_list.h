// game/audio/music_list.h — enumerate "all the game's music" and play a track natively.
//
// The Sound Test's catalogue. v1 = the 10 SEP sequences in the global container TOMBA2.SND with
// the documented song->VAB mapping (spec §6 S2SV: songs 0-3,7-9 -> vab idx 13; songs 4-6 -> vab
// idx 7). The container is loaded once from \CD\TOMBA2.SND at first use. Each entry resolves to
// a (seqOff, vabOff) byte pair into that buffer, fed straight to native_music.play().
//
// One `class MusicList` per Core, on the game-side TombaCtx aggregate (gctx(c)->music_list). Audio
// is one host output stream so per-Core each has its own catalogue (only the Core whose Game owns
// the SDL device via SpuAudio is actually audible). Holds a Core* and reaches the disc backend
// (core->game->disc) + its sibling native_music (gctx(core)->native_music) at call time.
#ifndef MUSIC_LIST_H
#define MUSIC_LIST_H
#include <stdint.h>

#ifdef __cplusplus
class Core;

class MusicList {
public:
  Core* core = nullptr;   // back-pointer wired by tomba_ctx_create (game_ctx.cpp)
  ~MusicList();

  int         count() const { return 10; }
  const char* name(int i) const;      // NULL if out of range
  int         play(int i);            // 0 ok, -1 err
  void        stop();
  int         nowPlaying();           // -1 if none / stopped (also clears cached value if playback ended)

  // IN-GAME: play the field BGM for `song` (0..9) from the LIVE area bundle. The bundle is copied
  // into a Game-owned buffer so playback survives area data churn. Returns 0 ok, -1 on error.
  int         playArea(const uint8_t* bundle, long bundle_len, int song);

private:
  int  loadContainer();
  long seqOff(int si) const;
  long vabOff(int vi) const { return (long)(mBuf[vi*2] | (mBuf[vi*2+1] << 8)) * 0x800; }
  long areaSeqOff(int si) const;

  uint8_t* mBuf  = nullptr;   // TOMBA2.SND contents (Sound Test catalogue)
  long     mSize = 0;
  int      mNow  = -1;
  uint8_t* mArea    = nullptr;   // per-Game copy of the live area bundle (in-game BGM)
  long     mAreaLen = 0;
};
#endif // __cplusplus
#endif
