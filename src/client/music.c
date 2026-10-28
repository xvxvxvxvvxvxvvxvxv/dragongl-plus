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

/*The procedural soundtrack of Dragon GL.
 *
 * Synthesizer: 40 voices of subtractive (PolyBLEP saw), FM, sine and
 * Karplus-Strong instruments plus synthesized drums, a stereo
 * Schroeder/Freeverb-style reverb and a soft limiter.
 *
 * Composition: each track is a step routine called on every 16th note.
 * The themes are written out (melodies as scale degrees, chord
 * progressions); accompaniment and the ambient layers are generated from
 * deterministic per-step hashes, so a loop never sounds mechanically the
 * same while staying in key. Every track has a "combat" layer whose
 * volume follows the intensity set by the game.
 *
 *   1 The Long Descent      D minor, 72 BPM   - main theme (menus)
 *   2 Hearth of the Maw     G major, 104 BPM  - the village
 *   3 Echoes Below          A phrygian, 64 BPM - upper dungeon
 *   4 The Abyssal Choir     C# minor, 52 BPM  - the deep floors
 *   5 Guardian of the Seal  E minor, 138 BPM  - boss floors
 *   6 Lament                D minor, 50 BPM   - death*/

#include "music.h"
#include "audio.h"
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdbool.h>

#define PI_F 3.14159265f
#define TAU_F 6.28318531f
#define MAX_VOICES 40
#define KS_MAX 4096

static float SR = 44100.0f;

/* ------------------------------------------------------------------ */
/* Instruments                                                         */
/* ------------------------------------------------------------------ */

enum {
    I_PAD, I_STRINGS, I_PLUCK, I_KALIMBA, I_BELL, I_FLUTE, I_HORN, I_BASS,
    I_CHOIR, I_SUB, I_KICK, I_SNARE, I_HAT, I_TOM, I_TIMPANI, I_TAMB, I_CRASH,
    I_COUNT
};

typedef struct {
    float a, d, s, r;  /*envelope (seconds, sustain level)*/
    float gain;
    float rev;         /*reverb send*/
    float pan;
} InstDef;

static const InstDef INST[I_COUNT] = {
    [I_PAD]     = {0.9f, 0.6f, 0.85f, 1.8f, 0.16f, 0.55f, 0.0f},
    [I_STRINGS] = {0.25f, 0.3f, 0.85f, 0.9f, 0.16f, 0.45f, -0.2f},
    [I_PLUCK]   = {0.002f, 0.1f, 1.0f, 0.4f, 0.42f, 0.35f, 0.25f},
    [I_KALIMBA] = {0.002f, 0.2f, 0.0f, 0.5f, 0.32f, 0.55f, 0.3f},
    [I_BELL]    = {0.002f, 0.1f, 1.0f, 1.2f, 0.2f, 0.65f, -0.3f},
    [I_FLUTE]   = {0.07f, 0.2f, 0.85f, 0.25f, 0.28f, 0.35f, 0.1f},
    [I_HORN]    = {0.06f, 0.3f, 0.8f, 0.35f, 0.24f, 0.3f, -0.05f},
    [I_BASS]    = {0.01f, 0.25f, 0.75f, 0.2f, 0.42f, 0.08f, 0.0f},
    [I_CHOIR]   = {0.8f, 0.5f, 0.9f, 1.6f, 0.22f, 0.6f, 0.0f},
    [I_SUB]     = {1.5f, 0.5f, 1.0f, 2.0f, 0.38f, 0.1f, 0.0f},
    [I_KICK]    = {0.001f, 0.0f, 1.0f, 0.05f, 0.85f, 0.04f, 0.0f},
    [I_SNARE]   = {0.001f, 0.0f, 1.0f, 0.05f, 0.42f, 0.14f, 0.05f},
    [I_HAT]     = {0.001f, 0.0f, 1.0f, 0.03f, 0.18f, 0.08f, 0.3f},
    [I_TOM]     = {0.001f, 0.0f, 1.0f, 0.05f, 0.55f, 0.18f, -0.2f},
    [I_TIMPANI] = {0.001f, 0.0f, 1.0f, 0.3f, 0.7f, 0.35f, 0.0f},
    [I_TAMB]    = {0.001f, 0.0f, 1.0f, 0.05f, 0.16f, 0.2f, 0.35f},
    [I_CRASH]   = {0.001f, 0.0f, 1.0f, 0.3f, 0.2f, 0.4f, -0.3f},
};

typedef struct {
    bool active;
    int inst;
    int layer;          /*0 = base, 1 = combat layer*/
    float freq, vel, pan;
    float t;            /*seconds since note on*/
    float dur;          /*seconds before release*/
    float env;
    int stage;          /*0 attack 1 decay 2 sustain 3 release*/
    double ph[4];
    float lp1, lp2, lp3;
    float bp1_l, bp1_b, bp2_l, bp2_b;
    uint32_t rng;
    float ks[KS_MAX];
    int ks_len, ks_pos;
    uint32_t age;
} Voice;

static Voice V[MAX_VOICES];
static uint32_t g_age;

