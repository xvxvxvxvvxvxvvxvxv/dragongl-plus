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

/*Application main loop: window & input, screen flow, game controls
 * (keyboard movement relative to the camera, click-to-move, auto-attack,
 * hotkeys) and the per-frame scene/UI/render pipeline.*/

#include "app.h"
#include "assets.h"
#include "audio.h"
#include "path.h"
#include "platform.h"
#include "settings.h"
#include "scene_internal.h"
#include "textures.h"
#include "net.h"
#include <GLFW/glfw3.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO_DEPRECATION
#include "stb_image_write.h"

App g_app;

/* ------------------------------------------------------------------ */
/* Input collection (GLFW callbacks -> UIInput)                        */
/* ------------------------------------------------------------------ */

static UIInput g_in;
static double g_last_mx, g_last_my;
static bool g_rmb_drag;
static float g_drag_dist;

static void cb_char(GLFWwindow *w, unsigned int cp) {
    (void)w;
    if (g_in.nchars < 32) g_in.chars[g_in.nchars++] = cp;
}

static void cb_key(GLFWwindow *w, int key, int sc, int action, int mods) {
    (void)w;
    (void)sc;
    if (action != GLFW_PRESS && action != GLFW_REPEAT) return;
    g_in.shift = (mods & GLFW_MOD_SHIFT) != 0;
    g_in.ctrl = (mods & GLFW_MOD_CONTROL) != 0;
    switch (key) {
    case GLFW_KEY_BACKSPACE: if (g_in.keys[UIK_BACKSPACE] < 255) g_in.keys[UIK_BACKSPACE]++; break;
    case GLFW_KEY_DELETE: if (g_in.keys[UIK_DELETE] < 255) g_in.keys[UIK_DELETE]++; break;
    case GLFW_KEY_ENTER: case GLFW_KEY_KP_ENTER: if (g_in.keys[UIK_ENTER] < 255) g_in.keys[UIK_ENTER]++; break;
    case GLFW_KEY_TAB: if (g_in.keys[UIK_TAB] < 255) g_in.keys[UIK_TAB]++; break;
    case GLFW_KEY_ESCAPE: if (g_in.keys[UIK_ESCAPE] < 255) g_in.keys[UIK_ESCAPE]++; break;
    case GLFW_KEY_LEFT: if (g_in.keys[UIK_LEFT] < 255) g_in.keys[UIK_LEFT]++; break;
    case GLFW_KEY_RIGHT: if (g_in.keys[UIK_RIGHT] < 255) g_in.keys[UIK_RIGHT]++; break;
    case GLFW_KEY_UP: if (g_in.keys[UIK_UP] < 255) g_in.keys[UIK_UP]++; break;
    case GLFW_KEY_DOWN: if (g_in.keys[UIK_DOWN] < 255) g_in.keys[UIK_DOWN]++; break;
    case GLFW_KEY_HOME: if (g_in.keys[UIK_HOME] < 255) g_in.keys[UIK_HOME]++; break;
    case GLFW_KEY_END: if (g_in.keys[UIK_END] < 255) g_in.keys[UIK_END]++; break;
    case GLFW_KEY_V:
        if (mods & GLFW_MOD_CONTROL) {
            if (g_in.keys[UIK_V_PASTE] < 255) g_in.keys[UIK_V_PASTE]++;
            g_in.clipboard = glfwGetClipboardString(g_app.win);
        }
        break;
    default: break;
    }
}

static void cb_scroll(GLFWwindow *w, double xo, double yo) {
    (void)w;
    (void)xo;
    g_in.wheel += (float)yo;
}

static void cb_focus(GLFWwindow *w, int focused) {
    (void)w;
    g_app.focused = focused != 0;
    audio_set_paused(!focused && g_settings.mute_unfocused);
}

/*Keys pressed this frame for game hotkeys (not text)*/
static bool g_key_pressed[GLFW_KEY_LAST + 1];
static bool g_key_prev[GLFW_KEY_LAST + 1];

static void poll_keys(void) {
    for (int k = GLFW_KEY_SPACE; k <= GLFW_KEY_LAST; k++) {
        bool down = glfwGetKey(g_app.win, k) == GLFW_PRESS;
        g_key_pressed[k] = down && !g_key_prev[k];
        g_key_prev[k] = down;
    }
}

/*Down this frame: the sticky state sampled by poll_keys (a tap that was
 * pressed and released between two frames still counts once).*/
