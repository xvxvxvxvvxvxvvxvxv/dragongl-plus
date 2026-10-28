/*
 * DRAGON GL - 3D ARCANE ENGINE
 * Copyright (C) 2026 Nicola Taibi
 * License: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*Audio: a software synthesizer renders the whole soundtrack and every
 * sound effect in real time (no audio files are shipped). Output goes
 * through miniaudio (PulseAudio/ALSA/JACK on Linux, WASAPI on Windows),
 * loaded dynamically: without a sound device the game simply runs mute.*/
#ifndef AUDIO_H
#define AUDIO_H

#include <stdbool.h>

enum {
    MUSIC_NONE = 0,
    MUSIC_TITLE,     /*"The Long Descent" - main theme*/
    MUSIC_TOWN,      /*"Hearth of the Maw" - village, floor 0*/
    MUSIC_DUNGEON,   /*"Echoes Below" - upper floors*/
    MUSIC_DEEP,      /*"The Abyssal Choir" - floors 40+*/
    MUSIC_BOSS,      /*"Guardian of the Seal" - boss floors*/
    MUSIC_DEATH,     /*short lament*/
    MUSIC_COUNT
};

enum {
    SFX_CLICK = 0, SFX_HOVER, SFX_STEP, SFX_HIT, SFX_CRIT, SFX_MISS, SFX_HURT,
    SFX_KILL, SFX_COIN, SFX_LEVELUP, SFX_SPELL, SFX_HEAL, SFX_STAIRS, SFX_PICKUP,
    SFX_DEATH, SFX_ERROR, SFX_OPEN, SFX_CLOSE, SFX_EQUIP, SFX_EXPLOSION,
    SFX_MESSAGE, SFX_BOSS, SFX_COUNT
};

bool audio_init(void);
void audio_shutdown(void);
bool audio_available(void);
void audio_set_volumes(float master, float music, float sfx);
void audio_set_paused(bool paused);  /*e.g. window unfocused*/
/*Cross-fades to the track (no-op if already playing it).*/
void audio_music(int track);
/*0..1 combat intensity: adds the percussion layer to the music.*/
void audio_set_intensity(float v);
/*pan: -1 left .. 1 right; pitch: 1 = normal*/
void audio_sfx(int id, float volume, float pan);
void audio_sfx_pitch(int id, float volume, float pan, float pitch);
const char *audio_track_name(int track);

#endif
