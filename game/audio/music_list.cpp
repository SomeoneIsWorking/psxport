// game/audio/music_list.cpp — see music_list.h.
//
// Loads \CD\TOMBA2.SND once and exposes the 10 BGM songs (the §6 S2SV table) as playable tracks.
// Each song resolves to (seqOff, vabOff): seqOff by linear 'pQES' scan to the song's scan-index,
// vabOff from the container's VAB offset table. native_music_play() drives the shared synth from
// those offsets.
//
// All state (TOMBA2.SND buffer, area bundle copy, current-song index) lives on the `MusicList`
// singleton — audio is one host output stream per process, so class-shape here is a singleton
// (same pattern as class Sbs / class Memcard / class RmlOverlay), not per-Core.
#include "music_list.h"
#include "native_music.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" int disc_read_sector(uint32_t lba, uint8_t* out);                                // runtime/recomp/disc.c
extern "C" int disc_find_file(const char* path, uint32_t* out_lba, uint32_t* out_size);

// Catalogue: track -> (seq scan-index in TOMBA2.SND, vab scan-index, name).
//
// The seq scan-index is RE'd from MAIN.EXE (spec §6 S2SV). The VAB column is the EMPIRICALLY
// AUDIBLE bank, NOT the documented §6 bank-slot: §6.4 warns the bank<->VAB-body binding is
// runtime/area-bound, so the spec's "bank 14 -> vab 13" slot numbers don't address a TOMBA2.SND
// VAB that actually covers the song's notes. The vabs below are empirically-resolved (sweep all
// 24 container VABs per seq, tools/snd_render song <seq> <vab>). After the program-0 tone-selection
// fix (slot[0x26], native_audio.c) ALL 10 songs are audible with these VABs — the earlier
// "needs area VAB" silence was the synth dropping notes, not missing data.
static const struct { int seq, vab; const char* name; } S2SV[10] = {
    {3,  7, "Song 0 (jingle)"},
    {2,  7, "Song 1 (jingle)"},
    {1,  7, "Song 2 (jingle)"},
    {0,  7, "Song 3 (jingle)"},
    {4,  3, "Song 4 (cue)"},
    {5, 21, "Song 5 (cue)"},
    {6, 21, "Song 6 (cue)"},
    {7, 21, "Song 7 (field)"},
    {8, 21, "Song 8 (field)"},
    {9,  7, "Song 9 (field)"},
};

// The area bundle holds the 10 SEPs concatenated from 0x30 (same layout as TOMBA2.SND), then TWO
// area VABs: bank0 'pBAV' @0x26b4 (ps=4) and bank1 'pBAV' @0x38d4 (ps=18). The area songs emit 0xCn
// program-changes up to 15 (verified live, NA_PROGDBG) — those programs only exist in bank1, so we
// bind bank1 as the instrument bank (bank0's 4 programs dropped most notes -> the silent/SFX-tone
// bug). See scratch/handoff_audio_unify.md + docs/journal later-220.
#define AREA_SEP_END 0x26b4   // SEPs occupy [0x30, 0x26b4); first VAB starts here
#define AREA_VAB_OFF 0x38d4   // bank1 (ps=18) — the instrument bank the songs are authored against

MusicList& MusicList::instance() {
    static MusicList inst;
    return inst;
}

MusicList::~MusicList() {
    // Not routinely called (process-scoped singleton), but keeps ownership honest.
    free(mBuf);
    free(mArea);
}

const char* MusicList::name(int i) const {
    if (i < 0 || i >= 10) return nullptr;
    return S2SV[i].name;
}

// The SND container is read from the GAME DISC at runtime (NOT a pre-extracted file — that path is
// machine-local/gitignored and absent on a fresh checkout, which silenced the whole sound test). Uses
// the port's native ISO9660 reader, the same path the game itself loads \CD\TOMBA2.SND through.
int MusicList::loadContainer() {
    if (mBuf) return 0;
    uint32_t lba = 0, size = 0;
    if (!disc_find_file("\\CD\\TOMBA2.SND", &lba, &size) || size == 0) {
        fprintf(stderr, "[music_list] disc_find_file \\CD\\TOMBA2.SND failed\n"); return -1;
    }
    uint32_t nsec = (size + 2047) / 2048;
    mBuf = (uint8_t*)malloc((size_t)nsec * 2048);
    if (!mBuf) return -1;
    for (uint32_t i = 0; i < nsec; i++) {
        if (!disc_read_sector(lba + i, mBuf + (size_t)i * 2048)) {
            fprintf(stderr, "[music_list] disc read failed at sector %u\n", i);
            free(mBuf); mBuf = nullptr; return -1;
        }
    }
    mSize = (long)size;
    fprintf(stderr, "[music_list] loaded \\CD\\TOMBA2.SND from disc (lba=%u, %ld bytes)\n", lba, mSize);
    return 0;
}

