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

/*Immediate-mode user interface toolkit.
 *
 * Everything 2D (menus, HUD, windows, tooltips, name plates) is emitted
 * every frame into a triangle list textured by ONE atlas: TrueType fonts
 * (stb_truetype, fonts embedded in the binary), a white texel for solid
 * fills and pixel-art icons. Both renderers draw that list verbatim.
 *
 * Coordinates are LOGICAL pixels (a 1920x1080 reference scaled by the
 * UI scale); ui_begin_frame() converts them to framebuffer pixels.*/
#ifndef UI_H
#define UI_H

#include <stdbool.h>
#include <stdint.h>
#include "gfx.h"

/*Packed RGBA colour, byte order r,g,b,a in memory.*/
#define UIC(r, g, b, a) ((uint32_t)(r) | ((uint32_t)(g) << 8) | ((uint32_t)(b) << 16) | ((uint32_t)(a) << 24))
static inline uint32_t ui_alpha(uint32_t c, float a) {
    uint32_t al = (uint32_t)((float)(c >> 24) * (a < 0 ? 0 : (a > 1 ? 1 : a)));
    return (c & 0x00FFFFFFu) | (al << 24);
}
uint32_t ui_lerp_color(uint32_t a, uint32_t b, float t);

/*Theme*/
#define COL_TEXT        UIC(236, 226, 205, 255)
#define COL_TEXT_DIM    UIC(170, 160, 140, 255)
#define COL_TEXT_FAINT  UIC(120, 112, 100, 255)
#define COL_GOLD        UIC(222, 184, 96, 255)
#define COL_GOLD_DARK   UIC(140, 104, 46, 255)
#define COL_PANEL       UIC(18, 15, 22, 232)
#define COL_PANEL_LIGHT UIC(34, 29, 40, 240)
#define COL_RED         UIC(214, 62, 54, 255)
#define COL_GREEN       UIC(96, 200, 96, 255)
#define COL_BLUE        UIC(90, 150, 236, 255)
#define COL_PURPLE      UIC(178, 110, 230, 255)
#define COL_ORANGE      UIC(236, 146, 54, 255)
#define COL_WHITE       UIC(255, 255, 255, 255)
#define COL_BLACK       UIC(0, 0, 0, 255)

enum { FONT_BODY = 0, FONT_BOLD, FONT_TITLE, FONT_COUNT };

/*Text alignment flags*/
#define ALIGN_LEFT   0
#define ALIGN_CENTER 1
#define ALIGN_RIGHT  2
#define ALIGN_MIDDLE 4 /*vertical centre on y*/

/*Pixel-art icons*/
enum {
    ICON_NONE = 0,
    ICON_SWORD, ICON_AXE, ICON_BOW, ICON_STAFF, ICON_DAGGER, ICON_MACE,
    ICON_SHIELD, ICON_ARMOR, ICON_HELM, ICON_BOOTS, ICON_GLOVES, ICON_CLOAK,
    ICON_RING, ICON_AMULET, ICON_POTION, ICON_SCROLL, ICON_BOOK, ICON_FOOD,
    ICON_TORCH, ICON_OIL, ICON_ARROW, ICON_GEM, ICON_COIN, ICON_HEART,
    ICON_SKULL, ICON_BRACELET, ICON_BAG, ICON_STAR, ICON_GEAR, ICON_MAP,
    ICON_QUESTION, ICON_CHEST, ICON_STAIRS, ICON_SPEAR, ICON_WAND, ICON_MANA,
    ICON_SHOE_PRINT, ICON_EYE, ICON_HAND, ICON_SPEECH, ICON_MOON, ICON_SUN,
    ICON_COUNT
};

/*Keys the widgets care about (filled by the app from GLFW events)*/
enum {
    UIK_BACKSPACE = 1, UIK_DELETE, UIK_ENTER, UIK_TAB, UIK_ESCAPE,
    UIK_LEFT, UIK_RIGHT, UIK_UP, UIK_DOWN, UIK_HOME, UIK_END, UIK_V_PASTE,
    UIK_COUNT
};

typedef struct {
    float mx, my;           /*mouse, logical pixels*/
    bool down[3];           /*left, right, middle*/
    bool pressed[3];        /*went down this frame*/
    bool released[3];
    float wheel;
    uint32_t chars[32];     /*typed codepoints this frame*/
    int nchars;
    uint8_t keys[UIK_COUNT]; /*times pressed (incl. repeat) this frame*/
    bool shift, ctrl;
    const char *clipboard;  /*for paste*/
} UIInput;

/*Life cycle*/
bool ui_init(void);
void ui_shutdown(void);
/*Starts a frame. fb = framebuffer size; scale = logical->pixel factor.*/
void ui_begin_frame(int fb_w, int fb_h, float scale, const UIInput *in, float dt, double time);
void ui_end_frame(UIFrame *out);
float ui_width(void);   /*logical size of the screen*/
float ui_height(void);
float ui_scale(void);
double ui_time(void);
const UIInput *ui_input(void);

/*Layers: 0 = normal, 1 = overlay (popups, tooltips, drag)*/
void ui_layer(int layer);

/*Clipping (axis aligned, logical px). Nested clips intersect.*/
void ui_push_clip(float x, float y, float w, float h);
void ui_pop_clip(void);

