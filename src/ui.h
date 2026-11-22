#ifndef CELLSCOPE_UI_H
#define CELLSCOPE_UI_H

#include <SDL3/SDL.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fonts.h"
#include "gpu_renderer.h"

typedef struct {
    float x, y, w, h;
} UiRect;

typedef GpuColor UiColor;

static inline UiRect ui_rect(float x, float y, float w, float h)
{
    UiRect r = {x, y, w, h};
    return r;
}

static inline bool ui_rect_contains(UiRect r, float x, float y)
{
    return x >= r.x && y >= r.y && x <= r.x + r.w && y <= r.y + r.h;
}

static inline UiRect ui_inset(UiRect r, float d)
{
    return ui_rect(r.x + d, r.y + d, r.w - 2 * d, r.h - 2 * d);
}

#define UI_COLOR(r, g, b, a) ((UiColor){(Uint8)(r), (Uint8)(g), (Uint8)(b), (Uint8)(a)})
#define UI_HEX(v) UI_COLOR(((v) >> 16) & 0xFF, ((v) >> 8) & 0xFF, (v) & 0xFF, 255)

extern const UiColor UI_BG_TOP;
extern const UiColor UI_BG_BOTTOM;
extern const UiColor UI_PANEL;
extern const UiColor UI_PANEL_HI;
extern const UiColor UI_BORDER;
extern const UiColor UI_BORDER_SOFT;
extern const UiColor UI_TEXT;
extern const UiColor UI_MUTED;
extern const UiColor UI_FAINT;
extern const UiColor UI_ACCENT;
extern const UiColor UI_ACCENT_SOFT;
extern const UiColor UI_BLUE;
extern const UiColor UI_AMBER;
extern const UiColor UI_RED;
extern const UiColor UI_VIOLET;
extern const UiColor UI_WHITE;
extern const UiColor UI_BLACK;

typedef enum {
    BUTTON_PRIMARY = 0,
    BUTTON_SECONDARY,
    BUTTON_QUIET,
    BUTTON_DANGER
} UiButtonKind;

#define UI_TEXT_CACHE 1024
#define UI_SHAPE_CACHE 256
#define UI_CLIP_STACK 8

typedef struct {
    uint64_t hash;
    int font;
    int px;
    int ax, ay;
    int w, h;
    uint64_t used;
    bool valid;
} UiTextEntry;

typedef struct {
    int w, h, radius;
    int ax, ay;
    uint64_t used;
    bool valid;
} UiShapeEntry;

typedef struct {
    GpuRenderer *r;
    Fonts fonts;
    float scale;
    int win_w, win_h;
    float origin_y;
    int view_h;
    uint64_t frame;

    float mx, my;
    float press_x, press_y;
    bool mouse_down, mouse_pressed, mouse_released;
    float wheel;
    bool block_interactive;
    bool hand_cursor;
    SDL_Cursor *cursor_arrow;
    SDL_Cursor *cursor_hand;

    UiTextEntry text_cache[UI_TEXT_CACHE];
    UiShapeEntry shape_cache[UI_SHAPE_CACHE];

    SDL_Rect clip_stack[UI_CLIP_STACK];
    int clip_depth;
    GpuTexture *bg_gradient;
    char asset_dir[CS_PATH_MAX];
} Ui;

bool ui_init(Ui *ui, GpuRenderer *r, const char *asset_dir, float scale, char *err,
             size_t err_cap);
void ui_shutdown(Ui *ui);
void ui_handle_event(Ui *ui, const SDL_Event *e);
void ui_begin_frame(Ui *ui, int w, int h);
void ui_end_frame(Ui *ui);
void ui_set_viewport(Ui *ui, float origin_y, int view_h);

void ui_background(Ui *ui);
void ui_push_clip(Ui *ui, UiRect r);
void ui_pop_clip(Ui *ui);
void ui_rect_fill(Ui *ui, UiRect r, UiColor c);
void ui_rect_outline(Ui *ui, UiRect r, UiColor c, float thickness);
void ui_round_rect(Ui *ui, UiRect r, float radius, UiColor c);
void ui_circle(Ui *ui, float cx, float cy, float radius, UiColor c);
void ui_line(Ui *ui, float x0, float y0, float x1, float y1, UiColor c, float thickness);
void ui_arc(Ui *ui, float cx, float cy, float radius, float start_deg, float end_deg,
            float thickness, UiColor c);
void ui_panel(Ui *ui, UiRect r, UiColor fill);
void ui_scrollbar(Ui *ui, UiRect track, float scroll, float content, float view);

float ui_text_font(Ui *ui, FontId font, float x, float y, float size, UiColor c,
                   const char *text);
float ui_text(Ui *ui, float x, float y, float size, UiColor c, const char *text);
float ui_textf(Ui *ui, float x, float y, float size, UiColor c, const char *fmt, ...);
float ui_text_width(Ui *ui, FontId font, float size, const char *text);
float ui_text_ellipsis(Ui *ui, FontId font, float x, float y, float size, UiColor c,
                       float max_w, const char *text);
void ui_text_right(Ui *ui, FontId font, float right, float y, float size, UiColor c,
                   const char *text);
void ui_text_center(Ui *ui, FontId font, float cx, float y, float size, UiColor c,
                    const char *text);
float ui_text_wrapped(Ui *ui, float x, float y, float w, float size, UiColor c,
                      float line_gap, const char *text);
float ui_text_wrapped_max(Ui *ui, float x, float y, float w, float size, UiColor c,
                          float line_gap, int max_lines, const char *text);

bool ui_button(Ui *ui, UiRect r, const char *label, UiButtonKind kind, bool enabled);
bool ui_hit(Ui *ui, UiRect r);
bool ui_hover(Ui *ui, UiRect r);
void ui_consume_click(Ui *ui);

#endif
