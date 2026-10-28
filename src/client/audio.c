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

/*Audio output and sound effects.
 *
 * Every sound effect is synthesized once at start-up into a small mono
 * buffer; the audio callback mixes up to SFX_VOICES of them (with pitch
 * and pan) over the procedural soundtrack from music.c. The main thread
 * talks to the callback only through atomics and a single-producer /
 * single-consumer request ring, so the callback never blocks.*/

#include "audio.h"
#include "music.h"
#include "miniaudio.h"
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AUDIO_SR 48000
#define SFX_VOICES 24
#define REQ_RING 64
#define PI_F 3.14159265f
#define TAU_F 6.28318531f

typedef struct {
    float *data;
    int len;
} SfxBuf;

typedef struct {
    int id;
    float vol, pan, pitch;
} SfxReq;

typedef struct {
    bool active;
    int id;
    double pos;
    float step, gl, gr;
} SfxVoice;

static ma_device g_dev;
static bool g_ok;
static SfxBuf g_sfx[SFX_COUNT];
static SfxVoice g_voices[SFX_VOICES];

static SfxReq g_req[REQ_RING];
static atomic_uint g_req_w, g_req_r;

static atomic_int g_want_track;
static atomic_int g_paused;
static _Atomic float g_vol_master = 0.8f, g_vol_music = 0.7f, g_vol_sfx = 0.8f, g_intensity;

/*audio-thread state*/
static int g_cur_track = MUSIC_NONE;
static float g_fade = 1.0f;       /*music gain used for cross-fades*/
static float g_pause_gain = 1.0f;
static float g_mix[2 * 4096];

/* ------------------------------------------------------------------ */
/* Sound effect synthesis (offline, at start-up)                       */
/* ------------------------------------------------------------------ */

static uint32_t g_seed = 0x1234567u;
static float nrand(void) {
    g_seed ^= g_seed << 13; g_seed ^= g_seed >> 17; g_seed ^= g_seed << 5;
    return (float)(g_seed & 0xFFFFFF) / 8388607.5f - 1.0f;
}

static float *sfx_alloc(int id, float seconds) {
    int n = (int)(seconds * AUDIO_SR);
    g_sfx[id].data = calloc((size_t)n, sizeof(float));
    g_sfx[id].len = g_sfx[id].data ? n : 0;
    return g_sfx[id].data;
}

typedef struct { float lp, bp; } Svf;
static float svf(Svf *f, float x, float fc, float q, int mode) {
    float k = 2.0f * sinf(PI_F * fminf(fc, AUDIO_SR * 0.22f) / AUDIO_SR);
    float hp = x - f->lp - q * f->bp;
    f->bp += k * hp;
    f->lp += k * f->bp;
    return mode == 0 ? f->lp : (mode == 1 ? f->bp : hp);
}

static float tone(float t, float f) { return sinf(TAU_F * f * t); }

/*a decaying plucked/bell tone at time t relative to `start`*/
static float chime(float t, float start, float f, float decay) {
    if (t < start) return 0;
    float u = t - start;
    return synth_fm_bell(u, f, 2.0f, 1.2f, decay) * fminf(1.0f, u * 400.0f);
}

static void normalize(int id, float peak) {
    SfxBuf *b = &g_sfx[id];
    float m = 0;
    for (int i = 0; i < b->len; i++) m = fmaxf(m, fabsf(b->data[i]));
    if (m < 1e-6f) return;
    float g = peak / m;
    for (int i = 0; i < b->len; i++) b->data[i] *= g;
    /*short fade-out against clicks*/
    int f = AUDIO_SR / 200;
    for (int i = 0; i < f && i < b->len; i++) b->data[b->len - 1 - i] *= (float)i / (float)f;
}

