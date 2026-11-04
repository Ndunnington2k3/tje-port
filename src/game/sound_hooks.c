/*
 * The game's music engine, watched for the native sound (core/music.h): the songs
 * it starts, whether it runs (a pause stops it), and, with the native sound, the
 * drum sounds it plays through the Z80 left out (they are in the recordings).
 */
#include "game.h"
#include "core/music.h"
#include "core/sound.h"

/* (the recorder: only this song may start, -1 any; told of the others asked for) */
int g_music_record_song = -1;
void (*g_music_record_held)(int song);

#define SONG_ALT 0xFFE476u   /* word: nonzero = the second set of songs (+7) */

static int song_allowed(int song)
{
    if (g_music_record_song < 0 || song == g_music_record_song) return 1;
    if (g_music_record_held) g_music_record_held(song);
    return 0;
}

/* LOC_0003E3FA: start song n (a long argument; +7 with SONG_ALT set) */
TJE_OVERRIDE(0x03E3FA)
void f_03E3FA(void)
{
    int song = (int)(uint16_t)(ARG_L(0) + (rd16(SONG_ALT) ? 7u : 0u));
    if (!song_allowed(song)) RETURN();
    orig_f_03E3FA();
    music_song(song);
}

/* LOC_0003E40E: the same, without the +7 (the song ending and starting again) */
TJE_OVERRIDE(0x03E40E)
void f_03E40E(void)
{
    int song = (int)(uint16_t)ARG_L(0);
    if (!song_allowed(song)) RETURN();
    orig_f_03E40E();
    music_song(song);
}

/* LOC_0003E3A8: every channel's song data cleared (the music stopped) */
TJE_OVERRIDE(0x03E3A8)
void f_03E3A8(void)
{
    orig_f_03E3A8();
    music_song(-1);
}

/* the music engine, once a frame while the music plays */
TJE_OVERRIDE(0x03ECFE)
void f_03ECFE_MassiveRoutine(void)
{
    music_tick();
    orig_f_03ECFE_MassiveRoutine();
}

/* LOC_0003F656: a drum sound of the song (Z80 command 2), from the engine. With the
 * native sound the recording has it: not played. (It only writes the Z80's
 * mailbox; the game's own memory is untouched either way.) */
TJE_OVERRIDE(0x03F656)
void f_03F656(void)
{
    if (snd_native()) RETURN();
    orig_f_03F656();
}