/*Primitives*/
void ui_rect(float x, float y, float w, float h, uint32_t col);
void ui_rect_grad(float x, float y, float w, float h, uint32_t top, uint32_t bottom);
void ui_rect_hgrad(float x, float y, float w, float h, uint32_t left, uint32_t right);
void ui_rrect(float x, float y, float w, float h, float r, uint32_t col);
void ui_rrect_grad(float x, float y, float w, float h, float r, uint32_t top, uint32_t bottom);
void ui_rrect_line(float x, float y, float w, float h, float r, float t, uint32_t col);
void ui_line(float x0, float y0, float x1, float y1, float t, uint32_t col);
void ui_circle(float cx, float cy, float r, uint32_t col);
void ui_ring(float cx, float cy, float r, float t, uint32_t col);
void ui_arc(float cx, float cy, float r, float t, float a0, float a1, uint32_t col);
void ui_tri(float x0, float y0, float x1, float y1, float x2, float y2, uint32_t col);
void ui_shadow(float x, float y, float w, float h, float r, float spread, float alpha);
void ui_icon(int icon, float x, float y, float size, uint32_t tint);
void ui_diamond(float cx, float cy, float r, uint32_t col);

/*Text. size = logical pixel height (any value; the closest baked size
 * is used). Returns the advance width.*/
float ui_text(int font, float size, float x, float y, const char *s, uint32_t col, int align);
float ui_text_shadow(int font, float size, float x, float y, const char *s, uint32_t col, int align);
float ui_text_outline(int font, float size, float x, float y, const char *s, uint32_t col, uint32_t outline, int align);
float ui_text_width(int font, float size, const char *s);
float ui_text_width_n(int font, float size, const char *s, int n);
/*Word-wrapped paragraph; returns the height used. Supports '\n'.*/
float ui_text_wrap(int font, float size, float x, float y, float w, const char *s, uint32_t col, float line_h);
float ui_text_wrap_height(int font, float size, float w, const char *s, float line_h);
/*Text with inline colour codes: ^1 red ^2 green ^3 yellow ^4 blue
 * ^5 purple ^6 cyan ^7 white ^8 gold ^9 dim ^0 default.*/
float ui_text_rich(int font, float size, float x, float y, const char *s, uint32_t base, int align);

/*Interaction helpers*/
bool ui_hover(float x, float y, float w, float h);
bool ui_clicked(float x, float y, float w, float h, int button);
/*Register a rectangle as UI so the game does not receive clicks there.*/
void ui_block(float x, float y, float w, float h);
bool ui_mouse_captured(void);       /*mouse is over some UI this frame*/
bool ui_keyboard_captured(void);    /*a text field has focus*/
void ui_release_focus(void);

/*Widgets*/
enum { BTN_NORMAL = 0, BTN_PRIMARY, BTN_DANGER, BTN_GHOST, BTN_TAB, BTN_TAB_ACTIVE };
bool ui_button(const char *label, float x, float y, float w, float h, int style);
bool ui_button_icon(int icon, const char *tip, float x, float y, float size, bool active);
bool ui_checkbox(const char *label, float x, float y, bool *value);
bool ui_slider(const char *id, float x, float y, float w, float *value, float minv, float maxv);
/*Single-line text field. Returns true when Enter is pressed.
 * flags: 1 = password, 2 = take focus now, 4 = digits only*/
#define TF_PASSWORD 1
#define TF_FOCUS 2
#define TF_DIGITS 4
bool ui_text_field(const char *id, float x, float y, float w, float h, char *buf, int cap, int flags, const char *placeholder);
bool ui_text_field_focused(const char *id);
void ui_focus(const char *id);
/*Decorated window frame with a title bar. *x,*y hold the default place
 * and receive the actual one: the player can drag the window by its title
 * bar (double click: back to the default place). Clicking a window brings
 * it to the front, and widgets covered by another window ignore the mouse.
 * "Text##key" shows "Text" and identifies the window by "##key".
 * Everything drawn until ui_window_end() belongs to the window.
 * Returns false if closed via the X button.*/
#define UIW_CLOSE 1 /*X button*/
#define UIW_MODAL 2 /*dims the screen, always on top, blocks other windows*/
bool ui_window(const char *title, float *x, float *y, float w, float h, int flags);
void ui_window_end(void);
/*Moved windows, for the settings file: i-th window with a non-zero
 * offset (returns 0 past the last one) / offset restored at start-up.*/
int ui_window_saved(int i, uint32_t *id, float *dx, float *dy);
void ui_window_restore(uint32_t id, float dx, float dy);
/*Plain panel with the ornamental border.*/
void ui_panel(float x, float y, float w, float h);
void ui_frame_ornament(float x, float y, float w, float h, uint32_t col);
/*Horizontal bar with optional label centred on it.*/
void ui_bar(float x, float y, float w, float h, float frac, uint32_t col_top, uint32_t col_bot, const char *label);
/*Section header with a gold rule*/
void ui_header(const char *text, float x, float y, float w);
/*Tooltip shown next to the mouse at the end of the frame (overlay).*/
void ui_tooltip(const char *title, const char *body);
void ui_tooltip_color(const char *title, uint32_t title_col, const char *body);
/*Scroll area: begin returns the content y offset; end clamps.*/
float ui_scroll_begin(const char *id, float x, float y, float w, float h, float content_h);
void ui_scroll_end(void);

/*Toasts / banners (overlay, auto-fade)*/
void ui_toast(const char *text, uint32_t col);
void ui_draw_toasts(void);

/*UTF-8 helpers (the font covers Latin-1)*/
int ui_utf8_encode(uint32_t cp, char out[4]);

/*Atlas access (the app passes it in the UIFrame)*/
const uint8_t *ui_atlas(int *w, int *h, uint32_t *version);

#endif