static bool key_down(int k) { return g_key_prev[k]; }
static bool key_hit(int k) { return g_key_pressed[k]; }

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

void app_send(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (atomic_load(&g_net_status) != NET_ONLINE) return;
    client_send_text_cmd(buf);
}

void app_set_screen(Screen s) {
    g_app.prev_screen = g_app.screen;
    g_app.screen = s;
    g_app.screen_t = 0;
    g_app.screen_frames = 0;
    ui_release_focus();
    if (s == SCR_TITLE || s == SCR_SOLO || s == SCR_JOIN || s == SCR_CREATE || s == SCR_SETTINGS ||
        s == SCR_CREDITS || s == SCR_HOWTO) {
        audio_music(MUSIC_TITLE);
    }
}

void app_start_session(const LoginInfo *li, bool solo) {
    g_app.login = *li;
    g_app.solo = solo;
    g_app.err[0] = '\0';
    if (solo) {
        char err[200];
        int port = 18080 + (int)(plat_time_ms() % 1000);
        g_app.login.port = port;
        snprintf(g_app.login.host, sizeof(g_app.login.host), "127.0.0.1");
        /*random access password for the private server*/
        uint8_t rnd[8];
        if (!plat_random_bytes(rnd, sizeof(rnd))) for (int i = 0; i < 8; i++) rnd[i] = (uint8_t)rand();
        snprintf(g_app.login.server_pass, sizeof(g_app.login.server_pass), "%02x%02x%02x%02x%02x%02x%02x%02x",
                 rnd[0], rnd[1], rnd[2], rnd[3], rnd[4], rnd[5], rnd[6], rnd[7]);
        local_server_stop();
        if (!local_server_start(port, g_app.login.server_pass, err, sizeof(err))) {
            snprintf(g_app.err, sizeof(g_app.err), "%s", err);
            app_set_screen(SCR_CONNECTING);
            atomic_store(&g_net_status, NET_FAILED);
            snprintf(g_net_error, sizeof(g_net_error), "%s", err);
            return;
        }
        g_app.login.retry_ms = 90000; /*first launch generates 100 floors*/
    } else {
        g_app.login.retry_ms = 0;
    }
    session_start(&g_app.login);
    app_set_screen(SCR_CONNECTING);
}

void app_disconnect(const char *reason) {
    session_stop();
    if (g_app.solo) local_server_stop();
    g_app.solo = false;
    g_app.windows = 0;
    g_app.path_len = 0;
    g_app.target_id = 0;
    g_app.dead = false;
    if (reason) snprintf(g_app.err, sizeof(g_app.err), "%s", reason);
    scene_new_floor(-5);
    app_set_screen(SCR_TITLE);
}

void app_apply_video_settings(void) {
    GLFWmonitor *mon = glfwGetPrimaryMonitor();
    if (g_settings.fullscreen && mon) {
        const GLFWvidmode *vm = glfwGetVideoMode(mon);
        glfwSetWindowMonitor(g_app.win, mon, 0, 0, vm->width, vm->height, vm->refreshRate);
    } else if (!g_settings.fullscreen && glfwGetWindowMonitor(g_app.win)) {
        glfwSetWindowMonitor(g_app.win, NULL, 80, 80, g_settings.win_w, g_settings.win_h, 0);
    }
    g_app.gfx->set_vsync(g_settings.vsync);
}

static void take_screenshot(void) {
    int w, h;
    uint8_t *px = g_app.gfx->screenshot(&w, &h);
    if (!px) return;
    char dir[900];
    if (!plat_user_dir(dir, sizeof(dir))) snprintf(dir, sizeof(dir), ".");
    char sdir[1000];
    snprintf(sdir, sizeof(sdir), "%s/screenshots", dir);
    plat_mkdirs(sdir);
    time_t t = time(NULL);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char name[64];
    strftime(name, sizeof(name), "dragongl_%Y%m%d_%H%M%S.png", &tmv);
    snprintf(g_app.shot_path, sizeof(g_app.shot_path), "%s/%s", sdir, name);
    for (int i = 0; i < w * h; i++) px[i * 4 + 3] = 255;
    if (stbi_write_png(g_app.shot_path, w, h, 4, px, w * 4)) {
        g_app.shot_time = g_app.time;
        char msg[200];
        snprintf(msg, sizeof(msg), "Screenshot saved: %s", name);
        ui_toast(msg, COL_GOLD);
    }
    free(px);
}

/* ------------------------------------------------------------------ */
/* Game controls                                                       */
/* ------------------------------------------------------------------ */

