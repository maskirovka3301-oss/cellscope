#include "ui.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define strtok_r strtok_s
#endif
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

const UiColor UI_BG_TOP = {12, 18, 25, 255};
const UiColor UI_BG_BOTTOM = {7, 10, 14, 255};
const UiColor UI_PANEL = {16, 23, 31, 255};
const UiColor UI_PANEL_HI = {21, 30, 40, 255};
const UiColor UI_BORDER = {35, 49, 64, 255};
const UiColor UI_BORDER_SOFT = {26, 36, 48, 255};
const UiColor UI_TEXT = {234, 241, 248, 255};
const UiColor UI_MUTED = {139, 155, 173, 255};
const UiColor UI_FAINT = {94, 111, 128, 255};
const UiColor UI_ACCENT = {69, 224, 160, 255};
const UiColor UI_ACCENT_SOFT = {30, 109, 87, 255};
const UiColor UI_BLUE = {90, 167, 255, 255};
const UiColor UI_AMBER = {242, 180, 94, 255};
const UiColor UI_RED = {255, 107, 107, 255};
const UiColor UI_VIOLET = {177, 140, 255, 255};
const UiColor UI_WHITE = {255, 255, 255, 255};
const UiColor UI_BLACK = {0, 0, 0, 255};

static Uint8 clamp_u8(int v)
{
    return (Uint8)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

static UiColor col_mix(UiColor a, UiColor b, float t)
{
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    return UI_COLOR(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t,
                    a.a + (b.a - a.a) * t);
}

static UiColor col_alpha(UiColor c, int a)
{
    c.a = clamp_u8(a);
    return c;
}

bool ui_init(Ui *ui, SDL_Renderer *ren, const char *asset_dir, float scale, char *err,
             size_t err_cap)
{
    memset(ui, 0, sizeof(*ui));
    ui->ren = ren;
    ui->scale = scale > 0 ? scale : 1.0f;
    cs_strlcpy(ui->asset_dir, asset_dir ? asset_dir : "", sizeof(ui->asset_dir));
    if (!fonts_init(&ui->fonts, asset_dir, ui->scale, err, err_cap)) return false;

    ui->cursor_arrow = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_DEFAULT);
    ui->cursor_hand = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_POINTER);

    ui->bg_gradient = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC,
                                        1, 256);
    if (ui->bg_gradient) {
        Uint8 pixels[256 * 4];
        for (int i = 0; i < 256; i++) {
            float t = (float)i / 255.0f;
            pixels[i * 4 + 0] = (Uint8)(UI_BG_TOP.r + (UI_BG_BOTTOM.r - UI_BG_TOP.r) * t);
            pixels[i * 4 + 1] = (Uint8)(UI_BG_TOP.g + (UI_BG_BOTTOM.g - UI_BG_TOP.g) * t);
            pixels[i * 4 + 2] = (Uint8)(UI_BG_TOP.b + (UI_BG_BOTTOM.b - UI_BG_TOP.b) * t);
            pixels[i * 4 + 3] = 255;
        }
        SDL_UpdateTexture(ui->bg_gradient, NULL, pixels, 4);
        SDL_SetTextureScaleMode(ui->bg_gradient, SDL_SCALEMODE_LINEAR);
    }

    return true;
}

void ui_shutdown(Ui *ui)
{
    if (!ui) return;
    for (int i = 0; i < UI_TEXT_CACHE; i++) {
        if (ui->text_cache[i].tex) SDL_DestroyTexture(ui->text_cache[i].tex);
    }
    for (int i = 0; i < UI_SHAPE_CACHE; i++) {
        if (ui->shape_cache[i].tex) SDL_DestroyTexture(ui->shape_cache[i].tex);
    }
    if (ui->bg_gradient) SDL_DestroyTexture(ui->bg_gradient);
    if (ui->cursor_arrow) SDL_DestroyCursor(ui->cursor_arrow);
    if (ui->cursor_hand) SDL_DestroyCursor(ui->cursor_hand);
    fonts_done(&ui->fonts);
    memset(ui, 0, sizeof(*ui));
}

