// game/audio/music_list.h — enumerate "all the game's music" and play a track natively.
//
// The Sound Test's catalogue. v1 = the 10 SEP sequences in the global container TOMBA2.SND with
// the documented song->VAB mapping (spec §6 S2SV: songs 0-3,7-9 -> vab idx 13; songs 4-6 -> vab
// idx 7). The container is loaded once from \CD\TOMBA2.SND at first use. Each entry resolves to a
// (seqOff, vabOff) byte pair into that buffer, fed straight to native_music_play().
//
// One `class MusicList` singleton per process (audio is one host output stream). The legacy
// `music_list_*` C entries are thin bridges over `MusicList::instance()` so the REPL / RmlUi Sound
// Test / engine BGM-start hook link unchanged.
#ifndef MUSIC_LIST_H
#define MUSIC_LIST_H
#include <stdint.h>

#ifdef __cplusplus
class MusicList {
public:
  static MusicList& instance();

  int         count() const { return 10; }
  const char* name(int i) const;      // NULL if out of range
  int         play(int i);            // 0 ok, -1 err
  void        stop();
  int         nowPlaying();           // -1 if none / stopped (also clears cached value if playback ended)

  // IN-GAME: play the field BGM for `song` (0..9) from the LIVE area bundle. The bundle is copied
  // into an engine-owned buffer so playback survives area data churn. Returns 0 ok, -1 on error.
  int         playArea(const uint8_t* bundle, long bundle_len, int song);

private:
  MusicList() = default;
  ~MusicList();
  MusicList(const MusicList&) = delete;
  MusicList& operator=(const MusicList&) = delete;

  int  loadContainer();
  long seqOff(int si) const;
  long vabOff(int vi) const { return (long)(mBuf[vi*2] | (mBuf[vi*2+1] << 8)) * 0x800; }
  long areaSeqOff(int si) const;

  uint8_t* mBuf  = nullptr;   // TOMBA2.SND contents (Sound Test catalogue)
  long     mSize = 0;
  int      mNow  = -1;
  uint8_t* mArea    = nullptr;   // engine-owned copy of the live area bundle (in-game BGM)
  long     mAreaLen = 0;
};
#endif // __cplusplus

// Legacy free-function API — thin bridges to `MusicList::instance()`.
#ifdef __cplusplus
extern "C" {
#endif
int         music_list_count(void);
const char* music_list_name(int i);
int         music_list_play(int i);
void        music_list_stop(void);
int         music_list_now_playing(void);
int         music_list_play_area(const uint8_t* bundle, long bundle_len, int song);
#ifdef __cplusplus
}
#endif
#endif