static const Entity *find_entity(int id) {
    if (id <= 0) return NULL;
    for (int i = 0; i < CLIENT_MAX_ENTITIES; i++)
        if (g_app.snap->entities[i].active && g_app.snap->entities[i].id == id) return &g_app.snap->entities[i];
    return NULL;
}

static bool is_hostile(const Entity *e) {
    return e && !e->is_player && !e->is_merchant && !e->is_tombstone && e->kind != KIND_CHEST && e->kind != KIND_GOLD;
}

static float step_interval(void) {
    float cd = g_app.snap->movement_cooldown;
    if (cd < 0.21f) cd = 0.21f; /*server rate limit: 200 ms*/
    return cd;
}

static void send_step(int dx, int dy) {
    client_send_move(dx, dy);
    g_app.next_step_time = g_app.time + step_interval();
    if (g_app.time - g_app.last_step_sound > 0.25) {
        audio_sfx_pitch(SFX_STEP, 0.35f, 0, 0.9f + (float)(rand() % 20) / 100.0f);
        g_app.last_step_sound = g_app.time;
    }
}

static void build_occupancy(uint8_t *occ) {
    memset(occ, 0, MAP_WIDTH * MAP_HEIGHT);
    for (int i = 0; i < CLIENT_MAX_ENTITIES; i++) {
        const Entity *e = &g_app.snap->entities[i];
        if (!e->active || e->id == g_app.snap->my_entity_id) continue;
        if (e->x < 0 || e->y < 0 || e->x >= MAP_WIDTH || e->y >= MAP_HEIGHT) continue;
        if (e->is_tombstone) continue;
        occ[e->y * MAP_WIDTH + e->x] = 1;
    }
}

static bool plan_path(int tx, int ty, bool adjacent) {
    static uint8_t occ[MAP_WIDTH * MAP_HEIGHT];
    build_occupancy(occ);
    const int max = (int)(sizeof(g_app.path) / sizeof(g_app.path[0]) / 2);
    if (g_app.traveling) {
        /*long travel: only through tiles the hero has already explored*/
        const uint8_t *ex = scene_explored();
        for (int i = 0; i < MAP_WIDTH * MAP_HEIGHT; i++)
            if (!ex[i]) occ[i] = 1;
    }
    int n = path_find(g_app.snap->map, occ, g_app.snap->my_x, g_app.snap->my_y, tx, ty, adjacent, g_app.path, max);
    if (n <= 0 && !g_app.traveling) {
        /*retry ignoring other creatures (they move anyway)*/
        n = path_find(g_app.snap->map, NULL, g_app.snap->my_x, g_app.snap->my_y, tx, ty, adjacent, g_app.path, max);
    }
    g_app.path_len = n;
    g_app.path_pos = 0;
    if (n <= 0) g_app.traveling = false;
    return n > 0;
}

static bool hostile_in_sight(int radius) {
    for (int i = 0; i < CLIENT_MAX_ENTITIES; i++) {
        const Entity *e = &g_app.snap->entities[i];
        if (!e->active || !is_hostile(e)) continue;
        if (abs(e->x - g_app.snap->my_x) <= radius && abs(e->y - g_app.snap->my_y) <= radius) return true;
    }
    return false;
}

bool app_travel_to(int tx, int ty) {
    if (tx == g_app.snap->my_x && ty == g_app.snap->my_y) return false;
    if (hostile_in_sight(5)) {
        ui_toast("You cannot travel with enemies nearby.", COL_TEXT_DIM);
        return false;
    }
    g_app.traveling = true;
    g_app.path_target_id = 0;
    if (!plan_path(tx, ty, false)) {
        ui_toast("No known path there.", COL_TEXT_DIM);
        return false;
    }
    ui_toast("Travelling... (move or click to stop)", COL_TEXT);
    return true;
}

static void cancel_auto(void) {
    g_app.path_len = 0;
    g_app.path_target_id = 0;
    g_app.traveling = false;
}