static inline float frandu(uint32_t *s) {
    *s ^= *s << 13; *s ^= *s >> 17; *s ^= *s << 5;
    return (float)(*s & 0xFFFFFF) / 8388607.5f - 1.0f;
}

static inline float clampf_local(float v) { return v < -1 ? -1 : (v > 1 ? 1 : v); }
static inline float mtof(float m) { return 440.0f * powf(2.0f, (m - 69.0f) / 12.0f); }

/*PolyBLEP saw (band-limited enough for this purpose)*/
static inline float polyblep(double t, double dt) {
    if (t < dt) { t /= dt; return (float)(t + t - t * t - 1.0); }
    if (t > 1.0 - dt) { t = (t - 1.0) / dt; return (float)(t * t + t + t + 1.0); }
    return 0.0f;
}
static inline float saw(double *ph, float f) {
    double dt = f / SR;
    float v = (float)(2.0 * *ph - 1.0) - polyblep(*ph, dt);
    *ph += dt;
    if (*ph >= 1.0) *ph -= 1.0;
    return v;
}
static inline float sine(double *ph, float f) {
    float v = sinf((float)(*ph * TAU_F));
    *ph += f / SR;
    if (*ph >= 1.0) *ph -= 1.0;
    return v;
}
static inline float lp_coef(float fc) {
    if (fc > SR * 0.45f) fc = SR * 0.45f;
    return 1.0f - expf(-TAU_F * fc / SR);
}

float synth_fm_bell(float t, float freq, float ratio, float index, float decay) {
    float I = index * expf(-t * 3.0f);
    return sinf(TAU_F * freq * t + I * sinf(TAU_F * freq * ratio * t)) * expf(-t * decay);
}

static void note_on(int inst, float midi, float vel, float dur_s, float pan, int layer) {
    Voice *v = NULL;
    for (int i = 0; i < MAX_VOICES; i++)
        if (!V[i].active) { v = &V[i]; break; }
    if (!v) {
        /*steal the oldest voice*/
        uint32_t best = UINT32_MAX;
        for (int i = 0; i < MAX_VOICES; i++)
            if (V[i].age < best) { best = V[i].age; v = &V[i]; }
    }
    memset(v, 0, offsetof(Voice, ks));
    v->active = true;
    v->inst = inst;
    v->layer = layer;
    v->freq = mtof(midi);
    v->vel = vel;
    v->pan = clampf_local(INST[inst].pan + pan);
    v->dur = dur_s;
    v->rng = 0x9E3779B9u ^ (uint32_t)(midi * 977.0f) ^ (++g_age * 2654435761u);
    v->age = g_age;
    if (inst == I_PLUCK) {
        v->ks_len = (int)(SR / v->freq);
        if (v->ks_len < 2) v->ks_len = 2;
        if (v->ks_len > KS_MAX) v->ks_len = KS_MAX;
        float prev = 0;
        for (int i = 0; i < v->ks_len; i++) {
            float n = frandu(&v->rng);
            prev = prev + 0.55f * (n - prev); /*slightly darker excitation*/
            v->ks[i] = prev;
        }
        v->ks_pos = 0;
    }
}

/* ------------------------------------------------------------------ */
/* Voice rendering                                                     */
/* ------------------------------------------------------------------ */

static float envelope(Voice *v, float dt) {
    const InstDef *d = &INST[v->inst];
    if (v->stage < 3 && v->t >= v->dur) v->stage = 3;
    switch (v->stage) {
    case 0:
        v->env += dt / fmaxf(d->a, 1e-4f);
        if (v->env >= 1.0f) { v->env = 1.0f; v->stage = 1; }
        break;
    case 1:
        v->env = d->s + (v->env - d->s) * expf(-dt / fmaxf(d->d * 0.3f, 1e-4f));
        if (fabsf(v->env - d->s) < 1e-3f) v->stage = 2;
        break;
    case 2:
        v->env = d->s;
        break;
    default:
        v->env *= expf(-dt / fmaxf(d->r * 0.25f, 1e-4f));
        if (v->env < 1e-4f) v->active = false;
        break;
    }
    return v->env;
}

static inline float svf_bp(float *low, float *band, float x, float fc, float q) {
    float f = 2.0f * sinf(PI_F * fc / SR);
    float high = x - *low - q * *band;
    *band += f * high;
    *low += f * *band;
    return *band;
}

static inline float smoothstep_local(float a, float b, float x) {
    float t = (x - a) / (b - a);
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    return t * t * (3 - 2 * t);
}