void ui_handle_event(Ui *ui, const SDL_Event *e)
{
    if (!ui || !e) return;
    switch (e->type) {
        case SDL_EVENT_MOUSE_MOTION:
            ui->mx = e->motion.x;
            ui->my = e->motion.y;
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (e->button.button == SDL_BUTTON_LEFT) {
                ui->mx = e->button.x;
                ui->my = e->button.y;
                ui->mouse_down = true;
                ui->mouse_pressed = true;
                ui->press_x = e->button.x;
                ui->press_y = e->button.y;
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (e->button.button == SDL_BUTTON_LEFT) {
                ui->mx = e->button.x;
                ui->my = e->button.y;
                ui->mouse_down = false;
                ui->mouse_released = true;
            }
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            ui->wheel += e->wheel.y;
            break;
        default:
            break;
    }
}

void ui_begin_frame(Ui *ui, int w, int h)
{
    ui->win_w = w;
    ui->win_h = h;
    ui->origin_y = 0;
    ui->view_h = h;
    ui->frame++;
    ui->block_interactive = false;
    ui->hand_cursor = false;
    ui->clip_depth = 0;
    SDL_SetRenderDrawBlendMode(ui->ren, SDL_BLENDMODE_BLEND);
}

void ui_set_viewport(Ui *ui, float origin_y, int view_h)
{
    ui->origin_y = origin_y;
    ui->view_h = view_h > 0 ? view_h : ui->win_h;
}

void ui_end_frame(Ui *ui)
{
    if (ui->cursor_hand && ui->hand_cursor) SDL_SetCursor(ui->cursor_hand);
    else SDL_SetCursor(ui->cursor_arrow);
    ui->mouse_pressed = false;
    ui->mouse_released = false;
    ui->wheel = 0;
}

void ui_push_clip(Ui *ui, UiRect r)
{
    SDL_Rect rect = {(int)floorf(r.x), (int)floorf(r.y + ui->origin_y),
                     (int)ceilf(r.w), (int)ceilf(r.h)};
    if (ui->clip_depth > 0) {
        SDL_Rect *prev = &ui->clip_stack[ui->clip_depth - 1];
        SDL_Rect out;
        if (!SDL_GetRectIntersection(prev, &rect, &out)) out = (SDL_Rect){0, 0, 0, 0};
        rect = out;
    }
    if (ui->clip_depth < UI_CLIP_STACK) ui->clip_stack[ui->clip_depth++] = rect;
    SDL_SetRenderClipRect(ui->ren, &rect);
}

void ui_pop_clip(Ui *ui)
{
    if (ui->clip_depth <= 0) return;
    ui->clip_depth--;
    if (ui->clip_depth == 0) SDL_SetRenderClipRect(ui->ren, NULL);
    else SDL_SetRenderClipRect(ui->ren, &ui->clip_stack[ui->clip_depth - 1]);
}

void ui_background(Ui *ui)
{
    if (ui->bg_gradient) {
        SDL_FRect dst = {0, 0, (float)ui->win_w, (float)ui->win_h};
        SDL_SetTextureColorMod(ui->bg_gradient, 255, 255, 255);
        SDL_SetTextureAlphaMod(ui->bg_gradient, 255);
        SDL_RenderTexture(ui->ren, ui->bg_gradient, NULL, &dst);
    } else {
        SDL_SetRenderDrawColor(ui->ren, UI_BG_TOP.r, UI_BG_TOP.g, UI_BG_TOP.b, 255);
        SDL_RenderClear(ui->ren);
    }
}

void ui_rect_fill(Ui *ui, UiRect r, UiColor c)
{
    SDL_SetRenderDrawColor(ui->ren, c.r, c.g, c.b, c.a);
    SDL_FRect fr = {r.x, r.y + ui->origin_y, r.w, r.h};
    SDL_RenderFillRect(ui->ren, &fr);
}

void ui_rect_outline(Ui *ui, UiRect r, UiColor c, float thickness)
{
    SDL_SetRenderDrawColor(ui->ren, c.r, c.g, c.b, c.a);
    for (int i = 0; i < (int)thickness; i++) {
        SDL_FRect fr = {r.x + i, r.y + ui->origin_y + i, r.w - 2 * i, r.h - 2 * i};
        SDL_RenderRect(ui->ren, &fr);
    }
}

void ui_line(Ui *ui, float x0, float y0, float x1, float y1, UiColor c, float thickness)
{
    SDL_SetRenderDrawColor(ui->ren, c.r, c.g, c.b, c.a);
    SDL_SetRenderDrawBlendMode(ui->ren, SDL_BLENDMODE_BLEND);
    float oy = ui->origin_y;
    SDL_RenderLine(ui->ren, x0, y0 + oy, x1, y1 + oy);
    if (thickness > 1.1f) {
        float dx = x1 - x0, dy = y1 - y0;
        float len = sqrtf(dx * dx + dy * dy);
        if (len > 0.01f) {
            float nx = -dy / len * (thickness - 1) * 0.5f;
            float ny = dx / len * (thickness - 1) * 0.5f;
            SDL_RenderLine(ui->ren, x0 + nx, y0 + oy + ny, x1 + nx, y1 + oy + ny);
            SDL_RenderLine(ui->ren, x0 - nx, y0 + oy - ny, x1 - nx, y1 + oy - ny);
        }
    }
}

void ui_arc(Ui *ui, float cx, float cy, float radius, float start_deg, float end_deg,
            float thickness, UiColor c)
{
    int steps = (int)(fabsf(end_deg - start_deg) / 5.0f) + 2;
    float px = 0, py = 0;
    for (int i = 0; i <= steps; i++) {
        float t = (float)i / steps;
        float a = (start_deg + (end_deg - start_deg) * t) * (float)M_PI / 180.0f;
        float x = cx + cosf(a) * radius;
        float y = cy + sinf(a) * radius;
        if (i > 0) ui_line(ui, px, py, x, y, c, thickness);
        px = x;
        py = y;
    }
}

static UiShapeEntry *shape_get(Ui *ui, int w, int h, int radius)
{
    uint64_t now = ui->frame;
    UiShapeEntry *oldest = &ui->shape_cache[0];
    for (int i = 0; i < UI_SHAPE_CACHE; i++) {
        UiShapeEntry *e = &ui->shape_cache[i];
        if (e->tex && e->w == w && e->h == h && e->radius == radius) {
            e->used = now;
            return e;
        }
        if (e->used < oldest->used) oldest = e;
    }
    UiShapeEntry *e = oldest;
    if (e->tex) {
        SDL_DestroyTexture(e->tex);
        e->tex = NULL;
    }
    if (w <= 0 || h <= 0) return NULL;
    SDL_Surface *surf = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA32);
    if (!surf) return NULL;
    Uint8 *pixels = surf->pixels;
    int pitch = surf->pitch;
    float r = (float)radius;
    if (r > w / 2.0f) r = w / 2.0f;
    if (r > h / 2.0f) r = h / 2.0f;
    float cx = w / 2.0f, cy = h / 2.0f;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float px = x + 0.5f, py = y + 0.5f;
            float qx = fabsf(px - cx) - (w / 2.0f - r);
            float qy = fabsf(py - cy) - (h / 2.0f - r);
            if (qx < 0) qx = 0;
            if (qy < 0) qy = 0;
            float d = sqrtf(qx * qx + qy * qy) - r;
            float cov = 0.5f - d;
            if (cov < 0) cov = 0;
            if (cov > 1) cov = 1;
            Uint8 *p = pixels + (size_t)y * pitch + (size_t)x * 4;
            p[0] = 255;
            p[1] = 255;
            p[2] = 255;
            p[3] = (Uint8)(cov * 255);
        }
    }
    e->tex = SDL_CreateTextureFromSurface(ui->ren, surf);
    SDL_DestroySurface(surf);
    e->w = w;
    e->h = h;
    e->radius = radius;
    e->used = now;
    if (e->tex) SDL_SetTextureBlendMode(e->tex, SDL_BLENDMODE_BLEND);
    return e;
}