static void click_world(void) {
    g_app.traveling = false;
    int id = g_app.hover_id;
    const Entity *e = find_entity(id);
    int px = g_app.snap->my_x, py = g_app.snap->my_y;
    if (e) {
        g_app.target_id = id;
        bool adj = abs(e->x - px) <= 1 && abs(e->y - py) <= 1;
        if (adj) {
            if (g_app.time >= g_app.next_step_time) send_step(e->x - px, e->y - py);
            g_app.path_len = 0;
            g_app.path_target_id = is_hostile(e) ? id : 0;
            return;
        }
        if (plan_path(e->x, e->y, true)) g_app.path_target_id = id;
        else ui_toast("No path to the target.", COL_TEXT_DIM);
        return;
    }
    if (g_app.hover_tx < 0) return;
    g_app.target_id = 0;
    g_app.path_target_id = 0;
    if (!scene_tile_explored(g_app.hover_tx, g_app.hover_ty)) return;
    if (!plan_path(g_app.hover_tx, g_app.hover_ty, false)) {
        audio_sfx(SFX_ERROR, 0.3f, 0);
    } else {
        audio_sfx(SFX_CLICK, 0.25f, 0);
    }
}

static void follow_path(void) {
    FrameSnapshot *s = g_app.snap;
    if (g_app.path_len > 0) {
        /*advance past reached nodes*/
        while (g_app.path_pos < g_app.path_len && g_app.path[g_app.path_pos * 2] == s->my_x &&
               g_app.path[g_app.path_pos * 2 + 1] == s->my_y)
            g_app.path_pos++;
        if (g_app.path_pos >= g_app.path_len) {
            g_app.path_len = 0;
            g_app.traveling = false;
        } else if (g_app.traveling && hostile_in_sight(5)) {
            cancel_auto();
            ui_toast("An enemy is near: travel interrupted.", UIC(255, 140, 100, 255));
            audio_sfx(SFX_ERROR, 0.3f, 0);
        } else if (g_app.time >= g_app.next_step_time) {
            int nx = g_app.path[g_app.path_pos * 2], ny = g_app.path[g_app.path_pos * 2 + 1];
            int dx = nx - s->my_x, dy = ny - s->my_y;
            if (abs(dx) > 1 || abs(dy) > 1) {
                /*knocked off the path (teleport, trap...): re-plan once*/
                int gx = g_app.path[(g_app.path_len - 1) * 2], gy = g_app.path[(g_app.path_len - 1) * 2 + 1];
                if (!plan_path(gx, gy, false)) g_app.path_len = 0;
                return;
            }
            /*somebody stepped in the way?*/
            for (int i = 0; i < CLIENT_MAX_ENTITIES; i++) {
                const Entity *e = &s->entities[i];
                if (e->active && e->x == nx && e->y == ny && e->id != s->my_entity_id && !e->is_tombstone &&
                    e->id != g_app.path_target_id) {
                    int gx = g_app.path[(g_app.path_len - 1) * 2], gy = g_app.path[(g_app.path_len - 1) * 2 + 1];
                    if (!plan_path(gx, gy, g_app.path_target_id != 0)) g_app.path_len = 0;
                    return;
                }
            }
            send_step(dx, dy);
        }
        return;
    }
    /*end of path: interact with / keep attacking the target*/
    if (g_app.path_target_id) {
        const Entity *e = find_entity(g_app.path_target_id);
        if (!e) { g_app.path_target_id = 0; return; }
        int dx = e->x - s->my_x, dy = e->y - s->my_y;
        if (abs(dx) <= 1 && abs(dy) <= 1 && (dx || dy)) {
            if (g_app.time >= g_app.next_step_time) {
                send_step(dx, dy);
                if (!is_hostile(e)) g_app.path_target_id = 0; /*talk/open once*/
            }
        } else if (abs(dx) + abs(dy) < 14) {
            /*chase*/
            if (!plan_path(e->x, e->y, true)) g_app.path_target_id = 0;
        } else {
            g_app.path_target_id = 0;
        }
    }
}

