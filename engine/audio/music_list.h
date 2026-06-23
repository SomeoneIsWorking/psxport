// engine/audio/music_list.h — enumerate "all the game's music" and play a track natively.
//
// The Sound Test's catalogue. v1 = the 10 SEP sequences in the global container TOMBA2.SND with
// the documented song->VAB mapping (spec §6 S2SV: songs 0-3,7-9 -> vab idx 13; songs 4-6 -> vab
// idx 7). The container is loaded once from scratch/bin/snd/TOMBA2.SND at first use. Each entry
// resolves to a (seqOff, vabOff) byte pair into that buffer, fed straight to native_music_play().
//
// Used by BOTH the REPL `musictest <n>` command and the RmlUi Sound Test pane.
#ifndef MUSIC_LIST_H
#define MUSIC_LIST_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Number of catalogued tracks.
int         music_list_count(void);
// Display name for track i (e.g. "Song 4"), or NULL if out of range.
const char* music_list_name(int i);
// Play catalogued track i natively (loads the container lazily). Returns 0 ok, -1 on error.
int         music_list_play(int i);
// Stop playback.
void        music_list_stop(void);
// Index of the currently-playing track, or -1 if none / stopped.
int         music_list_now_playing(void);

// IN-GAME: play the field BGM for `song` (0..9) from the LIVE area bundle. `bundle` points at the
// area bundle in guest RAM (guest 0x80182000); `bundle_len` = bytes safe to copy. The bundle is
// copied into an engine-owned buffer (survives area data churn); the song's SEP is found by 'pQES'
// scan (song id == scan index, spec §6.3); the area VAB (+0x26b4) drives the synth. Returns 0 ok,
// -1 on error. Used by the BGM-start hook in engine/sound.cpp.
int         music_list_play_area(const uint8_t* bundle, long bundle_len, int song);

#ifdef __cplusplus
}
#endif
#endif