void ui_round_rect(Ui *ui, UiRect r, float radius, UiColor c)
{
    if (r.w <= 0 || r.h <= 0) return;
    int pw = (int)ceilf(r.w * ui->scale);
    int ph = (int)ceilf(r.h * ui->scale);
    int pr = (int)ceilf(radius * ui->scale);
    UiShapeEntry *e = shape_get(ui, pw, ph, pr);
    if (!e || !e->tex) return;
    SDL_SetTextureColorMod(e->tex, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(e->tex, c.a);
    SDL_FRect dst = {r.x, r.y + ui->origin_y, r.w, r.h};
    SDL_RenderTexture(ui->ren, e->tex, NULL, &dst);
    SDL_SetTextureColorMod(e->tex, 255, 255, 255);
    SDL_SetTextureAlphaMod(e->tex, 255);
}


void ui_circle(Ui *ui, float cx, float cy, float radius, UiColor c)
{
    ui_round_rect(ui, ui_rect(cx - radius, cy - radius, radius * 2, radius * 2), radius, c);
}

void ui_panel(Ui *ui, UiRect r, UiColor fill)
{
    ui_round_rect(ui, r, 12, UI_BORDER);
    ui_round_rect(ui, ui_inset(r, 1), 11, fill);
}

static uint64_t hash_text(const char *s)
{
    uint64_t h = 1469598103934665603ULL;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        h ^= *p;
        h *= 1099511628211ULL;
    }
    return h;
}