static void keyboard_movement(void) {
    float f = 0, r = 0;
    if (key_down(GLFW_KEY_W) || key_down(GLFW_KEY_UP) || key_down(GLFW_KEY_KP_8)) f += 1;
    if (key_down(GLFW_KEY_S) || key_down(GLFW_KEY_DOWN) || key_down(GLFW_KEY_KP_2)) f -= 1;
    if (key_down(GLFW_KEY_D) || key_down(GLFW_KEY_RIGHT) || key_down(GLFW_KEY_KP_6)) r += 1;
    if (key_down(GLFW_KEY_A) || key_down(GLFW_KEY_LEFT) || key_down(GLFW_KEY_KP_4)) r -= 1;
    int ndx = 0, ndy = 0;
    if (key_down(GLFW_KEY_KP_7)) { ndx = -1; ndy = -1; }
    if (key_down(GLFW_KEY_KP_9)) { ndx = 1; ndy = -1; }
    if (key_down(GLFW_KEY_KP_1)) { ndx = -1; ndy = 1; }
    if (key_down(GLFW_KEY_KP_3)) { ndx = 1; ndy = 1; }
    if (f == 0 && r == 0 && ndx == 0) return;
    cancel_auto();
    if (g_app.time < g_app.next_step_time) return;
    int dx, dy;
    if (ndx) {
        dx = ndx; dy = ndy;
    } else if (g_settings.cam_relative_move) {
        float yaw = scene_cam_yaw();
        /*camera forward on the ground = -(sin yaw, cos yaw)*/
        float vx = -sinf(yaw) * f + cosf(yaw) * r;
        float vz = -cosf(yaw) * f - sinf(yaw) * r;
        float a = atan2f(vz, vx);
        int sector = (int)lroundf(a / (MX_PI / 4.0f));
        dx = (int)lroundf(cosf((float)sector * MX_PI / 4.0f));
        dy = (int)lroundf(sinf((float)sector * MX_PI / 4.0f));
    } else {
        dx = (int)r;
        dy = (int)-f;
    }
    if (dx || dy) send_step(dx, dy);
}

static void cycle_target(void) {
    const SceneLabel *L;
    int n = scene_labels(&L);
    int best = -1;
    float bd = 1e9f;
    /*nearest hostile that is not the current target*/
    for (int i = 0; i < n; i++) {
        if (!L[i].hostile || L[i].id == g_app.target_id) continue;
        if (L[i].dist < bd) { bd = L[i].dist; best = i; }
    }
    if (best >= 0) g_app.target_id = L[best].id;
}

static void attack_target_or_nearest(void) {
    const Entity *e = find_entity(g_app.target_id);
    if (!e || !is_hostile(e)) {
        cycle_target();
        e = find_entity(g_app.target_id);
    }
    if (!e) {
        ui_toast("No enemy nearby.", COL_TEXT_DIM);
        return;
    }
    g_app.hover_id = e->id;
    click_world();
}