static float voice_sample(Voice *v) {
    float dt = 1.0f / SR;
    float t = v->t;
    float f = v->freq;
    float out = 0;
    float env = 1.0f;
    switch (v->inst) {
    case I_PAD: {
        env = envelope(v, dt);
        float lfo = sinf(TAU_F * 0.11f * t + v->freq * 0.01f);
        float x = saw(&v->ph[0], f * 0.996f) + saw(&v->ph[1], f) + saw(&v->ph[2], f * 1.004f);
        x = x * 0.33f + sine(&v->ph[3], f * 0.5f) * 0.35f;
        float fc = 500.0f + 450.0f * lfo + 500.0f * env;
        float a = lp_coef(fc);
        v->lp1 += a * (x - v->lp1);
        v->lp2 += a * (v->lp1 - v->lp2);
        out = v->lp2;
        break;
    }
    case I_STRINGS: {
        env = envelope(v, dt);
        float vib = 1.0f + 0.004f * sinf(TAU_F * 5.2f * t) * fminf(1.0f, t * 2.0f);
        float x = saw(&v->ph[0], f * vib * 0.997f) + saw(&v->ph[1], f * vib * 1.003f);
        float a = lp_coef(1800.0f + 800.0f * env);
        v->lp1 += a * (x * 0.5f - v->lp1);
        v->lp2 += a * (v->lp1 - v->lp2);
        out = v->lp2;
        break;
    }
    case I_PLUCK: {
        if (v->stage != 3 && t >= v->dur) { v->stage = 3; v->env = 1.0f; }
        if (v->stage == 3) {
            v->env *= expf(-dt / 0.08f);
            if (v->env < 1e-4f) v->active = false;
            env = v->env;
        }
        int i0 = v->ks_pos, i1 = (v->ks_pos + 1) % v->ks_len;
        float y = v->ks[i0];
        v->ks[i0] = 0.4985f * (v->ks[i0] + v->ks[i1]);
        v->ks_pos = i1;
        out = y * 1.4f;
        if (t > 6.0f) v->active = false;
        break;
    }
    case I_KALIMBA: {
        env = envelope(v, dt);
        float d1 = expf(-t * 3.0f), d2 = expf(-t * 14.0f);
        out = sine(&v->ph[0], f) * d1 + sine(&v->ph[1], f * 5.4f) * d2 * 0.35f;
        if (t > 3.0f) v->active = false;
        break;
    }
    case I_BELL: {
        env = envelope(v, dt);
        out = synth_fm_bell(t, f, 3.5f, 2.2f, 1.1f) * 0.8f + synth_fm_bell(t, f * 2.01f, 1.0f, 0.5f, 2.5f) * 0.2f;
        if (t > 6.0f) v->active = false;
        break;
    }
    case I_FLUTE: {
        env = envelope(v, dt);
        float vib = 1.0f + 0.006f * sinf(TAU_F * 5.0f * t) * smoothstep_local(0.25f, 0.7f, t);
        float x = sine(&v->ph[0], f * vib) + 0.18f * sine(&v->ph[1], f * 2.0f * vib) + 0.05f * sine(&v->ph[2], f * 3.0f * vib);
        float n = frandu(&v->rng);
        v->lp1 += lp_coef(2500.0f) * (n - v->lp1);
        out = x * 0.8f + v->lp1 * 0.12f * (0.4f + env);
        break;
    }
    case I_HORN: {
        env = envelope(v, dt);
        float x = saw(&v->ph[0], f) * 0.7f + saw(&v->ph[1], f * 1.002f) * 0.3f;
        float a = lp_coef(250.0f + 2600.0f * env * env);
        v->lp1 += a * (x - v->lp1);
        v->lp2 += a * (v->lp1 - v->lp2);
        out = v->lp2;
        break;
    }
    case I_BASS: {
        env = envelope(v, dt);
        float x = saw(&v->ph[0], f) * 0.6f + sine(&v->ph[1], f) * 0.6f;
        float a = lp_coef(180.0f + 900.0f * env * env);
        v->lp1 += a * (x - v->lp1);
        v->lp2 += a * (v->lp1 - v->lp2);
        out = v->lp2;
        break;
    }
    case I_CHOIR: {
        env = envelope(v, dt);
        float vib = 1.0f + 0.005f * sinf(TAU_F * 4.6f * t + f);
        float x = saw(&v->ph[0], f * vib * 0.995f) + saw(&v->ph[1], f * vib) + saw(&v->ph[2], f * vib * 1.005f);
        x *= 0.33f;
        float y = svf_bp(&v->bp1_l, &v->bp1_b, x, 720.0f, 0.25f) * 0.9f + svf_bp(&v->bp2_l, &v->bp2_b, x, 1150.0f, 0.3f) * 0.5f;
        float a = lp_coef(3000.0f);
        v->lp1 += a * (y - v->lp1);
        out = v->lp1 * 1.4f;
        break;
    }
    case I_SUB:
        env = envelope(v, dt);
        out = sine(&v->ph[0], f) * 0.8f + sine(&v->ph[1], f * 2.0f) * 0.15f;
        break;
    case I_KICK: {
        float fr = 45.0f + 90.0f * expf(-t * 28.0f);
        out = sine(&v->ph[0], fr) * expf(-t * 7.0f) + frandu(&v->rng) * expf(-t * 300.0f) * 0.3f;
        if (t > 0.6f) v->active = false;
        break;
    }
    case I_SNARE: {
        float n = frandu(&v->rng);
        v->lp1 += lp_coef(5000.0f) * (n - v->lp1);
        out = (n - v->lp1 * 0.5f) * expf(-t * 16.0f) * 0.7f + sine(&v->ph[0], 185.0f) * expf(-t * 22.0f) * 0.5f;
        if (t > 0.5f) v->active = false;
        break;
    }
    case I_HAT: {
        float n = frandu(&v->rng);
        v->lp1 += lp_coef(7000.0f) * (n - v->lp1);
        out = (n - v->lp1) * expf(-t * 55.0f);
        if (t > 0.2f) v->active = false;
        break;
    }
    case I_TOM: {
        float fr = f * (1.0f + 0.6f * expf(-t * 18.0f));
        out = sine(&v->ph[0], fr) * expf(-t * 6.0f) + frandu(&v->rng) * expf(-t * 60.0f) * 0.2f;
        if (t > 0.8f) v->active = false;
        break;
    }
    case I_TIMPANI: {
        float fr = f * (1.0f + 0.08f * expf(-t * 10.0f));
        out = (sine(&v->ph[0], fr) + 0.35f * sine(&v->ph[1], fr * 1.5f) * expf(-t * 3.0f)) * expf(-t * 2.2f) +
              frandu(&v->rng) * expf(-t * 40.0f) * 0.25f;
        if (t > 3.0f) v->active = false;
        break;
    }
    case I_TAMB: {
        float n = frandu(&v->rng);
        float y = svf_bp(&v->bp1_l, &v->bp1_b, n, 7500.0f, 0.5f);
        out = y * expf(-t * 22.0f) * (0.7f + 0.3f * sinf(TAU_F * 60.0f * t));
        if (t > 0.35f) v->active = false;
        break;
    }
    case I_CRASH: {
        float n = frandu(&v->rng);
        v->lp1 += lp_coef(4000.0f) * (n - v->lp1);
        out = (n - v->lp1) * expf(-t * 1.6f);
        if (t > 3.5f) v->active = false;
        break;
    }
    default:
        v->active = false;
        break;
    }
    v->t += dt;
    return out * env * v->vel * INST[v->inst].gain;
}

