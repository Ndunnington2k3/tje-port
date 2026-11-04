/*
 * Recorded music for the native sound (sound.h, SND_FM_NATIVE).
 *
 * The game's music engine (MassiveRoutine and the song table at $3D626) still
 * runs; when it starts a song, the recording of that song plays instead of the FM
 * chip: music/NN.wav, NN the song's number in the table (two digits, 00-14). The
 * engine plays each song once and starts the next itself (the same again, or an
 * intro's main part, song+7), so a recording is one pass of its song. A WAV file
 * may be any rate, 16-bit, mono or stereo; one of a different length (a remix) may
 * loop instead, from the loop of its "smpl" chunk or of a file NN.loop next to it
 * (two numbers: the loop's first and last sample frames, the last excluded), and
 * then plays until the engine starts another song. tje_headless --record-music DIR
 * makes the recordings from the Genesis sound.
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
void music_tick(void);                     /* the music engine ran this frame */
void music_frame(void);                    /* (once a frame: no engine for a while = paused) */
void music_mix(int16_t *out, int frames);  /* add the music to 48 kHz stereo samples */

/* (the recorder: told of each song started) */
extern void (*music_song_hook)(int song);

#endif
