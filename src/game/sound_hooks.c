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
void (*g_music_record_sfx_hook)(int id);

#define SONG_ALT 0xFFE476u   /* word: nonzero = the second set of songs (+7) */

static int song_allowed(int song)
{
    if (g_music_record_song < 0 || song == g_music_record_song) return 1;
    if (g_music_record_held) g_music_record_held(song);
    return 0;
}

/* LOC_0003E3FA: start song n (a long argument; +7 with SONG_ALT set) */
static int starting;   /* (inside a song start: its own loading of the defaults is not one) */

TJE_OVERRIDE(0x03E3FA)
void f_03E3FA(void)
{
    int song = (int)(uint16_t)(ARG_L(0) + (rd16(SONG_ALT) ? 7u : 0u));
    if (!song_allowed(song)) RETURN();
    starting++;
    orig_f_03E3FA();
    starting--;
    music_song(song);
}

/* LOC_0003E40E: the same, without the +7 (the song ending and starting again) */
TJE_OVERRIDE(0x03E40E)
void f_03E40E(void)
{
    int song = (int)(uint16_t)ARG_L(0);
    if (!song_allowed(song)) RETURN();
    starting++;
    orig_f_03E40E();
    starting--;
    music_song(song);
}

/* RAMWritesRoutine1: the engine's defaults loaded - at power on, song 0's channels
 * among them (the title music: no song is asked for) */
TJE_OVERRIDE(0x03E384)
void f_03E384_RAMWritesRoutine1(void)
{
    orig_f_03E384_RAMWritesRoutine1();
    if (!starting && song_allowed(0)) music_song(0);
}

/* LOC_0003E494: the engine's FM sound effect ($E47C) started, on channels taken
 * from the music for it. With the native sound: its recording, sfx/NN.wav. */
#define SFX_REQUEST 0xFFE47Cu
TJE_OVERRIDE(0x03E494)
void f_03E494(void)
{
    int id = (int16_t)rd16(SFX_REQUEST);
    orig_f_03E494();
    if (id >= 0) {
        if (id > 0 && g_music_record_sfx_hook) g_music_record_sfx_hook(id);
        if (snd_native()) music_sfx(id);
    }
}

/* LOC_0003E3A8: every channel's song data cleared (the music stopped) */
TJE_OVERRIDE(0x03E3A8)
void f_03E3A8(void)
{
    orig_f_03E3A8();
    music_song(-1);
    music_sfx(0);   /* (the effect's channels are cleared too) */
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

/* A save state or snapshot loaded (a saved game, network play joining): the song
 * the game is playing, from its start unless it is the one playing already. */
#define FLAG_8 0xFF802Du
static void restored(void)
{
    int song = (int)rd8(FLAG_8);
    if (song >= 0x80) song = -1;
    else if (rd16(SONG_ALT)) song += 7;
    if (song != music_current()) music_song(song);
}

__attribute__((constructor)) static void sound_hooks_init(void) { snd_restored_hook = restored; }