/* ------------------------------------------------------------------ */
/* Reverb (Freeverb-style: 4 combs + 2 allpasses per channel)          */
/* ------------------------------------------------------------------ */

#define NCOMB 4
#define NAP 2
typedef struct {
    float *buf;
    int len, pos;
    float fb, damp, store;
} Comb;
typedef struct {
    float *buf;
    int len, pos;
} Allpass;

static Comb combs[2][NCOMB];
static Allpass aps[2][NAP];
static float revbuf[2 * (1600 + 1700 + 1800 + 1900 + 600 + 500) * 2];

static void reverb_init(void) {
    static const int CL[NCOMB] = {1116, 1188, 1277, 1356};
    static const int AL[NAP] = {556, 441};
    float scale = SR / 44100.0f;
    float *p = revbuf;
    for (int ch = 0; ch < 2; ch++) {
        for (int i = 0; i < NCOMB; i++) {
            combs[ch][i].len = (int)((float)(CL[i] + ch * 23) * scale);
            combs[ch][i].buf = p;
            p += combs[ch][i].len;
            combs[ch][i].pos = 0;
            combs[ch][i].fb = 0.86f;
            combs[ch][i].damp = 0.35f;
            combs[ch][i].store = 0;
        }
        for (int i = 0; i < NAP; i++) {
            aps[ch][i].len = (int)((float)(AL[i] + ch * 23) * scale);
            aps[ch][i].buf = p;
            p += aps[ch][i].len;
            aps[ch][i].pos = 0;
        }
    }
    memset(revbuf, 0, sizeof(revbuf));
}

static float reverb_ch(int ch, float in) {
    float out = 0;
    for (int i = 0; i < NCOMB; i++) {
        Comb *c = &combs[ch][i];
        float y = c->buf[c->pos];
        c->store = y * (1.0f - c->damp) + c->store * c->damp;
        c->buf[c->pos] = in + c->store * c->fb;
        if (++c->pos >= c->len) c->pos = 0;
        out += y;
    }
    for (int i = 0; i < NAP; i++) {
        Allpass *a = &aps[ch][i];
        float b = a->buf[a->pos];
        a->buf[a->pos] = out + b * 0.5f;
        out = b - out;
        if (++a->pos >= a->len) a->pos = 0;
    }
    return out * 0.25f;
}

/* ------------------------------------------------------------------ */
/* Composition helpers                                                 */
/* ------------------------------------------------------------------ */

static const int SC_MINOR[7] = {0, 2, 3, 5, 7, 8, 10};
static const int SC_MAJOR[7] = {0, 2, 4, 5, 7, 9, 11};
static const int SC_PHRYG[7] = {0, 1, 3, 5, 7, 8, 10};

static int g_track = MUSIC_NONE;
static double g_step_acc;
static long g_step;
static float g_bpm = 72;
static float g_intensity, g_intensity_s;

static float deg(int root, const int *sc, int d) {
    int o = (d >= 0) ? d / 7 : -((-d + 6) / 7);
    int i = d - o * 7;
    return (float)(root + o * 12 + sc[i]);
}

static float step_sec(void) { return 60.0f / g_bpm / 4.0f; }
static float steps(float n) { return n * step_sec(); }

static uint32_t h32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
    return x;
}
static float hrand(long step, int salt) { return (float)(h32((uint32_t)step * 2654435761u + (uint32_t)salt * 97u) & 0xFFFF) / 65535.0f; }