static int utf8_next(const char **pp)
{
    const unsigned char *p = (const unsigned char *)*pp;
    if (!*p) return 0;
    int cp = 0, len = 1;
    if (p[0] < 0x80) {
        cp = p[0];
    } else if ((p[0] & 0xE0) == 0xC0 && p[1]) {
        cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F);
        len = 2;
    } else if ((p[0] & 0xF0) == 0xE0 && p[1] && p[2]) {
        cp = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        len = 3;
    } else if ((p[0] & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) {
        cp = ((p[0] & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) |
             (p[3] & 0x3F);
        len = 4;
    } else {
        cp = '?';
    }
    *pp += len;
    return cp;
}

static UiTextEntry *text_get(Ui *ui, FontId font, float size, const char *text)
{
    int px = (int)(size * ui->scale + 0.5f);
    if (px < 5) px = 5;
    uint64_t h = hash_text(text) ^ ((uint64_t)font << 56) ^ ((uint64_t)px << 40);
    UiTextEntry *oldest = &ui->text_cache[0];
    for (int i = 0; i < UI_TEXT_CACHE; i++) {
        UiTextEntry *e = &ui->text_cache[i];
        if (e->tex && e->hash == h && e->font == (int)font && e->px == px) {
            e->used = ui->frame;
            return e;
        }
        if (e->used < oldest->used) oldest = e;
    }

    if (!fonts_set_size(&ui->fonts, font, size)) return NULL;
    FT_Face face = ui->fonts.face[font];
    int asc = (int)(face->size->metrics.ascender >> 6);
    int desc = (int)(face->size->metrics.descender >> 6);
    int line_h = asc - desc;
    int pen = 4;
    const char *p = text;
    while (*p) {
        int cp = utf8_next(&p);
        if (FT_Load_Char(face, (FT_ULong)cp, FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT) != 0)
            continue;
        pen += (int)(face->glyph->advance.x >> 6);
    }
    int tex_w = pen + 4;
    int tex_h = line_h + 8;
    if (tex_w < 1 || tex_h < 1) return NULL;

    Uint8 *buf = calloc((size_t)tex_w * tex_h, 4);
    if (!buf) return NULL;
    int baseline = asc + 4;
    int x = 4;
    p = text;
    while (*p) {
        int cp = utf8_next(&p);
        if (FT_Load_Char(face, (FT_ULong)cp, FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT) != 0)
            continue;
        FT_GlyphSlot g = face->glyph;
        int gx = x + g->bitmap_left;
        int gy = baseline - g->bitmap_top;
        for (unsigned row = 0; row < g->bitmap.rows; row++) {
            for (unsigned col = 0; col < g->bitmap.width; col++) {
                Uint8 a = g->bitmap.buffer[row * g->bitmap.pitch + col];
                if (!a) continue;
                int px_x = gx + (int)col;
                int px_y = gy + (int)row;
                if (px_x < 0 || px_y < 0 || px_x >= tex_w || px_y >= tex_h) continue;
                Uint8 *d = buf + ((size_t)px_y * tex_w + px_x) * 4;
                if (a > d[3]) d[3] = a;
                d[0] = 255;
                d[1] = 255;
                d[2] = 255;
            }
        }
        x += (int)(g->advance.x >> 6);
    }

    UiTextEntry *e = oldest;
    if (e->tex) SDL_DestroyTexture(e->tex);
    e->tex = SDL_CreateTexture(ui->ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, tex_w,
                               tex_h);
    if (e->tex) {
        SDL_UpdateTexture(e->tex, NULL, buf, tex_w * 4);
        SDL_SetTextureBlendMode(e->tex, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(e->tex, SDL_SCALEMODE_LINEAR);
    }
    free(buf);
    e->hash = h;
    e->font = (int)font;
    e->px = px;
    e->w = tex_w;
    e->h = tex_h;
    e->used = ui->frame;
    return e->tex ? e : NULL;
}

float ui_text_font(Ui *ui, FontId font, float x, float y, float size, UiColor c,
                   const char *text)
{
    if (!text || !*text) return 0;
    UiTextEntry *e = text_get(ui, font, size, text);
    if (!e) return 0;
    float w = (float)(e->w - 8) / ui->scale;
    float h = (float)e->h / ui->scale;
    SDL_SetTextureColorMod(e->tex, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(e->tex, c.a);
    SDL_FRect dst = {x - 4.0f / ui->scale, y + ui->origin_y,
                     (float)e->w / ui->scale, h};
    SDL_RenderTexture(ui->ren, e->tex, NULL, &dst);
    SDL_SetTextureColorMod(e->tex, 255, 255, 255);
    SDL_SetTextureAlphaMod(e->tex, 255);
    return w;
}

float ui_text(Ui *ui, float x, float y, float size, UiColor c, const char *text)
{
    return ui_text_font(ui, FONT_UI, x, y, size, c, text);
}

float ui_textf(Ui *ui, float x, float y, float size, UiColor c, const char *fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return ui_text(ui, x, y, size, c, buf);
}

float ui_text_width(Ui *ui, FontId font, float size, const char *text)
{
    if (!text || !*text) return 0;
    UiTextEntry *e = text_get(ui, font, size, text);
    if (!e) return 0;
    return (float)(e->w - 8) / ui->scale;
}

void ui_text_right(Ui *ui, FontId font, float right, float y, float size, UiColor c,
                   const char *text)
{
    float w = ui_text_width(ui, font, size, text);
    ui_text_font(ui, font, right - w, y, size, c, text);
}

void ui_text_center(Ui *ui, FontId font, float cx, float y, float size, UiColor c,
                    const char *text)
{
    float w = ui_text_width(ui, font, size, text);
    ui_text_font(ui, font, cx - w / 2, y, size, c, text);
}

float ui_text_ellipsis(Ui *ui, FontId font, float x, float y, float size, UiColor c,
                       float max_w, const char *text)
{
    if (!text) return 0;
    if (ui_text_width(ui, font, size, text) <= max_w || max_w <= 0)
        return ui_text_font(ui, font, x, y, size, c, text);
    char buf[512];
    cs_strlcpy(buf, text, sizeof(buf));
    size_t n = strlen(buf);
    while (n > 0) {
        n--;
        while (n > 0 && ((unsigned char)buf[n] & 0xC0) == 0x80) n--;
        buf[n] = '\0';
        char with_dots[520];
        snprintf(with_dots, sizeof(with_dots), "%s…", buf);
        if (ui_text_width(ui, font, size, with_dots) <= max_w) {
            return ui_text_font(ui, font, x, y, size, c, with_dots);
        }
    }
    return 0;
}

float ui_text_wrapped_max(Ui *ui, float x, float y, float w, float size, UiColor c,
                          float line_gap, int max_lines, const char *text)
{
    if (!text) return 0;
    int lines = 0;
    float line_h = fonts_line_height(&ui->fonts, FONT_UI, size);
    const char *p = text;
    char line[1024] = {0};
    float yy = y;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t seg_len = eol ? (size_t)(eol - p) : strlen(p);
        char seg[1024];
        if (seg_len >= sizeof(seg)) seg_len = sizeof(seg) - 1;
        memcpy(seg, p, seg_len);
        seg[seg_len] = '\0';

        char *save = NULL;
        char *word = strtok_r(seg, " ", &save);
        line[0] = '\0';
        while (word) {
            char trial[1024];
            if (line[0]) snprintf(trial, sizeof(trial), "%s %s", line, word);
            else cs_strlcpy(trial, word, sizeof(trial));
            if (line[0] && ui_text_width(ui, FONT_UI, size, trial) > w) {
                if (max_lines > 0 && lines >= max_lines - 1) {
                    ui_text_ellipsis(ui, FONT_UI, x, yy, size, c, w, line);
                    return yy + line_h - y;
                }
                ui_text(ui, x, yy, size, c, line);
                lines++;
                yy += line_h + line_gap;
                cs_strlcpy(line, word, sizeof(line));
            } else {
                cs_strlcpy(line, trial, sizeof(line));
            }
            word = strtok_r(NULL, " ", &save);
        }
        if (line[0]) {
            ui_text(ui, x, yy, size, c, line);
            lines++;
            yy += line_h + line_gap;
        }
        if (max_lines > 0 && lines >= max_lines) break;
        if (eol) p = eol + 1;
        else break;
        if (!*p) break;
    }
    return yy - y;
}

float ui_text_wrapped(Ui *ui, float x, float y, float w, float size, UiColor c,
                      float line_gap, const char *text)
{
    return ui_text_wrapped_max(ui, x, y, w, size, c, line_gap, 0, text);
}

bool ui_hover(Ui *ui, UiRect r)
{
    if (ui->block_interactive) return false;
    bool hit = ui_rect_contains(r, ui->mx, ui->my - ui->origin_y);
    if (hit) ui->hand_cursor = true;
    return hit;
}

void ui_consume_click(Ui *ui)
{
    ui->mouse_pressed = false;
}

bool ui_button(Ui *ui, UiRect r, const char *label, UiButtonKind kind, bool enabled)
{
    bool hover = enabled && ui_hover(ui, r);
    bool active = hover && ui->mouse_down;
    float dy = active ? 1.0f : 0.0f;
    UiRect rr = ui_rect(r.x, r.y + dy, r.w, r.h);
    UiColor bg, fg, border;
    switch (kind) {
        case BUTTON_PRIMARY:
            bg = hover ? col_mix(UI_ACCENT, UI_WHITE, 0.08f) : UI_ACCENT;
            fg = UI_COLOR(7, 18, 14, 255);
            border = col_alpha(UI_ACCENT, 160);
            if (!enabled) bg = col_alpha(UI_ACCENT, 55);
            break;
        case BUTTON_DANGER:
            bg = hover ? col_mix(UI_RED, UI_WHITE, 0.06f) : UI_RED;
            fg = UI_WHITE;
            border = col_alpha(UI_RED, 180);
            break;
        case BUTTON_SECONDARY:
            bg = hover ? UI_PANEL_HI : UI_COLOR(22, 31, 42, 255);
            fg = UI_TEXT;
            border = hover ? col_mix(UI_BORDER, UI_TEXT, 0.08f) : UI_BORDER;
            break;
        default:
            bg = hover ? UI_PANEL_HI : UI_COLOR(0, 0, 0, 0);
            fg = hover ? UI_TEXT : UI_MUTED;
            border = UI_COLOR(0, 0, 0, 0);
            break;
    }
    if (!enabled) fg = col_alpha(fg, 100);
    if (kind == BUTTON_PRIMARY && enabled) {
        ui_round_rect(ui, ui_rect(r.x - 3, r.y - 3, r.w + 6, r.h + 6), 11,
                      col_alpha(UI_ACCENT, 26));
    }
    if (border.a) ui_round_rect(ui, rr, 8, border);
    ui_round_rect(ui, ui_inset(rr, border.a ? 1 : 0), 7, bg);
    float th = fonts_line_height(&ui->fonts, FONT_SEMIBOLD, 12.5f);
    ui_text_center(ui, FONT_SEMIBOLD, rr.x + rr.w / 2, rr.y + (rr.h - th) / 2 + 1, 12.5f, fg,
                   label);
    if (hover && enabled && ui->mouse_released &&
        ui_rect_contains(r, ui->press_x, ui->press_y - ui->origin_y)) {
        ui->mouse_released = false;
        ui->mouse_pressed = false;
        return true;
    }
    return false;
}

void ui_scrollbar(Ui *ui, UiRect track, float scroll, float content, float view)
{
    if (content <= view + 1) return;
    ui_round_rect(ui, track, track.w / 2, UI_COLOR(26, 36, 48, 160));
    float frac = view / content;
    float th = CS_MAX(28.0f, track.h * frac);
    float max_scroll = content - view;
    float t = max_scroll > 0 ? scroll / max_scroll : 0;
    float ty = track.y + (track.h - th) * t;
    ui_round_rect(ui, ui_rect(track.x, ty, track.w, th), track.w / 2,
                  UI_COLOR(77, 96, 116, 210));
}
