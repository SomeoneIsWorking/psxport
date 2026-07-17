// game/audio/music_list.cpp — see music_list.h.
//
// Loads \CD\TOMBA2.SND once and exposes the 10 BGM songs (the §6 S2SV table) as playable tracks.
// Each song resolves to (seqOff, vabOff): seqOff by linear 'pQES' scan to the song's scan-index,
// vabOff from the container's VAB offset table. `game->native_music.play()` drives the shared
// synth from those offsets.
#include "music_list.h"
#include "native_music.h"
#include "game.h"       // core->game->disc — the native disc backend lives on the framework Game
#include "game_ctx.h"   // gctx(core)->native_music — the sibling player on the game aggregate

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "disc.h"

// Catalogue: track -> (seq scan-index in TOMBA2.SND, vab scan-index, name).
//
// The seq scan-index is RE'd from MAIN.EXE (spec §6 S2SV). The VAB column is the EMPIRICALLY
// AUDIBLE bank, NOT the documented §6 bank-slot: §6.4 warns the bank<->VAB-body binding is
// runtime/area-bound. The vabs below are empirically-resolved (sweep all 24 container VABs per
// seq, tools/snd_render song <seq> <vab>).
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
// area VABs: bank0 'pBAV' @0x26b4 (ps=4) and bank1 'pBAV' @0x38d4 (ps=18). Bank1 is the instrument
// bank the songs are authored against (bank0 = 4 programs, drops most notes).
#define AREA_SEP_END 0x26b4   // SEPs occupy [0x30, 0x26b4); first VAB starts here
#define AREA_VAB_OFF 0x38d4   // bank1 (ps=18) — the instrument bank the songs are authored against

MusicList::~MusicList() { free(mBuf); free(mArea); }

const char* MusicList::name(int i) const {
    if (i < 0 || i >= 10) return nullptr;
    return S2SV[i].name;
}

// The SND container is read from the GAME DISC at runtime via the port's native ISO9660 reader.
int MusicList::loadContainer() {
    if (mBuf) return 0;
    uint32_t lba = 0, size = 0;
    if (!disc_find_file(&core->game->disc, "\\CD\\TOMBA2.SND", &lba, &size) || size == 0) {
        fprintf(stderr, "[music_list] disc_find_file \\CD\\TOMBA2.SND failed\n"); return -1;
    }
    uint32_t nsec = (size + 2047) / 2048;
    mBuf = (uint8_t*)malloc((size_t)nsec * 2048);
    if (!mBuf) return -1;
    for (uint32_t i = 0; i < nsec; i++) {
        if (!disc_read_sector(&core->game->disc, lba + i, mBuf + (size_t)i * 2048)) {
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
    if (i < 0 || i >= 10 || !core) return -1;
    if (loadContainer()) return -1;
    long so = seqOff(S2SV[i].seq), vo = vabOff(S2SV[i].vab);
    if (so < 0) { fprintf(stderr, "[music_list] song %d: seq not found\n", i); return -1; }
    if (gctx(core)->native_music.play(mBuf, so, vo)) { fprintf(stderr, "[music_list] song %d: play failed\n", i); return -1; }
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
    if (!bundle || bundle_len < 0x4000 || song < 0 || song >= 10 || !core) return -1;
    // Validate this really is the bundle (SEP at 0x30, area VAB at 0x26b4) before committing.
    if (memcmp(bundle + 0x30, "pQES", 4) || memcmp(bundle + AREA_VAB_OFF, "pBAV", 4)) {
        fprintf(stderr, "[music_list] area bundle invalid (no pQES@0x30 / pBAV@0x26b4)\n");
        return -1;
    }
    long need = bundle_len; if (need > 0x50000) need = 0x50000;
    uint8_t* nb = (uint8_t*)malloc(need);
    if (!nb) return -1;
    memcpy(nb, bundle, need);
    gctx(core)->native_music.stop();
    free(mArea); mArea = nb; mAreaLen = need;
    long so = areaSeqOff(song);
    if (so < 0) { fprintf(stderr, "[music_list] area song %d: seq not found\n", song); return -1; }
    if (gctx(core)->native_music.play(mArea, so, AREA_VAB_OFF)) { fprintf(stderr, "[music_list] area song %d: play failed\n", song); return -1; }
    mNow = song;
    fprintf(stderr, "[music_list] area BGM song %d (seq@0x%lx vab@0x%lx)\n", song, so, (long)AREA_VAB_OFF);
    return 0;
}

void MusicList::stop() {
    if (core) gctx(core)->native_music.stop();
    mNow = -1;
}

int MusicList::nowPlaying() {
    if (mNow >= 0 && core && !gctx(core)->native_music.active()) mNow = -1;
    return mNow;
}