typedef struct { int d; int len; int acc; } MNote; /*scale degree (99 = rest), length in steps, semitone accidental*/

/*Melody playback: plays the note that STARTS at local step `ls`.*/
static void melody_at(const MNote *m, int n, int ls, int inst, int root, const int *sc, float vel, float pan, int layer) {
    int pos = 0;
    for (int i = 0; i < n; i++) {
        if (pos == ls) {
            if (m[i].d != 99) note_on(inst, deg(root, sc, m[i].d) + (float)m[i].acc, vel, steps((float)m[i].len) * 0.95f, pan, layer);
            return;
        }
        pos += m[i].len;
        if (pos > ls) return;
    }
}

typedef struct { int d; int third_acc; } ChordDef;

static void chord_notes(int root, const int *sc, ChordDef c, int octave_shift, float out[3]) {
    out[0] = deg(root, sc, c.d) + 12.0f * (float)octave_shift;
    out[1] = deg(root, sc, c.d + 2) + (float)c.third_acc + 12.0f * (float)octave_shift;
    out[2] = deg(root, sc, c.d + 4) + 12.0f * (float)octave_shift;
}

/* ------------------------------------------------------------------ */
/* 1. The Long Descent (title)                                          */
/* ------------------------------------------------------------------ */

static const ChordDef TITLE_CH[8] = {{0, 0}, {5, 0}, {2, 0}, {6, 0}, {0, 0}, {5, 0}, {3, 0}, {4, 1}};
static const MNote TITLE_THEME[] = {
    {4, 12, 0}, {3, 4, 0}, {2, 8, 0}, {3, 4, 0}, {4, 4, 0}, {5, 12, 0}, {4, 4, 0}, {6, 8, 0}, {4, 8, 0},
    {7, 8, 0}, {6, 4, 0}, {5, 4, 0}, {4, 8, 0}, {2, 8, 0}, {3, 4, 0}, {2, 4, 0}, {1, 4, 0}, {3, 4, 0}, {4, 16, 0},
};
static const MNote TITLE_CALL[] = {
    {7, 8, 0}, {4, 8, 0}, {5, 16, 0}, {4, 8, 0}, {2, 8, 0}, {1, 12, 0}, {99, 4, 0},
};

static void title_step(long s) {
    int bar = (int)(s / 16) % 16, pos = (int)(s % 16);
    ChordDef c = TITLE_CH[bar % 8];
    const int root = 50; /*D3*/
    float n[3];
    chord_notes(root, SC_MINOR, c, 0, n);
    if (pos == 0) {
        for (int i = 0; i < 3; i++) note_on(I_PAD, n[i] + (i == 0 ? 12.0f : 0.0f), 0.55f, steps(16), (float)(i - 1) * 0.3f, 0);
        note_on(I_BASS, n[0] - 12.0f, 0.7f, steps(14), 0, 0);
        if (bar >= 8) note_on(I_STRINGS, n[1] + 12.0f, 0.35f, steps(16), 0.3f, 0);
    }
    if (pos == 8 && (bar & 1)) note_on(I_BASS, n[2] - 12.0f, 0.55f, steps(7), 0, 0);
    /*harp arpeggio in 8ths*/
    if ((pos & 1) == 0) {
        static const int pat[8] = {0, 1, 2, 3, 4, 3, 2, 1};
        int k = pat[pos / 2];
        float note = k < 3 ? n[k] + 12.0f : n[k - 3] + 24.0f;
        note_on(I_PLUCK, note, 0.30f + 0.1f * (pos == 0), steps(6), ((float)k - 2.0f) * 0.25f, 0);
    }
    if (bar >= 4 && bar < 8) melody_at(TITLE_CALL, 7, (bar - 4) * 16 + pos, I_FLUTE, 62, SC_MINOR, 0.45f, 0.2f, 0);
    if (bar >= 8) melody_at(TITLE_THEME, 19, (bar - 8) * 16 + pos, I_HORN, 62, SC_MINOR, 0.55f, -0.1f, 0);
    if (pos == 0 && (bar % 4) == 0) note_on(I_TIMPANI, 38, 0.7f, steps(8), 0, 0);
    if (bar == 15 && pos >= 8) note_on(I_TIMPANI, 45, 0.25f + 0.05f * (float)(pos - 8), steps(1), 0, 0);
    /*combat layer: low drums*/
    if (pos == 0 || pos == 6 || pos == 10) note_on(I_TOM, 38, 0.6f, steps(2), -0.2f, 1);
}

/* ------------------------------------------------------------------ */
/* 2. Hearth of the Maw (village)                                       */
/* ------------------------------------------------------------------ */

static const ChordDef TOWN_CH[16] = {{0, 0}, {4, 0}, {5, 0}, {3, 0}, {0, 0}, {3, 0}, {4, 0}, {0, 0},
                                  {5, 0}, {3, 0}, {0, 0}, {4, 0}, {3, 0}, {4, 0}, {0, 0}, {0, 0}};
