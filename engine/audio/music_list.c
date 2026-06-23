// engine/audio/music_list.c — see music_list.h.
//
// Loads scratch/bin/snd/TOMBA2.SND once and exposes the 10 BGM songs (the §6 S2SV table) as
// playable tracks. Each song resolves to (seqOff, vabOff): seqOff by linear 'pQES' scan to the
// song's scan-index, vabOff from the container's VAB offset table. native_music_play() drives the
// shared synth from those offsets.
#include "music_list.h"
#include "native_music.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static uint8_t* s_buf;     // TOMBA2.SND contents
static long     s_size;
static int      s_now = -1;

// The SND container is read from the GAME DISC at runtime (NOT a pre-extracted file — that path is
// machine-local/gitignored and absent on a fresh checkout, which silenced the whole sound test). Uses
// the port's native ISO9660 reader, the same path the game itself loads \CD\TOMBA2.SND through.
int disc_read_sector(uint32_t lba, uint8_t* out);                              // runtime/recomp/disc.c
int disc_find_file(const char* path, uint32_t* out_lba, uint32_t* out_size);
static int load_container(void) {
    if (s_buf) return 0;
    uint32_t lba = 0, size = 0;
    if (!disc_find_file("\\CD\\TOMBA2.SND", &lba, &size) || size == 0) {
        fprintf(stderr, "[music_list] disc_find_file \\CD\\TOMBA2.SND failed\n"); return -1;
    }
    uint32_t nsec = (size + 2047) / 2048;
    s_buf = (uint8_t*)malloc((size_t)nsec * 2048);
    if (!s_buf) return -1;
    for (uint32_t i = 0; i < nsec; i++) {
        if (!disc_read_sector(lba + i, s_buf + (size_t)i * 2048)) {
            fprintf(stderr, "[music_list] disc read failed at sector %u\n", i);
            free(s_buf); s_buf = NULL; return -1;
        }
    }
    s_size = (long)size;
    fprintf(stderr, "[music_list] loaded \\CD\\TOMBA2.SND from disc (lba=%u, %ld bytes)\n", lba, s_size);
    return 0;
}

// byte offset of the si'th 'pQES' (linear scan from 0x30), or -1.
static long seq_off(int si) {
    long o = 0x30; int idx = 0;
    while (o < 0x51800 && o + 4 <= s_size) {
        if (!memcmp(s_buf + o, "pQES", 4)) { if (idx == si) return o; idx++; o += 4; }
        else o++;
    }
    return -1;
}

static long vab_off(int vi) { return (long)(s_buf[vi*2] | (s_buf[vi*2+1] << 8)) * 0x800; }

int music_list_count(void) { return 10; }

const char* music_list_name(int i) {
    if (i < 0 || i >= 10) return NULL;
    return S2SV[i].name;
}

int music_list_play(int i) {
    if (i < 0 || i >= 10) return -1;
    if (load_container()) return -1;
    long so = seq_off(S2SV[i].seq), vo = vab_off(S2SV[i].vab);
    if (so < 0) { fprintf(stderr, "[music_list] song %d: seq not found\n", i); return -1; }
    if (native_music_play(s_buf, so, vo)) { fprintf(stderr, "[music_list] song %d: play failed\n", i); return -1; }
    s_now = i;
    fprintf(stderr, "[music_list] playing song %d (seq@0x%lx vab@0x%lx)\n", i, so, vo);
    return 0;
}

// ---- in-game (live area bundle) ------------------------------------------------------------
// The area bundle's field instrument VAB sits at bundle offset 0x26b4 (ps=4); the 10 SEPs are
// concatenated from 0x30 (same layout as TOMBA2.SND). See scratch/handoff_audio_unify.md.
#define AREA_VAB_OFF 0x26b4

static uint8_t* s_area;       // engine-owned copy of the live area bundle
static long     s_area_len;

// byte offset of the si'th 'pQES' within the area bundle (linear scan from 0x30), or -1.
static long area_seq_off(int si) {
    long o = 0x30; int idx = 0;
    while (o + 4 <= s_area_len && o < AREA_VAB_OFF) {
        if (!memcmp(s_area + o, "pQES", 4)) { if (idx == si) return o; idx++; o += 4; }
        else o++;
    }
    return -1;
}

int music_list_play_area(const uint8_t* bundle, long bundle_len, int song) {
    if (!bundle || bundle_len < 0x4000 || song < 0 || song >= 10) return -1;
    // Validate this really is the bundle (SEP at 0x30, area VAB at 0x26b4) before committing.
    if (memcmp(bundle + 0x30, "pQES", 4) || memcmp(bundle + AREA_VAB_OFF, "pBAV", 4)) {
        fprintf(stderr, "[music_list] area bundle invalid (no pQES@0x30 / pBAV@0x26b4)\n");
        return -1;
    }
    // Copy into an engine-owned buffer so playback survives area data churn / overwrite.
    long need = bundle_len; if (need > 0x50000) need = 0x50000;
    uint8_t* nb = (uint8_t*)malloc(need);
    if (!nb) return -1;
    memcpy(nb, bundle, need);
    native_music_stop();
    free(s_area); s_area = nb; s_area_len = need;
    long so = area_seq_off(song);
    if (so < 0) { fprintf(stderr, "[music_list] area song %d: seq not found\n", song); return -1; }
    if (native_music_play(s_area, so, AREA_VAB_OFF)) { fprintf(stderr, "[music_list] area song %d: play failed\n", song); return -1; }
    s_now = song;
    fprintf(stderr, "[music_list] area BGM song %d (seq@0x%lx vab@0x%lx)\n", song, so, (long)AREA_VAB_OFF);
    return 0;
}

void music_list_stop(void) { native_music_stop(); s_now = -1; }

int music_list_now_playing(void) {
    if (s_now >= 0 && !native_music_active()) s_now = -1;
    return s_now;
}
