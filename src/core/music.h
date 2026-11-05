/*
 * Recorded music for the native sound (sound.h, SND_FM_NATIVE).
 *
 * The game's music engine (MassiveRoutine and the song table at $3D626) still
 * runs; when it starts a song, a WAV file plays instead of the FM chip.
 * music/tracks.txt says which file and from where (the soundtrack's tracks, as
 * tools/vgm2wav renders its VGM rips: a level theme and its second part, song n
 * and n+7, in one file with one loop), and how each file loops; a song the game
 * asks for at about the place its file has reached plays on - the engine starting
 * a theme's next part or the theme again as the file goes there by itself.
 * Without a table: music/NN.wav for song NN (00-14), one pass of it, or looping
 * as its "smpl" chunk or a file NN.loop says (two numbers: the loop's first and
 * last sample frames, the last excluded). A WAV may be any rate, 16-bit, mono or
 * stereo. The title's song waits for the game when the game runs behind it (the
 * intro's sounds are in it). tje_headless --record-music DIR records the songs
 * from the Genesis sound one by one (the old way; sfx/ still comes from it).
 *
 * music_song / music_tick come from the game (its thread); music_mix may be called
 * from another thread (the platform's audio thread): only it touches the files.
 */
#ifndef TJE_MUSIC_H
#define TJE_MUSIC_H
#include <stdint.h>

void music_set_dir(const char *dir);       /* the folder holding NN.wav (default: "music/") */
void music_song(int song);                 /* the game started song `song` (-1: none) */
int music_current(void);                   /* the song last started (-1 none) */
void music_sfx(int id);                    /* the engine started its FM sound effect id (sfx/NN.wav) */
void music_tick(void);                     /* the music engine ran this frame */
void music_frame(void);                    /* (once a frame: no engine for a while = paused) */
void music_mix(int16_t *out, int frames);  /* add the music to 48 kHz stereo samples */
/* The files are read ahead by music_fill: by music_mix itself, or - music_threaded
 * set - by a thread of the platform's calling it now and then (returns 0 when there
 * was nothing to do). */
extern int music_threaded;
int music_fill(void);

/* (the recorder: told of each song started) */
extern void (*music_song_hook)(int song);

#endif