static const MNote TOWN_TUNE[] = {
    {4, 4, 0}, {4, 2, 0}, {5, 2, 0}, {4, 4, 0}, {2, 4, 0},
    {1, 4, 0}, {2, 4, 0}, {3, 4, 0}, {1, 4, 0},
    {2, 6, 0}, {3, 2, 0}, {4, 4, 0}, {2, 4, 0},
    {0, 8, 0}, {99, 8, 0},
    {4, 4, 0}, {7, 4, 0}, {6, 4, 0}, {5, 4, 0},
    {4, 4, 0}, {3, 4, 0}, {2, 4, 0}, {3, 4, 0},
    {4, 6, 0}, {3, 2, 0}, {1, 4, 0}, {2, 4, 0},
    {0, 12, 0}, {99, 4, 0},
    {2, 4, 0}, {2, 2, 0}, {3, 2, 0}, {4, 4, 0}, {5, 4, 0},
    {7, 4, 0}, {5, 4, 0}, {4, 8, 0},
    {4, 4, 0}, {2, 4, 0}, {0, 4, 0}, {2, 4, 0},
    {1, 12, 0}, {99, 4, 0},
    {2, 4, 0}, {3, 4, 0}, {4, 4, 0}, {5, 4, 0},
    {6, 8, 0}, {4, 8, 0},
    {7, 6, 0}, {6, 2, 0}, {5, 4, 0}, {4, 4, 0},
    {0, 12, 0}, {99, 4, 0},
};

static void town_step(long s) {
    int bar = (int)(s / 16) % 32, pos = (int)(s % 16);
    ChordDef c = TOWN_CH[bar % 16];
    const int root = 55; /*G3*/
    float n[3];
    chord_notes(root, SC_MAJOR, c, 0, n);
    if (pos == 0) {
        note_on(I_PAD, n[1] + 12.0f, 0.22f, steps(16), 0.2f, 0);
        note_on(I_PAD, n[2], 0.22f, steps(16), -0.2f, 0);
    }
    if (pos == 0) note_on(I_BASS, n[0] - 12.0f, 0.65f, steps(6), 0, 0);
    if (pos == 8) note_on(I_BASS, n[2] - 24.0f, 0.55f, steps(6), 0, 0);
    if ((pos & 1) == 0) {
        static const int pat[8] = {0, 2, 3, 1, 2, 3, 1, 2};
        int k = pat[pos / 2];
        float note = k == 3 ? n[0] + 12.0f : n[k];
        note_on(I_PLUCK, note + 12.0f, 0.28f + (pos == 0 ? 0.08f : 0.0f), steps(4), (k - 1.5f) * 0.3f, 0);
    }
    if (pos == 4 || pos == 12) note_on(I_TAMB, 80, 0.45f, steps(1), 0, 0);
    if ((pos & 1) == 0 && pos != 4 && pos != 12) note_on(I_HAT, 80, 0.12f + 0.06f * hrand(s, 3), steps(1), 0, 0);
    /*the tune plays on the second pass; first pass = accompaniment only*/
    if (bar >= 16) melody_at(TOWN_TUNE, (int)(sizeof(TOWN_TUNE) / sizeof(TOWN_TUNE[0])), (bar - 16) * 16 + pos, I_FLUTE, 67, SC_MAJOR, 0.5f, 0.1f, 0);
    else if ((pos % 4) == 2 && hrand(s, 9) < 0.18f) note_on(I_KALIMBA, deg(67, SC_MAJOR, (int)(hrand(s, 10) * 7)), 0.25f, steps(2), 0.4f, 0);
    if (pos == 0 || pos == 8) note_on(I_KICK, 36, 0.5f, steps(1), 0, 1);
    if (pos == 4 || pos == 12) note_on(I_SNARE, 40, 0.45f, steps(1), 0, 1);
}

/* ------------------------------------------------------------------ */
/* 3. Echoes Below (upper dungeon)                                      */
/* ------------------------------------------------------------------ */

static void dungeon_step(long s) {
    int bar = (int)(s / 16) % 16, pos = (int)(s % 16);
    const int root = 45; /*A2*/
    if (pos == 0 && (bar % 8) == 0) {
        note_on(I_SUB, 33, 0.6f, steps(124), 0, 0);
        note_on(I_PAD, 40, 0.30f, steps(120), -0.2f, 0);
    }
    if (pos == 0 && (bar % 4) == 0) {
        ChordDef c = (bar % 8) < 4 ? (ChordDef){0, 0} : (ChordDef){1, 0};
        float n[3];
        chord_notes(root, SC_PHRYG, c, 1, n);
        for (int i = 0; i < 3; i++) note_on(I_STRINGS, n[i], 0.16f, steps(56), (float)(i - 1) * 0.4f, 0);
    }
    /*bells / drips*/
    if ((pos & 1) == 0 && hrand(s, 1) < 0.15f) {
        int d = (int)(hrand(s, 2) * 10.0f);
        note_on(hrand(s, 4) < 0.5f ? I_BELL : I_KALIMBA, deg(69, SC_PHRYG, d), 0.18f + 0.22f * hrand(s, 5), steps(8),
                hrand(s, 6) * 1.6f - 0.8f, 0);
    }
    if (pos == 0 && (bar % 4) == 2) {
        note_on(I_KALIMBA, deg(57, SC_PHRYG, 0), 0.3f, steps(4), -0.3f, 0);
    }
    if ((bar % 4) == 2 && (pos == 4 || pos == 8 || pos == 12)) {
        static const int motif[3] = {2, 4, 3};
        note_on(I_KALIMBA, deg(57, SC_PHRYG, motif[pos / 4 - 1]), 0.3f, steps(4), -0.3f + 0.2f * (float)(pos / 4), 0);
    }
    /*combat layer: toms and an ostinato*/
    if (pos == 0 || pos == 3 || pos == 6 || pos == 10) note_on(I_TOM, pos == 0 ? 40 : 45, 0.7f, steps(2), 0, 1);
    if ((pos & 1) == 0) {
        static const int ost[8] = {0, 0, 1, 0, 0, 0, -1, 0};
        note_on(I_BASS, 33.0f + (float)ost[pos / 2], 0.6f, steps(1.6f), 0, 1);
    }
    if (pos % 2 == 1) note_on(I_HAT, 80, 0.10f, steps(1), 0, 1);
}