static void game_controls(void) {
    if (!hud_game_input_allowed()) return;
    bool typing = ui_keyboard_captured();
    const UIInput *in = ui_input();

    /*mouse picking*/
    g_app.hover_id = 0;
    g_app.hover_tx = -1;
    if (!ui_mouse_captured() && !g_rmb_drag) {
        g_app.hover_id = scene_entity_at(in->mx, in->my);
        int tx, ty;
        if (scene_screen_to_tile(in->mx, in->my, &tx, &ty) && scene_tile_explored(tx, ty) &&
            tile_walkable(g_app.snap->map[ty][tx])) {
            g_app.hover_tx = tx;
            g_app.hover_ty = ty;
        }
        if (in->pressed[0] && g_settings.click_to_move) click_world();
        if (in->wheel != 0) scene_cam_zoom(powf(g_settings.invert_zoom ? 1.12f : 0.89f, in->wheel));
    }
    /*right-drag rotates the camera*/
    double mx, my;
    glfwGetCursorPos(g_app.win, &mx, &my);
    if (glfwGetMouseButton(g_app.win, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS) {
        if (!g_rmb_drag && !ui_mouse_captured()) { g_rmb_drag = true; g_drag_dist = 0; }
        if (g_rmb_drag) {
            float dx = (float)(mx - g_last_mx), dy = (float)(my - g_last_my);
            g_drag_dist += fabsf(dx) + fabsf(dy);
            scene_cam_orbit(-dx * 0.006f * g_settings.cam_sensitivity, dy * 0.004f * g_settings.cam_sensitivity);
        }
    } else {
        g_rmb_drag = false;
    }
    g_last_mx = mx;
    g_last_my = my;

    if (typing) return;
    keyboard_movement();
    follow_path();

    if (key_hit(GLFW_KEY_Q)) scene_cam_orbit(MX_PI / 4.0f, 0);
    if (key_hit(GLFW_KEY_E)) scene_cam_orbit(-MX_PI / 4.0f, 0);
    if (key_hit(GLFW_KEY_HOME)) scene_cam_reset();
    if (key_hit(GLFW_KEY_PAGE_UP)) scene_cam_zoom(0.85f);
    if (key_hit(GLFW_KEY_PAGE_DOWN)) scene_cam_zoom(1.18f);
    if (key_hit(GLFW_KEY_I)) hud_toggle(WIN_INVENTORY);
    if (key_hit(GLFW_KEY_C)) hud_toggle(WIN_CHARACTER);
    if (key_hit(GLFW_KEY_K) || key_hit(GLFW_KEY_B)) hud_toggle(WIN_SPELLS);
    if (key_hit(GLFW_KEY_M)) hud_toggle(WIN_MAP);
    if (key_hit(GLFW_KEY_H) || key_hit(GLFW_KEY_F1)) hud_toggle(WIN_HELP);
    if (key_hit(GLFW_KEY_F)) hud_do_context_action();
    if (key_hit(GLFW_KEY_G)) app_send("get");
    if (key_hit(GLFW_KEY_R)) app_send("rest");
    if (key_hit(GLFW_KEY_L)) app_send("look");
    if (key_hit(GLFW_KEY_T)) app_send("search");
    if (key_hit(GLFW_KEY_N)) g_settings.show_all_names = !g_settings.show_all_names;
    if (key_hit(GLFW_KEY_TAB)) cycle_target();
    if (key_hit(GLFW_KEY_SPACE)) attack_target_or_nearest();
    static const int num[10] = {GLFW_KEY_1, GLFW_KEY_2, GLFW_KEY_3, GLFW_KEY_4, GLFW_KEY_5,
                                GLFW_KEY_6, GLFW_KEY_7, GLFW_KEY_8, GLFW_KEY_9, GLFW_KEY_0};
    for (int i = 0; i < 4; i++)
        if (key_hit(num[i])) hud_use_belt(i);
    for (int i = 0; i < HOTBAR_SPELLS; i++)
        if (key_hit(num[4 + i])) hud_cast_hotbar(i);
}

/* ------------------------------------------------------------------ */
/* Main loop                                                           */
/* ------------------------------------------------------------------ */

static float auto_scale(int fbw, int fbh) {
    float s = fminf((float)fbw / 1920.0f, (float)fbh / 1080.0f);
    s = clampf(s, 0.55f, 3.0f);
    return s * g_settings.ui_scale;
}

static void connection_watch(void) {
    int st = atomic_load(&g_net_status);
    if (g_app.screen == SCR_CONNECTING) {
        if (st == NET_ONLINE) {
            app_set_screen(SCR_GAME);
            g_app.windows = 0;
            g_app.dead = false;
            g_app.last_floor_seen = -1;
            g_app.ev_cursor_scene = g_app.ev_cursor_audio = g_app.ev_cursor_hud = g_evq_head;
            if (!g_app.solo) {
                snprintf(g_settings.last_host, sizeof(g_settings.last_host), "%s", g_app.login.host);
                g_settings.last_port = g_app.login.port;
                snprintf(g_settings.last_user, sizeof(g_settings.last_user), "%s", g_app.login.user);
            } else {
                snprintf(g_settings.solo_user, sizeof(g_settings.solo_user), "%s", g_app.login.user);
            }
            settings_save();
        }
        if (g_app.solo && !local_server_running() && st != NET_ONLINE && st != NET_FAILED) {
            char tail[600];
            local_server_log_tail(tail, sizeof(tail));
            char *last = strrchr(tail, '\n');
            while (last && last > tail && last[1] == '\0') { *last = '\0'; last = strrchr(tail, '\n'); }
            snprintf(g_net_error, sizeof(g_net_error), "The solo server stopped: %s", last ? last + 1 : tail);
            atomic_store(&g_net_status, NET_FAILED);
        }
    } else if (g_app.screen == SCR_GAME) {
        if (st == NET_DISCONNECTED || st == NET_FAILED) {
            char reason[200];
            snprintf(reason, sizeof(reason), "Disconnected: %s", g_net_error[0] ? g_net_error : "connection lost");
            app_disconnect(reason);
        }
    }
}

static void game_music(void) {
    int floor = g_app.snap->my_floor;
    int track = MUSIC_DUNGEON;
    if (floor == 0) track = MUSIC_TOWN;
    else if (floor % 10 == 0) track = MUSIC_BOSS;
    else if (floor >= 40) track = MUSIC_DEEP;
    if (g_app.dead) track = MUSIC_DEATH;
    audio_music(track);
    /*combat intensity: hostile creatures close by, recent fighting*/
    float threat = 0;
    for (int i = 0; i < CLIENT_MAX_ENTITIES; i++) {
        const Entity *e = &g_app.snap->entities[i];
        if (!e->active || !is_hostile(e)) continue;
        int d = abs(e->x - g_app.snap->my_x) + abs(e->y - g_app.snap->my_y);
        if (d <= 6) threat += 0.35f;
    }
    if (g_app.time - g_app.last_combat < 6.0) threat += 0.6f;
    g_app.combat_intensity = damp(g_app.combat_intensity, clampf(threat, 0, 1), 1.5f, g_app.dt);
    audio_set_intensity(g_app.combat_intensity);
}

void app_run(void) {
    double last = glfwGetTime();
    static SceneFrame sf;
    static UIFrame uf;
    while (!glfwWindowShouldClose(g_app.win) && g_running && !g_app.request_quit) {
        memset(&g_in, 0, sizeof(g_in));
        glfwPollEvents();
        double now = glfwGetTime();
        g_app.dt = (float)fmin(0.1, now - last);
        last = now;
        g_app.time = now;
        g_app.screen_t += g_app.dt;
        g_app.screen_frames++;
        poll_keys();

        glfwGetFramebufferSize(g_app.win, &g_app.fb_w, &g_app.fb_h);
        if (g_app.fb_w <= 0 || g_app.fb_h <= 0) {
            plat_sleep_ms(30);
            continue;
        }
        if (!glfwGetWindowMonitor(g_app.win)) {
            int ww, wh;
            glfwGetWindowSize(g_app.win, &ww, &wh);
            if (ww >= 800 && wh >= 600) { g_settings.win_w = ww; g_settings.win_h = wh; }
        }
        g_app.ui_scale = auto_scale(g_app.fb_w, g_app.fb_h);
        double mx, my;
        glfwGetCursorPos(g_app.win, &mx, &my);
        int ww, wh;
        glfwGetWindowSize(g_app.win, &ww, &wh);
        float cs = ww > 0 ? (float)g_app.fb_w / (float)ww : 1.0f; /*HiDPI*/
        g_in.mx = (float)mx * cs / g_app.ui_scale;
        g_in.my = (float)my * cs / g_app.ui_scale;
        static bool prev[3];
        int btn[3] = {GLFW_MOUSE_BUTTON_LEFT, GLFW_MOUSE_BUTTON_RIGHT, GLFW_MOUSE_BUTTON_MIDDLE};
        for (int b = 0; b < 3; b++) {
            bool d = glfwGetMouseButton(g_app.win, btn[b]) == GLFW_PRESS;
            g_in.down[b] = d;
            g_in.pressed[b] = d && !prev[b];
            g_in.released[b] = !d && prev[b];
            prev[b] = d;
        }
        ui_begin_frame(g_app.fb_w, g_app.fb_h, g_app.ui_scale, &g_in, g_app.dt, now);
        audio_set_volumes(g_settings.vol_master, g_settings.vol_music, g_settings.vol_sfx);

        connection_watch();
        bool in_game = g_app.screen == SCR_GAME;
        FrameSnapshot *snap = in_game ? g_app.snap : g_app.title_snap;
        if (in_game) frame_snapshot_acquire(g_app.snap);

        if (in_game) {
            hud_handle_events();
            game_music();
        }

        SceneInput si;
        memset(&si, 0, sizeof(si));
        si.title_mode = !in_game;
        si.quality = g_settings.quality;
        si.brightness = g_settings.brightness;
        si.hover_id = g_app.hover_id;
        si.target_id = g_app.target_id;
        si.hover_tx = g_app.hover_tx;
        si.hover_ty = g_app.hover_ty;
        si.path = g_app.path + g_app.path_pos * 2;
        si.path_len = g_app.path_len > g_app.path_pos ? g_app.path_len - g_app.path_pos : 0;
        if (si.path_len > 48) si.path_len = 48; /*long travels: show the next stretch only*/
        si.weapon_name = g_weapon_name;
        si.race_id = g_race_id;
        si.class_id = g_class_id;
        si.ui_scale = g_app.ui_scale;
        {
            extern int screens_preview(int *race, int *cls);
            int pr, pc;
            if (!in_game && screens_preview(&pr, &pc)) {
                si.preview = true;
                si.preview_race = pr;
                si.preview_class = pc;
                si.preview_offset_x = 0.0f;
                extern int screens_preview_spot(int *x, int *y);
                screens_preview_spot(&si.preview_x, &si.preview_y);
            }
        }
        for (int i = 0; i < g_inventory.count; i++) {
            const InvItem *it = &g_inventory.items[i];
            if (it->where != INV_EQUIPPED) continue;
            if (it->slot == EQ_HEAD) si.helm = true;
            if (it->slot == EQ_BACK) si.cape = true;
            if ((it->slot == EQ_HAND_L || it->slot == EQ_ARM_L) && it->category == 2 /*ITEM_SHIELD*/) si.shield = true;
        }
        scene_frame(snap, &si, g_app.dt, now, g_app.fb_w, g_app.fb_h, &sf);

        if (in_game) {
            hud_draw();
            game_controls();
        } else {
            screens_draw();
        }
        ui_draw_toasts();
        if (g_app.screen != SCR_GAME) {
            /*menus: darker vignette*/
            sf.vignette = 0.9f;
        } else {
            sf.screen_tint[0] = 0.6f;
            sf.screen_tint[1] = 0.0f;
            sf.screen_tint[2] = 0.0f;
            sf.screen_tint[3] = g_app.damage_flash * 0.35f;
            g_app.damage_flash = fmaxf(0, g_app.damage_flash - g_app.dt * 2.5f);
            if (g_app.snap->max_hp > 0 && g_app.snap->hp * 4 < g_app.snap->max_hp)
                sf.vignette = 0.6f + 0.25f * sinf((float)now * 4.0f);
        }
        if (key_hit(GLFW_KEY_F12) && !g_app.want_screenshot) {
            int dw, dh;
            g_app.want_screenshot = true;
            g_app.gfx->screenshot(&dw, &dh); /*arms the capture of this frame*/
        }
        ui_end_frame(&uf);
        g_app.gfx->render(&sf, &uf, g_app.fb_w, g_app.fb_h);
        if (g_app.want_screenshot) {
            take_screenshot();
            g_app.want_screenshot = false;
        }
        if (!g_app.focused) plat_sleep_ms(20);
    }
}

bool app_init(const GfxBackend *gfx) {
    memset(&g_app, 0, sizeof(g_app));
    g_app.gfx = gfx;
    g_app.focused = true;
    g_app.snap = calloc(1, sizeof(FrameSnapshot));
    g_app.title_snap = calloc(1, sizeof(FrameSnapshot));
    if (!g_app.snap || !g_app.title_snap) return false;
    g_app.map_zoom = 1;

    gfx->window_hints();
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWmonitor *mon = g_settings.fullscreen ? glfwGetPrimaryMonitor() : NULL;
    int w = g_settings.win_w, h = g_settings.win_h;
    if (mon) {
        const GLFWvidmode *vm = glfwGetVideoMode(mon);
        w = vm->width;
        h = vm->height;
    }
    g_app.win = glfwCreateWindow(w, h, "Dragon GL - The Long Descent", mon, NULL);
    if (!g_app.win) return false;
    {
        GLFWimage icons[2] = {{64, 64, (unsigned char *)asset_icon64}, {32, 32, (unsigned char *)asset_icon32}};
        glfwSetWindowIcon(g_app.win, 2, icons); /*no-op where unsupported (Wayland, macOS)*/
    }
    glfwSetCharCallback(g_app.win, cb_char);
    glfwSetKeyCallback(g_app.win, cb_key);
    glfwSetScrollCallback(g_app.win, cb_scroll);
    glfwSetWindowFocusCallback(g_app.win, cb_focus);
    /*Sticky input: a click or key tap that is pressed AND released
     * between two frames (fast taps, touchpads, low frame rates) is
     * still reported once instead of being lost.*/
    glfwSetInputMode(g_app.win, GLFW_STICKY_KEYS, GLFW_TRUE);
    glfwSetInputMode(g_app.win, GLFW_STICKY_MOUSE_BUTTONS, GLFW_TRUE);
    glfwSetWindowSizeLimits(g_app.win, 960, 600, GLFW_DONT_CARE, GLFW_DONT_CARE);
    if (!gfx->init(g_app.win, g_settings.vsync)) {
        glfwDestroyWindow(g_app.win);
        g_app.win = NULL;
        return false;
    }
    uint8_t *mats = textures_generate();
    if (!mats) return false;
    gfx->upload_materials(mats, MAT_TEX_SIZE, MAT_COUNT);
    free(mats);
    if (!ui_init()) return false;
    scene_init();
    scene_new_floor(-5);
    screens_title_snapshot_init();
    glfwShowWindow(g_app.win);
    app_set_screen(SCR_TITLE);
    return true;
}

void app_shutdown(void) {
    session_stop();
    local_server_stop();
    settings_save();
    if (g_app.gfx) g_app.gfx->shutdown();
    if (g_app.win) glfwDestroyWindow(g_app.win);
    ui_shutdown();
    free(g_app.snap);
    free(g_app.title_snap);
}