// byte offset of the si'th 'pQES' (linear scan from 0x30), or -1.
long MusicList::seqOff(int si) const {
    long o = 0x30; int idx = 0;
    while (o < 0x51800 && o + 4 <= mSize) {
        if (!memcmp(mBuf + o, "pQES", 4)) { if (idx == si) return o; idx++; o += 4; }
        else o++;
    }
    return -1;
}

int MusicList::play(int i) {
    if (i < 0 || i >= 10) return -1;
    if (loadContainer()) return -1;
    long so = seqOff(S2SV[i].seq), vo = vabOff(S2SV[i].vab);
    if (so < 0) { fprintf(stderr, "[music_list] song %d: seq not found\n", i); return -1; }
    if (native_music_play(mBuf, so, vo)) { fprintf(stderr, "[music_list] song %d: play failed\n", i); return -1; }
    mNow = i;
    fprintf(stderr, "[music_list] playing song %d (seq@0x%lx vab@0x%lx)\n", i, so, vo);
    return 0;
}

// byte offset of the si'th 'pQES' within the area bundle (linear scan from 0x30), or -1.
long MusicList::areaSeqOff(int si) const {
    long o = 0x30; int idx = 0;
    while (o + 4 <= mAreaLen && o < AREA_SEP_END) {
        if (!memcmp(mArea + o, "pQES", 4)) { if (idx == si) return o; idx++; o += 4; }
        else o++;
    }
    return -1;
}

int MusicList::playArea(const uint8_t* bundle, long bundle_len, int song) {
    if (!bundle || bundle_len < 0x4000 || song < 0 || song >= 10) return -1;
    // Validate this really is the bundle (SEP at 0x30, area VAB at 0x26b4) before committing.
    if (memcmp(bundle + 0x30, "pQES", 4) || memcmp(bundle + AREA_VAB_OFF, "pBAV", 4)) {
        fprintf(stderr, "[music_list] area bundle invalid (no pQES@0x30 / pBAV@0x26b4)\n");
        return -1;
    }
    long need = bundle_len; if (need > 0x50000) need = 0x50000;
    uint8_t* nb = (uint8_t*)malloc(need);
    if (!nb) return -1;
    memcpy(nb, bundle, need);
    native_music_stop();
    free(mArea); mArea = nb; mAreaLen = need;
    long so = areaSeqOff(song);
    if (so < 0) { fprintf(stderr, "[music_list] area song %d: seq not found\n", song); return -1; }
    if (native_music_play(mArea, so, AREA_VAB_OFF)) { fprintf(stderr, "[music_list] area song %d: play failed\n", song); return -1; }
    mNow = song;
    fprintf(stderr, "[music_list] area BGM song %d (seq@0x%lx vab@0x%lx)\n", song, so, (long)AREA_VAB_OFF);
    return 0;
}

void MusicList::stop() { native_music_stop(); mNow = -1; }

int MusicList::nowPlaying() {
    if (mNow >= 0 && !native_music_active()) mNow = -1;
    return mNow;
}

// ---- Legacy free-function bridges — one-liners over the singleton -------------------------------
extern "C" {
int         music_list_count(void)              { return MusicList::instance().count(); }
const char* music_list_name(int i)              { return MusicList::instance().name(i); }
int         music_list_play(int i)              { return MusicList::instance().play(i); }
void        music_list_stop(void)               { MusicList::instance().stop(); }
int         music_list_now_playing(void)        { return MusicList::instance().nowPlaying(); }
int         music_list_play_area(const uint8_t* bundle, long bundle_len, int song) {
    return MusicList::instance().playArea(bundle, bundle_len, song);
}
}