/* ------------------------------------------------------------------ */
/* 4. The Abyssal Choir (deep floors)                                   */
/* ------------------------------------------------------------------ */

static const ChordDef DEEP_CH[4] = {{0, 0}, {5, 0}, {3, 0}, {4, 1}};

static void deep_step(long s) {
    int bar = (int)(s / 16) % 16, pos = (int)(s % 16);
    const int root = 49; /*C#3*/
    if (pos == 0 && (bar % 8) == 0) note_on(I_SUB, 25, 0.7f, steps(124), 0, 0);
    if (pos == 0 && (bar % 2) == 0) {
        ChordDef c = DEEP_CH[(bar / 2) % 4];
        float n[3];
        chord_notes(root, SC_MINOR, c, 0, n);
        for (int i = 0; i < 3; i++) note_on(I_CHOIR, n[i] + (i == 0 ? 12.0f : 0.0f), 0.42f, steps(30), (float)(i - 1) * 0.45f, 0);
        note_on(I_BELL, n[0] - 12.0f, 0.45f, steps(16), 0, 0);
    }
    if ((pos & 1) == 0 && hrand(s, 11) < 0.07f) {
        int d = (int)(hrand(s, 12) * 9.0f);
        int acc = hrand(s, 13) < 0.25f ? 1 : 0; /*a touch of dissonance*/
        note_on(I_BELL, deg(73, SC_MINOR, d) + (float)acc, 0.15f + 0.15f * hrand(s, 14), steps(10), hrand(s, 15) * 1.6f - 0.8f, 0);
    }
    /*combat: heartbeat + tremolo*/
    if (pos == 0 || pos == 3) note_on(I_KICK, 30, pos == 0 ? 0.8f : 0.55f, steps(1), 0, 1);
    if ((pos & 1) == 0) note_on(I_STRINGS, 37.0f + (bar % 2 ? 1.0f : 0.0f), 0.25f, steps(1.5f), 0.3f, 1);
}

/* ------------------------------------------------------------------ */
/* 5. Guardian of the Seal (boss)                                       */
/* ------------------------------------------------------------------ */

static const ChordDef BOSS_CH[8] = {{0, 0}, {5, 0}, {6, 0}, {0, 0}, {0, 0}, {5, 0}, {4, 1}, {4, 1}};
static const MNote BOSS_THEME[] = {
    {0, 6, 0}, {2, 2, 0}, {4, 8, 0}, {5, 6, 0}, {4, 2, 0}, {2, 8, 0}, {3, 4, 0}, {4, 4, 0}, {5, 4, 0}, {6, 4, 0},
    {4, 16, 0}, {7, 6, 0}, {6, 2, 0}, {4, 8, 0}, {5, 4, 0}, {4, 4, 0}, {2, 4, 0}, {4, 4, 0}, {1, 6, 0}, {2, 2, 0},
    {3, 8, 0}, {1, 16, 0},
};

static void boss_step(long s) {
    int bar = (int)(s / 16) % 16, pos = (int)(s % 16);
    ChordDef c = BOSS_CH[bar % 8];
    const int root = 40; /*E2*/
    float n[3];
    chord_notes(root, SC_MINOR, c, 0, n);
    if ((pos & 1) == 0) {
        static const int ost[8] = {0, 0, 12, 0, 0, 12, 0, 10};
        note_on(I_BASS, n[0] + (float)ost[pos / 2], 0.75f, steps(1.7f), 0, 0);
    }
    if (pos == 0 || pos == 6 || pos == 12) {
        note_on(I_HORN, n[0] + 12.0f, 0.42f, steps(2.5f), -0.3f, 0);
        note_on(I_HORN, n[2] + 12.0f, 0.38f, steps(2.5f), 0.3f, 0);
    }
    if (pos == 0) note_on(I_CHOIR, n[1] + 12.0f, 0.3f, steps(16), 0, 0);
    if (pos == 0 || pos == 6 || pos == 8 || (pos == 10 && (bar & 1))) note_on(I_KICK, 36, 0.85f, steps(1), 0, 0);
    if (pos == 4 || pos == 12) note_on(I_SNARE, 40, 0.7f, steps(1), 0, 0);
    if ((pos & 1) == 0) note_on(I_HAT, 80, 0.2f + 0.1f * (pos % 4 == 0), steps(1), 0, 0);
    if (pos == 0 && (bar % 8) == 0) note_on(I_CRASH, 80, 0.55f, steps(16), 0, 0);
    if ((bar % 8) == 7 && pos >= 8) note_on(I_TOM, 50.0f - (float)(pos - 8) * 1.5f, 0.75f, steps(1), (float)(pos - 12) * 0.15f, 0);
    if (bar >= 8) melody_at(BOSS_THEME, (int)(sizeof(BOSS_THEME) / sizeof(BOSS_THEME[0])), (bar - 8) * 16 + pos, I_HORN, 64, SC_MINOR, 0.6f, 0, 0);
    else if (pos == 0) note_on(I_TIMPANI, n[0], 0.6f, steps(8), 0, 0);
    if ((pos & 1) == 1) note_on(I_TIMPANI, n[0], 0.25f, steps(1), 0, 1);
}