static void build_sfx(void) {
    const float dt = 1.0f / AUDIO_SR;
    float *d;
    Svf a = {0}, b = {0};
    int n;

#define LOOP(id, secs) \
    d = sfx_alloc(id, secs); n = g_sfx[id].len; memset(&a, 0, sizeof a); memset(&b, 0, sizeof b); \
    for (int i = 0; i < n; i++) { float t = (float)i * dt; (void)t;
#define END(id, peak) } normalize(id, peak);

    LOOP(SFX_CLICK, 0.06f)
        d[i] = tone(t, 1250.0f) * expf(-t * 90.0f) + svf(&a, nrand(), 3000, 0.6f, 1) * expf(-t * 300.0f) * 0.5f;
    END(SFX_CLICK, 0.6f)

    LOOP(SFX_HOVER, 0.04f)
        d[i] = tone(t, 2100.0f) * expf(-t * 160.0f);
    END(SFX_HOVER, 0.25f)

    LOOP(SFX_STEP, 0.12f)
        d[i] = svf(&a, nrand(), 420.0f, 0.7f, 0) * expf(-t * 45.0f) + tone(t, 80.0f) * expf(-t * 40.0f) * 0.4f;
    END(SFX_STEP, 0.5f)

    LOOP(SFX_HIT, 0.25f)
        float thump = tone(t, 70.0f + 80.0f * expf(-t * 30.0f)) * expf(-t * 18.0f);
        float crack = svf(&a, nrand(), 1800.0f - 1400.0f * fminf(1.0f, t * 8.0f), 0.5f, 1) * expf(-t * 28.0f);
        d[i] = thump * 0.9f + crack * 1.4f;
    END(SFX_HIT, 0.85f)

    LOOP(SFX_CRIT, 0.6f)
        float thump = tone(t, 60.0f + 100.0f * expf(-t * 25.0f)) * expf(-t * 12.0f);
        float crack = svf(&a, nrand(), 2600.0f, 0.4f, 1) * expf(-t * 22.0f);
        float ring = synth_fm_bell(t, 880.0f, 1.41f, 2.5f, 5.0f) * 0.35f;
        d[i] = thump + crack * 1.4f + ring;
    END(SFX_CRIT, 0.95f)

    LOOP(SFX_MISS, 0.25f)
        float env = sinf(PI_F * fminf(1.0f, t / 0.22f));
        d[i] = svf(&a, nrand(), 500.0f + 2500.0f * t / 0.25f, 0.35f, 1) * env * env;
    END(SFX_MISS, 0.5f)

    LOOP(SFX_HURT, 0.3f)
        float f = 190.0f - 90.0f * t / 0.3f;
        float g = svf(&b, (fmodf(t * f, 1.0f) * 2.0f - 1.0f), 900.0f, 0.4f, 0);
        d[i] = g * expf(-t * 9.0f) + tone(t, 55.0f) * expf(-t * 20.0f) + svf(&a, nrand(), 1200, 0.5f, 1) * expf(-t * 35.0f);
    END(SFX_HURT, 0.85f)

    LOOP(SFX_KILL, 0.7f)
        float f = 300.0f * expf(-t * 3.0f);
        float g = svf(&b, (fmodf(t * f, 1.0f) * 2.0f - 1.0f), 1200.0f, 0.5f, 0);
        d[i] = g * expf(-t * 4.0f) * 0.8f + svf(&a, nrand(), 600.0f, 0.6f, 0) * expf(-t * 6.0f) +
               tone(t, 50.0f) * expf(-t * 8.0f) * 0.8f;
    END(SFX_KILL, 0.85f)

    LOOP(SFX_COIN, 0.45f)
        d[i] = chime(t, 0.0f, 1976.0f, 9.0f) + chime(t, 0.07f, 2637.0f, 7.0f);
    END(SFX_COIN, 0.6f)

    LOOP(SFX_LEVELUP, 1.6f)
        static const float notes[5] = {523.25f, 659.25f, 783.99f, 1046.5f, 1318.5f};
        float x = 0;
        for (int k = 0; k < 5; k++) x += chime(t, 0.09f * (float)k, notes[k], 2.2f) * (k == 4 ? 0.9f : 0.6f);
        float sh = svf(&a, nrand(), 6000.0f, 0.3f, 1) * expf(-t * 2.0f) * fminf(1.0f, t * 3.0f) * 0.25f;
        d[i] = x + sh + tone(t, 261.6f) * expf(-t * 1.5f) * 0.3f * fminf(1.0f, t * 40.0f);
    END(SFX_LEVELUP, 0.9f)

    LOOP(SFX_SPELL, 0.8f)
        float f = 300.0f + 900.0f * t;
        float x = synth_fm_bell(t, f, 1.5f, 3.0f * expf(-t), 3.0f);
        float sh = svf(&a, nrand(), 2000.0f + 5000.0f * t, 0.25f, 1) * sinf(PI_F * t / 0.8f) * 0.4f;
        d[i] = x * 0.6f + sh;
    END(SFX_SPELL, 0.75f)

    LOOP(SFX_HEAL, 1.0f)
        float env = fminf(1.0f, t * 10.0f) * expf(-t * 2.5f);
        float x = tone(t, 523.25f * (1.0f + 0.1f * t)) + tone(t, 659.25f * (1.0f + 0.1f * t)) * 0.8f +
                  tone(t, 783.99f * (1.0f + 0.1f * t)) * 0.6f;
        d[i] = x * env + chime(t, 0.15f, 1568.0f, 4.0f) * 0.4f;
    END(SFX_HEAL, 0.65f)

    LOOP(SFX_STAIRS, 1.2f)
        float x = 0;
        for (int k = 0; k < 4; k++) {
            float s0 = 0.16f * (float)k;
            if (t >= s0) x += tone(t - s0, 90.0f - 8.0f * (float)k) * expf(-(t - s0) * 25.0f) * (1.0f - 0.15f * (float)k);
        }
        float wind = svf(&a, nrand(), 300.0f + 200.0f * sinf(t * 3.0f), 0.2f, 1) * sinf(PI_F * t / 1.2f) * 0.8f;
        d[i] = x + wind;
    END(SFX_STAIRS, 0.75f)

    LOOP(SFX_PICKUP, 0.18f)
        float f = 700.0f + 900.0f * t / 0.18f;
        d[i] = tone(t, f) * expf(-t * 18.0f) + svf(&a, nrand(), 3500.0f, 0.5f, 1) * expf(-t * 80.0f) * 0.3f;
    END(SFX_PICKUP, 0.55f)

    LOOP(SFX_DEATH, 2.5f)
        float bell = synth_fm_bell(t, 98.0f, 1.4f, 3.0f, 1.0f);
        float low = tone(t, 49.0f) * expf(-t * 1.2f) * fminf(1.0f, t * 20.0f);
        float f = 220.0f * expf(-t * 0.6f);
        float wail = svf(&b, fmodf(t * f, 1.0f) * 2.0f - 1.0f, 700.0f, 0.3f, 0) * sinf(PI_F * fminf(1.0f, t / 2.5f)) * 0.5f;
        d[i] = bell + low + wail;
    END(SFX_DEATH, 0.9f)

    LOOP(SFX_ERROR, 0.25f)
        float sq = (fmodf(t * 160.0f, 1.0f) < 0.5f ? 1.0f : -1.0f);
        float gate = (t < 0.09f || (t > 0.12f && t < 0.21f)) ? 1.0f : 0.0f;
        d[i] = svf(&a, sq * gate, 900.0f, 0.6f, 0);
    END(SFX_ERROR, 0.45f)

    LOOP(SFX_OPEN, 0.2f)
        d[i] = svf(&a, nrand(), 900.0f + 3000.0f * t / 0.2f, 0.4f, 1) * sinf(PI_F * t / 0.2f) +
               tone(t, 600.0f) * expf(-t * 60.0f) * 0.3f;
    END(SFX_OPEN, 0.45f)

    LOOP(SFX_CLOSE, 0.18f)
        d[i] = svf(&a, nrand(), 3500.0f - 3000.0f * t / 0.18f, 0.4f, 1) * sinf(PI_F * t / 0.18f) +
               tone(t, 300.0f) * expf(-(fmaxf(0, t - 0.12f)) * 60.0f) * (t > 0.12f) * 0.4f;
    END(SFX_CLOSE, 0.4f)

    LOOP(SFX_EQUIP, 0.4f)
        float x = synth_fm_bell(t, 520.0f, 2.76f, 4.0f, 9.0f) + synth_fm_bell(t, 1310.0f, 1.73f, 2.0f, 12.0f) * 0.5f;
        d[i] = x + svf(&a, nrand(), 4000.0f, 0.4f, 1) * expf(-t * 60.0f) * 0.6f;
    END(SFX_EQUIP, 0.6f)

    LOOP(SFX_EXPLOSION, 1.4f)
        float boom = tone(t, 40.0f + 60.0f * expf(-t * 8.0f)) * expf(-t * 3.0f);
        float roar = svf(&a, nrand(), 200.0f + 2500.0f * expf(-t * 4.0f), 0.5f, 0) * expf(-t * 2.2f);
        float crackle = (nrand() > 0.985f ? nrand() : 0.0f) * expf(-t * 3.0f);
        d[i] = boom + roar * 1.5f + crackle;
    END(SFX_EXPLOSION, 0.95f)

    LOOP(SFX_MESSAGE, 0.4f)
        d[i] = chime(t, 0.0f, 1174.7f, 8.0f) + chime(t, 0.09f, 1568.0f, 7.0f) * 0.8f;
    END(SFX_MESSAGE, 0.45f)

    LOOP(SFX_BOSS, 3.0f)
        float gong = synth_fm_bell(t, 65.4f, 1.41f, 4.0f, 0.9f) + synth_fm_bell(t, 98.0f, 2.1f, 2.0f, 1.4f) * 0.5f;
        float bf = 82.4f;
        float brass = 0;
        for (int h = 1; h <= 3; h++) brass += svf(&b, fmodf(t * bf * (float)h, 1.0f) * 2.0f - 1.0f, 600.0f + 900.0f * expf(-t), 0.6f, 0) * 0.33f;
        brass *= fminf(1.0f, t * 8.0f) * expf(-t * 0.9f);
        d[i] = gong + brass * 0.8f + tone(t, 41.2f) * expf(-t * 1.0f) * 0.7f;
    END(SFX_BOSS, 0.95f)

#undef LOOP
#undef END
}

/* ------------------------------------------------------------------ */
/* Audio callback                                                      */
/* ------------------------------------------------------------------ */

static void start_voice(const SfxReq *r) {
    if (r->id < 0 || r->id >= SFX_COUNT || !g_sfx[r->id].len) return;
    SfxVoice *v = NULL;
    double best = -1;
    for (int i = 0; i < SFX_VOICES; i++) {
        if (!g_voices[i].active) { v = &g_voices[i]; break; }
        if (g_voices[i].pos > best) { best = g_voices[i].pos; v = &g_voices[i]; }
    }
    float pan = r->pan < -1 ? -1 : (r->pan > 1 ? 1 : r->pan);
    v->active = true;
    v->id = r->id;
    v->pos = 0;
    v->step = r->pitch > 0.05f ? r->pitch : 1.0f;
    v->gl = r->vol * sqrtf(0.5f * (1.0f - pan));
    v->gr = r->vol * sqrtf(0.5f * (1.0f + pan));
}

static inline float softclip(float x) {
    if (x > 1.5f) return 1.0f;
    if (x < -1.5f) return -1.0f;
    return x - x * x * x * (4.0f / 27.0f);
}

static void callback(ma_device *dev, void *out_v, const void *in, ma_uint32 frames) {
    (void)dev;
    (void)in;
    float *out = out_v;
    while (frames > 0) {
        ma_uint32 nf = frames > 2048 ? 2048 : frames;

        /*requests from the game thread*/
        unsigned w = atomic_load_explicit(&g_req_w, memory_order_acquire);
        unsigned r = atomic_load_explicit(&g_req_r, memory_order_relaxed);
        while (r != w) {
            start_voice(&g_req[r % REQ_RING]);
            r++;
        }
        atomic_store_explicit(&g_req_r, r, memory_order_release);

        float master = atomic_load(&g_vol_master), vmus = atomic_load(&g_vol_music), vsfx = atomic_load(&g_vol_sfx);
        int want = atomic_load(&g_want_track);
        bool paused = atomic_load(&g_paused) != 0;
        music_set_intensity(atomic_load(&g_intensity));

        memset(g_mix, 0, sizeof(float) * 2 * nf);
        if (g_cur_track != MUSIC_NONE) music_render(g_mix, (int)nf);

        /*cross-fade: fade out over ~1.2 s, switch, fade in over ~1.5 s*/
        float fade_step_out = 1.0f / (1.2f * AUDIO_SR), fade_step_in = 1.0f / (1.5f * AUDIO_SR);
        float pg_step = 1.0f / (0.3f * AUDIO_SR);
        for (ma_uint32 i = 0; i < nf; i++) {
            if (want != g_cur_track) {
                g_fade -= fade_step_out;
                if (g_fade <= 0.0f || g_cur_track == MUSIC_NONE) {
                    g_fade = 0.0f;
                    g_cur_track = want;
                    music_set_track(want);
                    /*the rest of this block was rendered for the old track: silence it*/
                    for (ma_uint32 k = i; k < nf; k++) g_mix[k * 2] = g_mix[k * 2 + 1] = 0;
                }
            } else if (g_fade < 1.0f) {
                g_fade = fminf(1.0f, g_fade + fade_step_in);
            }
            g_pause_gain = paused ? fmaxf(0.0f, g_pause_gain - pg_step) : fminf(1.0f, g_pause_gain + pg_step);
            float mg = g_fade * g_fade * vmus;
            g_mix[i * 2] *= mg;
            g_mix[i * 2 + 1] *= mg;
        }

        for (int k = 0; k < SFX_VOICES; k++) {
            SfxVoice *v = &g_voices[k];
            if (!v->active) continue;
            const SfxBuf *b = &g_sfx[v->id];
            for (ma_uint32 i = 0; i < nf; i++) {
                int p = (int)v->pos;
                if (p + 1 >= b->len) { v->active = false; break; }
                float fr = (float)(v->pos - p);
                float s = b->data[p] + (b->data[p + 1] - b->data[p]) * fr;
                g_mix[i * 2] += s * v->gl * vsfx;
                g_mix[i * 2 + 1] += s * v->gr * vsfx;
                v->pos += v->step;
            }
        }

        for (ma_uint32 i = 0; i < nf; i++) {
            float g = master * g_pause_gain;
            out[i * 2] = softclip(g_mix[i * 2] * g);
            out[i * 2 + 1] = softclip(g_mix[i * 2 + 1] * g);
        }
        out += nf * 2;
        frames -= nf;
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

bool audio_init(void) {
    if (g_ok) return true;
    music_init(AUDIO_SR);
    build_sfx();
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = 2;
    cfg.sampleRate = AUDIO_SR;
    cfg.dataCallback = callback;
    cfg.periodSizeInMilliseconds = 20;
    if (ma_device_init(NULL, &cfg, &g_dev) != MA_SUCCESS) {
        fprintf(stderr, "[AUDIO] no playback device: running silent\n");
        return false;
    }
    if (ma_device_start(&g_dev) != MA_SUCCESS) {
        fprintf(stderr, "[AUDIO] could not start the playback device\n");
        ma_device_uninit(&g_dev);
        return false;
    }
    g_ok = true;
    return true;
}

void audio_shutdown(void) {
    if (g_ok) ma_device_uninit(&g_dev);
    g_ok = false;
    for (int i = 0; i < SFX_COUNT; i++) {
        free(g_sfx[i].data);
        g_sfx[i].data = NULL;
        g_sfx[i].len = 0;
    }
}

bool audio_available(void) { return g_ok; }

void audio_set_volumes(float master, float music, float sfx) {
    atomic_store(&g_vol_master, master);
    atomic_store(&g_vol_music, music);
    atomic_store(&g_vol_sfx, sfx);
}

void audio_set_paused(bool paused) { atomic_store(&g_paused, paused ? 1 : 0); }

void audio_music(int track) {
    if (track < 0 || track >= MUSIC_COUNT) track = MUSIC_NONE;
    atomic_store(&g_want_track, track);
}

void audio_set_intensity(float v) { atomic_store(&g_intensity, v); }

void audio_sfx_pitch(int id, float volume, float pan, float pitch) {
    if (!g_ok || id < 0 || id >= SFX_COUNT) return;
    unsigned w = atomic_load_explicit(&g_req_w, memory_order_relaxed);
    unsigned r = atomic_load_explicit(&g_req_r, memory_order_acquire);
    if (w - r >= REQ_RING) return; /*queue full: drop*/
    g_req[w % REQ_RING] = (SfxReq){id, volume, pan, pitch};
    atomic_store_explicit(&g_req_w, w + 1, memory_order_release);
}

void audio_sfx(int id, float volume, float pan) { audio_sfx_pitch(id, volume, pan, 1.0f); }

const char *audio_track_name(int track) {
    static const char *names[MUSIC_COUNT] = {
        "", "The Long Descent", "Hearth of the Maw", "Echoes Below", "The Abyssal Choir", "Guardian of the Seal", "Lament",
    };
    return (track >= 0 && track < MUSIC_COUNT) ? names[track] : "";
}

#ifdef DRAGONGL_AUDIO_RENDER_TOOL
/*Offline renderer used to check levels: renders `seconds` of a track to
 * a raw float32 stereo file.*/
int audio_render_offline(int track, float seconds, float intensity, const char *path) {
    music_init(AUDIO_SR);
    music_set_track(track);
    music_set_intensity(intensity);
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int total = (int)(seconds * AUDIO_SR);
    static float buf[2 * 1024];
    while (total > 0) {
        int n = total > 1024 ? 1024 : total;
        memset(buf, 0, sizeof(buf));
        music_render(buf, n);
        for (int i = 0; i < n * 2; i++) buf[i] = softclip(buf[i] * 0.8f);
        fwrite(buf, sizeof(float), (size_t)n * 2, f);
        total -= n;
    }
    fclose(f);
    return 0;
}
#endif
