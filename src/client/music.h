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

/*Procedural soundtrack: a small polyphonic synthesizer (subtractive,
 * FM and Karplus-Strong voices, drums, reverb) driven by per-track
 * composition routines. Runs entirely on the audio thread.*/
#ifndef MUSIC_H
#define MUSIC_H

void music_init(int sample_rate);
/*Switch track immediately (the caller handles cross-fades).*/
void music_set_track(int track);
int music_track(void);
void music_set_intensity(float v);
/*Mix `frames` stereo frames INTO out (interleaved L/R).*/
void music_render(float *out, int frames);

/*Offline helpers reused by the sound-effect generator*/
float synth_fm_bell(float t, float freq, float ratio, float index, float decay);

#endif