/* ------------------------------------------------------------------ */
/* 6. Lament (death)                                                    */
/* ------------------------------------------------------------------ */

static const ChordDef DEATH_CH[4] = {{0, 0}, {3, 0}, {4, 1}, {0, 0}};
static const MNote DEATH_LINE[] = {{4, 8, 0}, {3, 8, 0}, {2, 8, 0}, {1, 8, 0}, {1, 8, 1}, {99, 8, 0}, {0, 16, 0}};

static void death_step(long s) {
    int bar = (int)(s / 16), pos = (int)(s % 16);
    if (bar >= 6) {
        if (pos == 0 && (bar % 8) == 6) note_on(I_SUB, 26, 0.4f, steps(120), 0, 0);
        return;
    }
    if (bar < 4 && pos == 0) {
        float n[3];
        chord_notes(50, SC_MINOR, DEATH_CH[bar], 0, n);
        for (int i = 0; i < 3; i++) note_on(I_STRINGS, n[i], 0.3f, steps(16), (float)(i - 1) * 0.4f, 0);
        note_on(I_BASS, n[0] - 12.0f, 0.5f, steps(16), 0, 0);
    }
    if (bar == 0 && pos == 0) note_on(I_BELL, 38, 0.6f, steps(32), 0, 0);
    melody_at(DEATH_LINE, 7, bar * 16 + pos, I_FLUTE, 62, SC_MINOR, 0.45f, 0.1f, 0);
}

/* ------------------------------------------------------------------ */
/* Engine                                                              */
/* ------------------------------------------------------------------ */

static const float TRACK_BPM[MUSIC_COUNT] = {60, 72, 104, 64, 52, 138, 50};
/*per-track loudness matching*/
static const float TRACK_GAIN[MUSIC_COUNT] = {1.0f, 1.25f, 2.2f, 1.5f, 0.65f, 0.72f, 1.7f};

void music_init(int sample_rate) {
    SR = (float)sample_rate;
    memset(V, 0, sizeof(V));
    reverb_init();
}

void music_set_track(int track) {
    if (track < 0 || track >= MUSIC_COUNT) track = MUSIC_NONE;
    g_track = track;
    g_bpm = TRACK_BPM[track];
    g_step = 0;
    g_step_acc = 0;
    for (int i = 0; i < MAX_VOICES; i++) V[i].active = false;
}

int music_track(void) { return g_track; }
void music_set_intensity(float v) { g_intensity = v < 0 ? 0 : (v > 1 ? 1 : v); }

static void step(void) {
    switch (g_track) {
    case MUSIC_TITLE: title_step(g_step); break;
    case MUSIC_TOWN: town_step(g_step); break;
    case MUSIC_DUNGEON: dungeon_step(g_step); break;
    case MUSIC_DEEP: deep_step(g_step); break;
    case MUSIC_BOSS: boss_step(g_step); break;
    case MUSIC_DEATH: death_step(g_step); break;
    default: break;
    }
    g_step++;
}

void music_render(float *out, int frames) {
    if (g_track == MUSIC_NONE) return;
    double sps = (double)SR * 60.0 / (double)g_bpm / 4.0;
    for (int i = 0; i < frames; i++) {
        if (g_step_acc <= 0.0) {
            step();
            g_step_acc += sps;
        }
        g_step_acc -= 1.0;
        g_intensity_s += (g_intensity - g_intensity_s) * 0.00003f;
        float l = 0, r = 0, rev = 0;
        for (int k = 0; k < MAX_VOICES; k++) {
            Voice *v = &V[k];
            if (!v->active) continue;
            float x = voice_sample(v);
            if (v->layer) x *= g_intensity_s;
            float pl = sqrtf(0.5f * (1.0f - v->pan)), pr = sqrtf(0.5f * (1.0f + v->pan));
            l += x * pl;
            r += x * pr;
            rev += x * INST[v->inst].rev;
        }
        float wl = reverb_ch(0, rev), wr = reverb_ch(1, rev);
        float g = TRACK_GAIN[g_track];
        out[i * 2] += (l + wl) * g;
        out[i * 2 + 1] += (r + wr) * g;
    }
}
