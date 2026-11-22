#include "app.h"
#include "camera_platform.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define strtok_r strtok_s
#endif

#define TABBAR_H 48.0f

typedef struct {
    AdbStatus adb;
    ReaderView reader;
    int cell_count;
    int meter_count;
    bool modal_open;
    bool modal_checking;
    bool remove_meter_open;
    int remove_meter_index;
    bool settings_modal_open;
    bool upload_modal_open;
    bool model_modal_open;
    bool paused;
    int active_tab;
    bool ever_connected;
    char device_label[128];
    int64_t last_poll_ok_ms;
    RuntimeView runtime;
} UiState;

typedef struct {
    UiRect header;
    UiRect content;
    UiRect camera;
    UiRect cells;
    UiRect meters;
    UiRect footer;
} Layout;

static UiColor kind_color(VlmBackend kind)
{
    switch (kind) {
        case VLM_BACKEND_OLLAMA:
        case VLM_BACKEND_LLAMA: return UI_ACCENT;
        case VLM_BACKEND_MOCK: return UI_VIOLET;
        case VLM_BACKEND_OFF: return UI_FAINT;
        default: return UI_MUTED;
    }
}

static void snapshot(App *app, UiState *s)
{
    memset(s, 0, sizeof(*s));
    SDL_LockMutex(app->mu);
    s->adb = app->adb;
    s->reader = app->reader;
    s->cell_count = app->cell_count;
    s->meter_count = app->meter_count;
    s->modal_open = app->modal_open;
    s->modal_checking = app->modal_checking;
    s->remove_meter_open = app->remove_meter_open;
    s->remove_meter_index = app->remove_meter_index;
    s->settings_modal_open = app->settings_modal_open;
    s->upload_modal_open = app->upload_modal_open;
    s->model_modal_open = app->model_modal_open;
    s->paused = app->paused;
    s->active_tab = app->active_tab;
    s->ever_connected = app->ever_connected;
    cs_strlcpy(s->device_label, app->device_label, sizeof(s->device_label));
    s->last_poll_ok_ms = app->last_poll_ok_ms;
    s->runtime = app->runtime_view;
    SDL_UnlockMutex(app->mu);
}

static void layout(App *app, Layout *l)
{
    int w = app->ui.win_w, h = app->ui.view_h;
    float pad = 18, gap = 14;
    l->header = ui_rect(0, 0, (float)w, 64);
    l->footer = ui_rect(0, (float)h - 28, (float)w, 28);
    float y = 80 + app->ui_banner_h;
    float avail_h = (float)h - y - 28 - pad;
    float left_w = ((float)w - 2 * pad - gap) * 0.43f;
    float right_w = (float)w - 2 * pad - gap - left_w;
    float half_h = (avail_h - gap) * 0.5f;
    l->content = ui_rect(pad, y, (float)w - 2 * pad, avail_h);
    l->cells = ui_rect(pad, y, left_w, half_h);
    l->meters = ui_rect(pad, y + half_h + gap, left_w, avail_h - half_h - gap);
    l->camera = ui_rect(pad + left_w + gap, y, right_w, avail_h);
}

static void format_mhz_reading(double hz, char *out, size_t cap)
{
    if (hz <= 0.0) {
        cs_strlcpy(out, "—", cap);
        return;
    }
    snprintf(out, cap, "%.4f MHz", hz / 1e6);
}

static void draw_dot(Ui *ui, float x, float y, UiColor c)
{
    ui_circle(ui, x, y, 3.2f, c);
}

static void draw_chip(Ui *ui, float x, float y, const char *text, UiColor fg, UiColor bg)
{
    float tw = ui_text_width(ui, FONT_SEMIBOLD, 9.2f, text);
    UiRect r = ui_rect(x, y, tw + 14, 16);
    ui_round_rect(ui, r, 8, bg);
    ui_text_font(ui, FONT_SEMIBOLD, x + 7, y + 2.2f, 9.2f, fg, text);
}

static void draw_logo(Ui *ui, float x, float y)
{
    ui_round_rect(ui, ui_rect(x, y, 32, 32), 9, UI_ACCENT);
    UiColor ink = UI_COLOR(7, 18, 14, 255);
    for (int i = 0; i < 5; i++) {
        float hgt = (i == 2 ? 16.0f : (i == 1 || i == 3 ? 11.0f : 6.0f));
        ui_round_rect(ui, ui_rect(x + 7 + i * 4, y + 8 + (16 - hgt) / 2, 2, hgt), 1, ink);
    }
}

static const char *adb_short_state(const UiState *s, char *buf, size_t cap, UiColor *color)
{
    if (s->adb.state == ADB_STATE_CONNECTED) {
        *color = UI_ACCENT;
        cs_strlcpy(buf, "PHONE CONNECTED", cap);
        return buf;
    }
    *color = (s->adb.state == ADB_STATE_NO_TOOL || s->adb.state == ADB_STATE_ERROR)
                  ? UI_RED
                  : UI_MUTED;
    cs_strlcpy(buf, "PHONE DISCONNECTED", cap);
    return buf;
}

static void draw_transport(App *app, UiState *s)
{
    Ui *ui = &app->ui;
    float cx = (float)ui->win_w / 2.0f;
    UiRect btn = ui_rect(cx - 22, 9, 44, 44);
    bool hover = ui_hover(ui, btn);
    UiColor ring = s->paused ? UI_AMBER : UI_ACCENT;
    ui_circle(ui, btn.x + 22, btn.y + 22, 22, ring);
    ui_circle(ui, btn.x + 22, btn.y + 22, 20.5f,
              hover ? UI_COLOR(30, 44, 53, 255) : UI_COLOR(17, 27, 35, 255));
    if (s->paused) {
        ui_line(ui, cx - 4, btn.y + 14, cx + 6, btn.y + 22, UI_ACCENT, 2.4f);
        ui_line(ui, cx + 6, btn.y + 22, cx - 4, btn.y + 30, UI_ACCENT, 2.4f);
        ui_line(ui, cx - 4, btn.y + 30, cx - 4, btn.y + 14, UI_ACCENT, 2.4f);
    } else {
        ui_round_rect(ui, ui_rect(cx - 7, btn.y + 15, 5, 15), 1.5f, UI_TEXT);
        ui_round_rect(ui, ui_rect(cx + 2, btn.y + 15, 5, 15), 1.5f, UI_TEXT);
    }
    if (hover && ui->mouse_pressed) {
        app_toggle_pause(app);
        ui_consume_click(ui);
    }
}

static void draw_runtime_banner(App *app, UiState *s)
{
    if (app->ui_banner_h <= 0 || (!s->runtime.active && !s->runtime.error)) return;
    Ui *ui = &app->ui;
    float y = 80, h = (float)app->ui_banner_h;
    UiColor accent = s->runtime.error ? UI_RED : UI_ACCENT;
    ui_rect_fill(ui, ui_rect(0, y, (float)ui->win_w, h), UI_COLOR(9, 14, 19, 248));
    ui_line(ui, 0, y + h - 0.5f, (float)ui->win_w, y + h - 0.5f, UI_BORDER_SOFT, 1);
    ui_rect_fill(ui, ui_rect(0, y, 3, h), accent);
    if (s->runtime.error) {
        ui_circle(ui, 22, y + h / 2, 8, UI_COLOR(58, 31, 32, 255));
        ui_text_center(ui, FONT_BOLD, 22, y + h / 2 - 7, 10, UI_RED, "!");
    } else {
        float a = (float)((SDL_GetTicks() % 900) / 900.0 * 360.0);
        ui_arc(ui, 22, y + h / 2, 8, a, a + 280, 2.0f, UI_ACCENT);
    }
    char msg[640];
    snprintf(msg, sizeof(msg), "%s%s%s", s->runtime.message[0] ? s->runtime.message : "Local AI",
             s->runtime.error && s->runtime.error_text[0] ? " · " : "",
             s->runtime.error ? s->runtime.error_text : "");
    ui_text_ellipsis(ui, FONT_SEMIBOLD, 38, y + 8, 10.8f,
                     s->runtime.error ? UI_COLOR(255, 178, 178, 255) : UI_TEXT,
                     (float)ui->win_w - 300, msg);
    char pct[32];
    if (!s->runtime.error) {
        snprintf(pct, sizeof(pct), "%d%%", s->runtime.percent);
        ui_text_right(ui, FONT_MONO, (float)ui->win_w - 90, y + 8, 9.5f, UI_MUTED, pct);
        UiRect bar = ui_rect(38, y + 28, (float)ui->win_w - 190, 3);
        ui_round_rect(ui, bar, 1.5f, UI_COLOR(30, 42, 54, 255));
        float frac = CS_MAX(0.0f, CS_MIN(1.0f, s->runtime.percent / 100.0f));
        ui_round_rect(ui, ui_rect(bar.x, bar.y, bar.w * frac, bar.h), 1.5f, UI_ACCENT);
    } else {
        UiRect retry = ui_rect((float)ui->win_w - 92, y + 7, 74, 24);
        if (ui_button(ui, retry, "Retry", BUTTON_SECONDARY, true)) app_retry_runtime(app);
    }
}

static void draw_header(App *app, UiState *s)
{
    Ui *ui = &app->ui;
    float w = (float)ui->win_w;

    ui_line(ui, 0, 63.5f, w, 63.5f, UI_BORDER_SOFT, 1);
    draw_logo(ui, 18, 16);
    ui_text_font(ui, FONT_BOLD, 60, 12, 15.5f, UI_TEXT, "CELLSCOPE");
    ui_text_font(ui, FONT_MEDIUM, 60, 33, 9.2f, UI_FAINT,
                 "LETS PROVE DIRECTED ENERGY TOGETHER!");

    char adb_buf[160];
    UiColor adb_col = UI_MUTED;
    adb_short_state(s, adb_buf, sizeof(adb_buf), &adb_col);
    bool checking = s->modal_checking && s->adb.state != ADB_STATE_CONNECTED;

    float pill_h = 26, gap = 8;
    float x = w - 18;

    /* Reader pill */
    char reader_text[128];
    cs_strlcpy(reader_text, s->reader.backend[0] ? s->reader.backend : "READER",
               sizeof(reader_text));
    float rw = CS_MIN(190.0f, ui_text_width(ui, FONT_MEDIUM, 10.2f, reader_text) + 28);
    UiRect rp = ui_rect(x - rw, 18, rw, pill_h);
    ui_round_rect(ui, rp, 13, UI_PANEL);
    ui_round_rect(ui, ui_inset(rp, 1), 12, UI_COLOR(13, 19, 26, 255));
    draw_dot(ui, rp.x + 12, rp.y + 13, kind_color(s->reader.kind));
    ui_text_ellipsis(ui, FONT_MEDIUM, rp.x + 20, rp.y + 7, 10.2f, UI_MUTED, rp.w - 28,
                     reader_text);
    if (ui_hover(ui, rp) && ui->mouse_pressed) {
        app_open_model_modal(app);
        ui_consume_click(ui);
    }
    x -= rw + gap;

    /* Camera pill */
    char cam_text[128];
    UiColor cam_col = UI_FAINT;
    if (s->paused) {
        cs_strlcpy(cam_text, "PAUSED", sizeof(cam_text));
        cam_col = UI_AMBER;
    } else if (app->demo) {
        cs_strlcpy(cam_text, "DEMO FEED", sizeof(cam_text));
        cam_col = UI_VIOLET;
    } else if (app->image_mode) {
        cs_strlcpy(cam_text, app->readme_fixture ? "MacBook Pro Camera" : "TEST IMAGE",
                   sizeof(cam_text));
        cam_col = app->readme_fixture ? UI_ACCENT : UI_BLUE;
    } else if (app->cam.permission_pending) {
        cs_strlcpy(cam_text, "CAMERA PERMISSION", sizeof(cam_text));
        cam_col = UI_AMBER;
    } else if (app->cam.permission_denied) {
        cs_strlcpy(cam_text, "CAMERA BLOCKED", sizeof(cam_text));
        cam_col = UI_RED;
    } else if (app->camera_ok && app->cam.active) {
        snprintf(cam_text, sizeof(cam_text), "%.22s", app->cam.name);
        cam_col = UI_ACCENT;
    } else {
        cs_strlcpy(cam_text, "NO CAMERA", sizeof(cam_text));
    }
    float cw = ui_text_width(ui, FONT_MEDIUM, 10.2f, cam_text) + 28;
    UiRect cp = ui_rect(x - cw, 18, cw, pill_h);
    ui_round_rect(ui, cp, 13, UI_PANEL);
    ui_round_rect(ui, ui_inset(cp, 1), 12, UI_COLOR(13, 19, 26, 255));
    draw_dot(ui, cp.x + 12, cp.y + 13, cam_col);
    ui_text_ellipsis(ui, FONT_MEDIUM, cp.x + 20, cp.y + 7, 10.2f, UI_MUTED, cp.w - 28,
                     cam_text);
    if (ui_hover(ui, cp) && ui->mouse_pressed) {
        if (app->camera_ok && app->cam.count > 1) {
            if (camera_switch(&app->cam)) app_notify(app, false, "Camera: %s", app->cam.name);
            else app_notify(app, true, "Camera switch failed: %s", app->cam.error);
        } else {
            app_notify(app, false, "%s", app->camera_ok ? app->cam.name : app->camera_error);
        }
        ui_consume_click(ui);
    }
    x -= cw + gap;

    /* ADB pill */
    float aw = ui_text_width(ui, FONT_MEDIUM, 10.2f, adb_buf) + 28;
    UiRect ap = ui_rect(x - aw, 18, aw, pill_h);
    ui_round_rect(ui, ap, 13, UI_PANEL);
    ui_round_rect(ui, ui_inset(ap, 1), 12, UI_COLOR(13, 19, 26, 255));
    if (checking) {
        float a = (float)((SDL_GetTicks() % 1000) / 1000.0 * 360.0);
        ui_arc(ui, ap.x + 12, ap.y + 13, 5, a, a + 260, 1.6f, adb_col);
    } else {
        draw_dot(ui, ap.x + 12, ap.y + 13, adb_col);
    }
    ui_text_ellipsis(ui, FONT_MEDIUM, ap.x + 20, ap.y + 7, 10.2f, UI_MUTED, ap.w - 28,
                     checking ? "CHECKING…" : adb_buf);
    if (ui_hover(ui, ap) && ui->mouse_pressed) {
        if (s->adb.state == ADB_STATE_CONNECTED) {
            app_notify(app, false, "Connected: %s · %s", s->device_label, s->adb.serial);
        } else {
            app_request_check(app);
        }
        ui_consume_click(ui);
    }
}

static void draw_guide(Ui *ui, UiRect preview, const UiState *s)
{
    float gw = preview.w * 0.72f;
    float gh = preview.h * 0.50f;
    UiRect g = ui_rect(preview.x + (preview.w - gw) / 2, preview.y + (preview.h - gh) / 2, gw,
                       gh);
    UiColor col = s->reader.screen_visible ? UI_ACCENT
                                           : UI_COLOR(180, 220, 205, 155);
    float len = 20, th = 2;
    /* Four corner brackets. */
    ui_line(ui, g.x, g.y, g.x + len, g.y, col, th);
    ui_line(ui, g.x, g.y, g.x, g.y + len, col, th);
    ui_line(ui, g.x + g.w, g.y, g.x + g.w - len, g.y, col, th);
    ui_line(ui, g.x + g.w, g.y, g.x + g.w, g.y + len, col, th);
    ui_line(ui, g.x, g.y + g.h, g.x + len, g.y + g.h, col, th);
    ui_line(ui, g.x, g.y + g.h, g.x, g.y + g.h - len, col, th);
    ui_line(ui, g.x + g.w, g.y + g.h, g.x + g.w - len, g.y + g.h, col, th);
    ui_line(ui, g.x + g.w, g.y + g.h, g.x + g.w, g.y + g.h - len, col, th);
    ui_text_center(ui, FONT_SEMIBOLD, g.x + g.w / 2, g.y + g.h + 8, 8.5f,
                   UI_COLOR(180, 220, 205, 175),
                   "HOLD THE METER SCREEN IN THE GUIDE · MOVE CLOSER IF SMALL");
}

static void draw_demo_feed(App *app, UiRect preview)
{
    Ui *ui = &app->ui;
    ui_rect_fill(ui, preview, UI_COLOR(6, 10, 15, 255));
    for (int i = 1; i < 8; i++) {
        float yy = preview.y + preview.h * i / 8;
        ui_line(ui, preview.x, yy, preview.x + preview.w, yy, UI_COLOR(18, 30, 38, 90), 1);
    }
    float dw = preview.w * 0.72f;
    float dh = CS_MIN(preview.h * 0.64f, 230.0f);
    UiRect d = ui_rect(preview.x + (preview.w - dw) / 2, preview.y + (preview.h - dh) / 2, dw,
                       dh);
    ui_round_rect(ui, d, 10, UI_COLOR(24, 34, 42, 255));
    ui_round_rect(ui, ui_inset(d, 1), 9, UI_COLOR(10, 16, 20, 255));
    ui_round_rect(ui, ui_inset(d, 1), 9, UI_COLOR(9, 15, 19, 255));

    ui_text_font(ui, FONT_SEMIBOLD, d.x + 14, d.y + 10, 8.0f, UI_COLOR(120, 150, 160, 220),
                 "FREQUENCY COUNTER");
    ui_textf(ui, d.x + 14, d.y + 21, 7.2f, UI_COLOR(90, 115, 125, 200),
             "10 s GATE · EXT REF · CH A");
    ui_circle(ui, d.x + d.w - 16, d.y + 14, 3.2f, UI_ACCENT);

    double hz = vlm_demo_hz(cs_epoch_ms());
    char num[64], unit[16];
    snprintf(num, sizeof(num), "%.7f", hz / 1e9);
    cs_strlcpy(unit, "GHz", sizeof(unit));
    float fsize = CS_MIN(44.0f, d.w / 6.5f);
    float tw = ui_text_width(ui, FONT_MONO, fsize, num);
    float bx = d.x + (d.w - tw) / 2;
    float by = d.y + dh * 0.37f;
    ui_text_font(ui, FONT_MONO, bx + 1, by + 1, fsize, UI_COLOR(30, 140, 100, 90), num);
    ui_text_font(ui, FONT_MONO, bx, by, fsize, UI_COLOR(115, 255, 190, 255), num);
    ui_text_font(ui, FONT_MEDIUM, bx + tw + 6, by + fsize * 0.62f, 11.0f,
                 UI_COLOR(140, 190, 165, 230), unit);

    float ly = d.y + dh - 25;
    ui_line(ui, d.x + 14, ly, d.x + d.w - 14, ly, UI_COLOR(34, 70, 60, 160), 1);
    ui_text_font(ui, FONT_MONO, d.x + 14, ly + 5, 8.0f, UI_COLOR(90, 150, 125, 220),
                 "AUTO");
    ui_text_font(ui, FONT_MONO, d.x + 58, ly + 5, 8.0f, UI_COLOR(90, 150, 125, 220),
                 "HOLD");
    ui_text_font(ui, FONT_MONO, d.x + 100, ly + 5, 8.0f, UI_COLOR(90, 150, 125, 220),
                 "TRIG");
    ui_text_font(ui, FONT_MONO, d.x + d.w - 60, ly + 5, 8.0f,
                 UI_COLOR(90, 150, 125, 220), "OVEN");
    /* waveform */
    float wx = d.x + d.w * 0.58f, wy = ly - 8;
    for (int i = 0; i < 36; i++) {
        float x0 = wx + i * 4.0f;
        float y0 = wy + sinf((float)i * 0.55f + (float)SDL_GetTicks() * 0.002f) * 5.0f;
        float x1 = wx + (i + 1) * 4.0f;
        float y1 = wy + sinf((float)(i + 1) * 0.55f + (float)SDL_GetTicks() * 0.002f) * 5.0f;
        ui_line(ui, x0, y0, x1, y1, UI_COLOR(70, 180, 140, 190), 1);
    }
}

static void copy_reader_setup(App *app)
{
    SDL_SetClipboardText(
        "Ollama VLM:\n"
        "  ollama pull moondream && ollama serve\n"
        "  cellscope --vlm ollama --vlm-model moondream\n\n"
        "llama.cpp + SmolVLM-256M:\n"
        "  llama-server -hf ggml-org/SmolVLM-256M-Instruct-GGUF --port 8080\n"
        "  cellscope --vlm llama --vlm-url http://127.0.0.1:8080\n\n"
        "Train your own meter reader:\n"
        "  cellscope --train ./meter_dataset --train-out ./training\n"
        "  python scripts/serve_minimind_vlm.py --minimind third_party/minimind-v "
        "--save_dir ./training --weight cellscope_meter --port 8090\n"
        "  cellscope --vlm llama --vlm-url http://127.0.0.1:8090\n");
    app_notify(app, false, "Reader setup commands copied to the clipboard.");
}

static void open_capture_folder(App *app)
{
    const char *dir = app->session_dir[0] ? app->session_dir : app->capture_dir;
    char url[CS_PATH_MAX + 16];
    snprintf(url, sizeof(url), "file://%s", dir);
    if (!SDL_OpenURL(url)) app_notify(app, true, "Could not open %s", dir);
}

static void draw_camera_panel(App *app, UiState *s, UiRect r)
{
    Ui *ui = &app->ui;
    ui_panel(ui, r, UI_PANEL);
    ui_text_font(ui, FONT_SEMIBOLD, r.x + 16, r.y + 13, 9.2f, UI_MUTED, "WEBCAM");
    const char *cname = s->paused ? "Paused"
                        : app->readme_fixture && app->image_mode ? "MacBook Pro Camera"
                        : app->demo ? "Synthetic bench meter"
                        : app->image_mode ? "Test image"
                        : app->camera_ok ? app->cam.name
                                         : "Unavailable";
    float hx = r.x + r.w - 10;
    if (app->camera_ok && app->cam.count > 1 && !app->demo && !app->image_mode) {
        hx -= 70;
        UiRect sw = ui_rect(hx, r.y + 8, 70, 24);
        if (ui_button(ui, sw, "Switch", BUTTON_QUIET, true)) {
            if (camera_switch(&app->cam)) app_notify(app, false, "Camera: %s", app->cam.name);
            else app_notify(app, true, "Camera switch failed: %s", app->cam.error);
        }
        hx -= 8;
    }
    hx -= 76;
    UiRect folder_btn = ui_rect(hx, r.y + 8, 76, 24);
    if (ui_button(ui, folder_btn, "Folder", BUTTON_QUIET, true)) open_capture_folder(app);
    hx -= 8;
    hx -= 88;
    UiRect setup_btn = ui_rect(hx, r.y + 8, 88, 24);
    if (ui_button(ui, setup_btn, "Setup", BUTTON_QUIET, true)) copy_reader_setup(app);
    float name_w = hx - (r.x + 80) - 10;
    if (name_w < 80) name_w = 80;
    ui_text_ellipsis(ui, FONT_MEDIUM, r.x + 80, r.y + 13, 9.6f, UI_FAINT, name_w, cname);

    UiRect p = ui_rect(r.x + 10, r.y + 40, r.w - 20, r.h - 50);
    ui_round_rect(ui, p, 10, UI_COLOR(5, 8, 11, 255));
    ui_round_rect(ui, ui_inset(p, 1), 9, UI_COLOR(8, 12, 16, 255));

    SDL_Texture *tex = NULL;
    int tw = 0, th = 0;
    if (app->demo) {
        draw_demo_feed(app, p);
    } else if (app->image_mode && app->img_tex) {
        tex = app->img_tex;
        tw = app->img_w;
        th = app->img_h;
    } else if (app->camera_ok && app->cam.tex) {
        tex = app->cam.tex;
        tw = app->cam.width;
        th = app->cam.height;
    }

    if (tex && tw > 0 && th > 0) {
        float aspect = (float)tw / (float)th;
        float fit_w = p.w, fit_h = fit_w / aspect;
        if (fit_h > p.h) {
            fit_h = p.h;
            fit_w = fit_h * aspect;
        }
        SDL_FRect dst = {p.x + (p.w - fit_w) / 2, p.y + (p.h - fit_h) / 2, fit_w, fit_h};
        SDL_RenderTexture(ui->ren, tex, NULL, &dst);
    } else if (!app->demo) {
        bool pending = app->cam.permission_pending;
        bool denied = app->cam.permission_denied;
        const char *title = "No camera";
        const char *sub = app->camera_error[0] ? app->camera_error
                                               : camera_platform_hint();
        if (pending) {
            title = "Camera permission required";
            sub = "A system camera permission dialog should be open. If it is not "
                  "visible, use Camera settings below.";
        } else if (denied) {
            title = "Camera access denied";
            sub = camera_platform_hint();
        } else if (!app->camera_ok) {
            title = "Camera unavailable";
        }
        if (!app->paused) {
            ui_text_center(ui, FONT_SEMIBOLD, p.x + p.w / 2, p.y + p.h / 2 - 30, 15,
                           UI_MUTED, title);
            ui_text_wrapped(ui, p.x + p.w * 0.14f, p.y + p.h / 2 - 8, p.w * 0.72f, 9.6f,
                            UI_FAINT, 4, sub);
            float button_y = p.y + p.h / 2 + 56;
            bool show_settings = camera_platform_can_open_settings();
            bool show_relaunch = camera_platform_can_relaunch() && (pending || denied);
            float bw_retry = 104, bw_settings = 112, bw_relaunch = 108, bgap = 8;
            float total = bw_retry + (show_settings ? bw_settings + bgap : 0) +
                          (show_relaunch ? bw_relaunch + bgap : 0);
            float bx = p.x + p.w / 2 - total / 2;
            UiRect retry = ui_rect(bx, button_y, bw_retry, 30);
            if (ui_button(ui, retry, "Retry camera", BUTTON_SECONDARY, true))
                app_retry_camera(app, true);
            bx += bw_retry + bgap;
            if (show_settings) {
                UiRect settings_cam = ui_rect(bx, button_y, bw_settings, 30);
                if (ui_button(ui, settings_cam, "Camera settings", BUTTON_SECONDARY, true)) {
                    if (!camera_platform_open_settings())
                        app_notify(app, true, "Could not open the camera settings page.");
                }
                bx += bw_settings + bgap;
            }
            if (show_relaunch) {
                UiRect relaunch = ui_rect(bx, button_y, bw_relaunch, 30);
                if (ui_button(ui, relaunch, "Relaunch app", BUTTON_SECONDARY, true)) {
                    if (camera_platform_relaunch()) {
                        SDL_SetAtomicInt(&app->quit, 1);
                    } else {
                        app_notify(app, true, "Could not relaunch CellScope.app.");
                    }
                }
            }
        }
    }

    /* Overlay badges. Paused rendering keeps the pane text centered only. */
    if (!s->paused) {
    UiRect badge = ui_rect(p.x + 12, p.y + 12, 74, 20);
    ui_round_rect(ui, badge, 10, UI_COLOR(4, 8, 11, 210));
    bool camera_live = app->demo || app->image_mode ||
                       (app->camera_ok && app->cam.active);
    UiColor bcol = s->paused ? UI_AMBER
                   : app->demo ? UI_VIOLET
                   : app->image_mode && !app->readme_fixture ? UI_BLUE
                   : camera_live ? UI_RED
                                 : UI_FAINT;
    draw_dot(ui, badge.x + 11, badge.y + 10, bcol);
    const char *blabel = s->paused ? "PAUSED"
                         : app->demo ? "DEMO"
                         : app->image_mode && !app->readme_fixture ? "IMAGE"
                         : camera_live ? "LIVE"
                                       : "OFFLINE";
    ui_text_font(ui, FONT_SEMIBOLD, badge.x + 19, badge.y + 4.5f, 8.2f, bcol, blabel);
    }
    if (tw > 0) {
        char info[64];
        snprintf(info, sizeof(info), "%d × %d", tw, th);
        ui_text_right(ui, FONT_MONO, p.x + p.w - 12, p.y + 12, 8.5f,
                      UI_COLOR(190, 210, 220, 185), info);
    }

    draw_guide(ui, p, s);

    if (s->paused) {
        ui_rect_fill(ui, p, UI_COLOR(4, 8, 11, 150));
        float cx = p.x + p.w / 2;
        float cy = p.y + p.h / 2;
        ui_text_center(ui, FONT_SEMIBOLD, cx, cy - 17, 13.0f, UI_TEXT, "PAUSED");
        ui_text_center(ui, FONT_MONO, cx, cy + 9, 8.6f, UI_FAINT,
                       "Space or the top play button resumes");
    }

    /* Live detection box. Consensus replaces the live value for the same
     * three-second window and moves the green check into this box. */
    bool consensus = s->reader.visible && s->reader.hz > 0;
    if (s->reader.screen_visible || consensus) {
        float cw = CS_MIN(276.0f, p.w * 0.36f);
        UiRect lr = ui_rect(p.x + p.w - cw - 12, p.y + p.h - 136, cw, 82);
        ui_round_rect(ui, lr, 9, UI_COLOR(4, 9, 12, 225));
        ui_rect_outline(ui, lr, consensus ? UI_COLOR(55, 150, 112, 235)
                                         : UI_COLOR(55, 92, 84, 220), 1);
        ui_text_font(ui, FONT_SEMIBOLD, lr.x + 12, lr.y + 9, 7.2f, UI_ACCENT,
                     "LIVE DETECTION");
        if (consensus) {
            float cx = lr.x + lr.w - 17;
            float cy = lr.y + 15;
            UiColor check = UI_COLOR(95, 245, 170, 255);
            ui_line(ui, cx - 7.0f, cy, cx - 2.0f, cy + 5.0f, check, 2.6f);
            ui_line(ui, cx - 2.0f, cy + 5.0f, cx + 7.5f, cy - 6.0f, check, 2.6f);
        }
        if (consensus) {
            char mhz[64];
            format_mhz_reading(s->reader.hz, mhz, sizeof(mhz));
            ui_text_ellipsis(ui, FONT_MONO, lr.x + 12, lr.y + 24, 18.0f, UI_TEXT,
                             lr.w - 24, mhz);
            char sub[96];
            snprintf(sub, sizeof(sub), "CONSENSUS REACHED · CONF %d%%",
                     (int)(CS_MAX(0.0, CS_MIN(1.0, s->reader.confidence)) * 100.0));
            ui_text_ellipsis(ui, FONT_MONO, lr.x + 12, lr.y + 58, 7.6f, UI_ACCENT,
                             lr.w - 24, sub);
        } else if (s->reader.live_valid && s->reader.live_hz > 0) {
            char mhz[64];
            format_mhz_reading(s->reader.live_hz, mhz, sizeof(mhz));
            ui_text_ellipsis(ui, FONT_MONO, lr.x + 12, lr.y + 24, 18.0f, UI_TEXT,
                             lr.w - 24, mhz);
            char sub[96];
            snprintf(sub, sizeof(sub), "LIVE · CONF %d%% · collecting best",
                     (int)(s->reader.live_confidence * 100.0));
            ui_text_ellipsis(ui, FONT_MONO, lr.x + 12, lr.y + 58, 7.6f, UI_FAINT,
                             lr.w - 24, sub);
        } else {
            ui_text_font(ui, FONT_MONO, lr.x + 12, lr.y + 26, 20.0f, UI_FAINT, "—");
            ui_text_font(ui, FONT_MONO, lr.x + 12, lr.y + 58, 7.6f, UI_FAINT,
                         "screen visible · reading next frame");
        }
    }
}

typedef struct {
    const char *label;
    float width;
} ListColumn;

static float list_width(const ListColumn *cols, int n)
{
    float w = 0;
    for (int i = 0; i < n; i++) w += cols[i].width;
    return w;
}

static void list_scrollbar_v(Ui *ui, UiRect track, float scroll, float content, float view,
                             float *out_scroll)
{
    if (content <= view + 1) return;
    ui_round_rect(ui, track, track.w / 2, UI_COLOR(24, 34, 45, 210));
    float max_scroll = content - view;
    float th = CS_MAX(26.0f, track.h * view / content);
    float t = max_scroll > 0 ? scroll / max_scroll : 0;
    float ty = track.y + (track.h - th) * t;
    UiRect thumb = ui_rect(track.x, ty, track.w, th);
    ui_round_rect(ui, thumb, track.w / 2, UI_COLOR(92, 116, 136, 230));
    if (ui->mouse_pressed && ui_hit(ui, track)) {
        float nt = (ui->my - ui->origin_y - track.y - th / 2) / CS_MAX(1.0f, track.h - th);
        if (nt < 0) nt = 0;
        if (nt > 1) nt = 1;
        *out_scroll = nt * max_scroll;
        ui_consume_click(ui);
    }
}

static void list_scrollbar_h(Ui *ui, UiRect track, float scroll, float content, float view,
                             float *out_scroll)
{
    if (content <= view + 1) return;
    ui_round_rect(ui, track, track.h / 2, UI_COLOR(24, 34, 45, 210));
    float max_scroll = content - view;
    float tw = CS_MAX(26.0f, track.w * view / content);
    float t = max_scroll > 0 ? scroll / max_scroll : 0;
    float tx = track.x + (track.w - tw) * t;
    UiRect thumb = ui_rect(tx, track.y, tw, track.h);
    ui_round_rect(ui, thumb, track.h / 2, UI_COLOR(92, 116, 136, 230));
    if (ui->mouse_pressed && ui_hit(ui, track)) {
        float nt = (ui->mx - track.x - tw / 2) / CS_MAX(1.0f, track.w - tw);
        if (nt < 0) nt = 0;
        if (nt > 1) nt = 1;
        *out_scroll = nt * max_scroll;
        ui_consume_click(ui);
    }
}

static int wifi_best_dbm(const char *text)
{
    if (!text || !*text) return -999;
    char buf[768];
    cs_strlcpy(buf, text, sizeof(buf));
    int best = -999;
    char *save = NULL;
    for (char *p = strtok_r(buf, ";", &save); p; p = strtok_r(NULL, ";", &save)) {
        unsigned id = 0;
        int dbm = 0;
        if (sscanf(p, "%u:%d", &id, &dbm) == 2 && dbm > best) best = dbm;
    }
    return best;
}

static void draw_cell_rows(App *app, UiState *s, UiRect body)
{
    Ui *ui = &app->ui;
    static const ListColumn cols[] = {
        {"TIME", 58},       {"OPERATOR", 145}, {"BAND", 46},  {"RAT", 66},
        {"PCI", 76},        {"ARFCN", 92},     {"DL MHz", 82}, {"UL MHz", 82},
        {"TAC", 42},        {"eNB / CID", 94}, {"BW", 50},    {"CA", 30},
        {"WIFI dBm", 76},
    };
    const int ncols = (int)CS_ARRAY_LEN(cols);
    const float row_h = 25.0f, header_h = 23.0f, sb_w = 7.0f, sb_h = 7.0f;
    int count = s->cell_count;
    if (count == 0) {
        float cx = body.x + body.w / 2;
        float cy = body.y + body.h / 2;
        ui_round_rect(ui, ui_rect(cx - 22, cy - 46, 44, 44), 12, UI_COLOR(22, 32, 42, 255));
        ui_text_center(ui, FONT_SEMIBOLD, cx, cy - 8, 13, UI_MUTED,
                       "Waiting for the first cell sample");
        ui_text_center(ui, FONT_UI, cx, cy + 14, 9.8f, UI_FAINT,
                       s->adb.state == ADB_STATE_CONNECTED
                           ? "Polling telephony.registry once per second."
                           : "Connect a phone over ADB to begin.");
        return;
    }

    float content_w = list_width(cols, ncols);
    float view_w = CS_MAX(80.0f, body.w - sb_w - 2);
    bool has_h = content_w > view_w + 1;
    float view_h = CS_MAX(40.0f, body.h - header_h - (has_h ? sb_h : 0));
    float content_h = count * row_h;
    float max_y = CS_MAX(0.0f, content_h - view_h);
    float max_x = CS_MAX(0.0f, content_w - view_w);
    if (app->cell_follow) app->cell_scroll = floorf(max_y / row_h) * row_h;
    if (ui_hit(ui, body) && ui->wheel != 0) {
        if ((SDL_GetModState() & SDL_KMOD_SHIFT) && has_h)
            app->cell_scroll_x -= ui->wheel * 42.0f;
        else
            app->cell_scroll -= ui->wheel * row_h * 1.5f;
        ui->wheel = 0;
    }
    if (app->cell_scroll < 0) app->cell_scroll = 0;
    if (app->cell_scroll > max_y) app->cell_scroll = max_y;
    if (app->cell_scroll_x < 0) app->cell_scroll_x = 0;
    if (app->cell_scroll_x > max_x) app->cell_scroll_x = max_x;
    app->cell_follow = app->cell_scroll >= max_y - 1;

    UiRect head = ui_rect(body.x, body.y, view_w, header_h);
    ui_push_clip(ui, ui_rect(head.x, head.y, head.w, head.h));
    ui_rect_fill(ui, head, UI_COLOR(17, 25, 33, 245));
    float hx = head.x + 8 - app->cell_scroll_x;
    for (int c = 0; c < ncols; c++) {
        ui_text_ellipsis(ui, FONT_MONO, hx, head.y + 7, 7.6f, UI_FAINT,
                         cols[c].width - 10, cols[c].label);
        hx += cols[c].width;
    }
    ui_pop_clip(ui);
    ui_line(ui, head.x, head.y + head.h - 1, head.x + head.w, head.y + head.h - 1,
            UI_BORDER_SOFT, 1);

    UiRect view = ui_rect(body.x, body.y + header_h, view_w, view_h);
    int start = (int)floorf(app->cell_scroll / row_h);
    float offset = app->cell_scroll - start * row_h;
    float y = view.y - offset;
    ui_push_clip(ui, view);
    for (int i = start; i < count && y < view.y + view.h; i++, y += row_h) {
        if (y + row_h < view.y) continue;
        CellSample c = app_cell_at(app, i);
        bool newest = (i == count - 1);
        UiRect row = ui_rect(view.x, y, view.w, row_h - 1);
        if (ui_hit(ui, row))
            ui_rect_fill(ui, row, UI_COLOR(25, 37, 48, 190));
        else if (i % 2 == 0)
            ui_rect_fill(ui, row, UI_COLOR(14, 21, 28, 120));
        if (newest) ui_rect_fill(ui, ui_rect(row.x, row.y + 4, 2, row.h - 8),
                                 UI_ACCENT);

        char time[20];
        cs_format_time(c.ts_ms, time, sizeof(time));
        char op[160];
        if (c.operator_name[0] && strcmp(c.operator_name, "—") != 0)
            snprintf(op, sizeof(op), "%s · %s", c.plmn[0] ? c.plmn : "—", c.operator_name);
        else
            snprintf(op, sizeof(op), "%s", c.plmn[0] ? c.plmn : "—");
        char dl[48], ul[48], ec[48], wifi[24];
        int wifi_dbm = wifi_best_dbm(c.wifi);
        if (wifi_dbm > -999) snprintf(wifi, sizeof(wifi), "%d", wifi_dbm);
        else cs_strlcpy(wifi, "—", sizeof(wifi));
        snprintf(dl, sizeof(dl), "%s", c.f_dl[0] ? c.f_dl : "—");
        snprintf(ul, sizeof(ul), "%s", c.f_ul[0] ? c.f_ul : "—");
        snprintf(ec, sizeof(ec), "%s / %s", c.enb[0] ? c.enb : "—",
                 c.cid[0] ? c.cid : "—");
        const char *vals[] = {
            time, op, c.band[0] ? c.band : "—", c.rat[0] ? c.rat : "—",
            c.pci[0] ? c.pci : "—", c.arfcn[0] ? c.arfcn : "—", dl, ul,
            c.tac[0] ? c.tac : "—", ec, c.bw[0] ? c.bw : "—", c.ca ? "CA" : "—",
            wifi,
        };
        float x = view.x + 8 - app->cell_scroll_x;
        for (int col = 0; col < ncols; col++) {
            UiColor fg = UI_COLOR(172, 194, 210, 255);
            if (col == 0) fg = newest ? UI_ACCENT : UI_FAINT;
            else if (col == 2) fg = c.ca ? UI_ACCENT : UI_TEXT;
            else if (col == 11 && c.ca) fg = UI_AMBER;
            else if (col == 12 && wifi_dbm >= -60) fg = UI_ACCENT;
            ui_text_ellipsis(ui, FONT_MONO, x, row.y + 7, 8.4f, fg,
                             cols[col].width - 10, vals[col]);
            x += cols[col].width;
        }
    }
    ui_pop_clip(ui);

    list_scrollbar_v(ui, ui_rect(body.x + body.w - sb_w, head.y, sb_w,
                                 body.h - (has_h ? sb_h : 0)),
                     app->cell_scroll, content_h, view_h, &app->cell_scroll);
    if (has_h)
        list_scrollbar_h(ui, ui_rect(view.x, body.y + body.h - sb_h, view.w, sb_h),
                         app->cell_scroll_x, content_w, view_w, &app->cell_scroll_x);
}

static void draw_cell_panel(App *app, UiState *s, UiRect r)
{
    Ui *ui = &app->ui;
    ui_panel(ui, r, UI_PANEL);
    ui_text_font(ui, FONT_SEMIBOLD, r.x + 16, r.y + 13, 9.2f, UI_MUTED, "CELL SAMPLES");
    char count[32];
    snprintf(count, sizeof(count), "%d", s->cell_count);
    draw_chip(ui, r.x + 16 + ui_text_width(ui, FONT_SEMIBOLD, 9.2f, "CELL SAMPLES") + 10,
              r.y + 10, count, UI_MUTED, UI_COLOR(26, 37, 48, 255));
    if (s->adb.state == ADB_STATE_CONNECTED) {
        float pull = (float)(s->last_poll_ok_ms ? (cs_now_ms() - s->last_poll_ok_ms) : -1);
        char live[64];
        if (pull >= 0 && pull < 2500) snprintf(live, sizeof(live), "ADB · 1 Hz");
        else snprintf(live, sizeof(live), "ADB · waiting");
        draw_dot(ui, r.x + r.w - 94, r.y + 18, pull >= 0 && pull < 2500 ? UI_ACCENT : UI_AMBER);
        ui_text_font(ui, FONT_MONO, r.x + r.w - 86, r.y + 12, 8.8f, UI_FAINT, live);
    } else {
        UiRect rec = ui_rect(r.x + r.w - 104, r.y + 8, 88, 24);
        if (ui_button(ui, rec, "Connect ADB", BUTTON_SECONDARY, true)) app_request_check(app);
    }
    ui_line(ui, r.x + 12, r.y + 39.5f, r.x + r.w - 12, r.y + 39.5f, UI_BORDER_SOFT, 1);
    draw_cell_rows(app, s, ui_rect(r.x + 8, r.y + 44, r.w - 16, r.h - 50));
}

static const char *ui_shot_name(const MeterSample *m)
{
    if (m->shot_name[0]) return m->shot_name;
    if (!m->path[0]) return "";
    const char *a = strrchr(m->path, '/');
    const char *b = strrchr(m->path, '\\');
    const char *p = NULL;
    if (a && b) p = b > a ? b : a;
    else p = a ? a : b;
    return p ? p + 1 : m->path;
}

static void draw_meter_rows(App *app, UiState *s, UiRect body)
{
    Ui *ui = &app->ui;
    static const ListColumn cols[] = {
        {"", 30}, {"TIME", 62}, {"SCREENSHOT", 190}, {"FREQUENCY (HZ)", 126},
        {"CONF", 52}, {"BACKEND", 112},
    };
    const int ncols = (int)CS_ARRAY_LEN(cols);
    const float row_h = 26.0f, header_h = 23.0f, sb_w = 7.0f, sb_h = 7.0f;
    int count = s->meter_count;
    if (count == 0) {
        float cx = body.x + body.w / 2;
        float cy = body.y + body.h / 2;
        ui_round_rect(ui, ui_rect(cx - 22, cy - 46, 44, 44), 12, UI_COLOR(22, 32, 42, 255));
        ui_text_center(ui, FONT_MONO, cx, cy - 33, 18, UI_ACCENT, "Hz");
        ui_text_center(ui, FONT_SEMIBOLD, cx, cy + 8, 13, UI_MUTED,
                       "No readings captured yet");
        ui_text_center(ui, FONT_UI, cx, cy + 28, 9.8f, UI_FAINT,
                       "Point the camera at a frequency meter.");
        return;
    }

    float content_w = list_width(cols, ncols);
    float view_w = CS_MAX(80.0f, body.w - sb_w - 2);
    bool has_h = content_w > view_w + 1;
    float view_h = CS_MAX(40.0f, body.h - header_h - (has_h ? sb_h : 0));
    float content_h = count * row_h;
    float max_y = CS_MAX(0.0f, content_h - view_h);
    float max_x = CS_MAX(0.0f, content_w - view_w);
    if (app->meter_follow) app->meter_scroll = floorf(max_y / row_h) * row_h;
    if (ui_hit(ui, body) && ui->wheel != 0) {
        if ((SDL_GetModState() & SDL_KMOD_SHIFT) && has_h)
            app->meter_scroll_x -= ui->wheel * 42.0f;
        else
            app->meter_scroll -= ui->wheel * row_h * 1.5f;
        ui->wheel = 0;
    }
    if (app->meter_scroll < 0) app->meter_scroll = 0;
    if (app->meter_scroll > max_y) app->meter_scroll = max_y;
    if (app->meter_scroll_x < 0) app->meter_scroll_x = 0;
    if (app->meter_scroll_x > max_x) app->meter_scroll_x = max_x;
    app->meter_follow = app->meter_scroll >= max_y - 1;

    UiRect head = ui_rect(body.x, body.y, view_w, header_h);
    ui_push_clip(ui, ui_rect(head.x, head.y, head.w, head.h));
    ui_rect_fill(ui, head, UI_COLOR(17, 25, 33, 245));
    float hx = head.x + 8 - app->meter_scroll_x;
    for (int c = 0; c < ncols; c++) {
        bool right = (c == 3 || c == 4);
        if (right)
            ui_text_right(ui, FONT_MONO, hx + cols[c].width - 10, head.y + 7, 7.6f,
                          UI_FAINT, cols[c].label);
        else
            ui_text_ellipsis(ui, FONT_MONO, hx, head.y + 7, 7.6f, UI_FAINT,
                             cols[c].width - 10, cols[c].label);
        hx += cols[c].width;
    }
    ui_pop_clip(ui);
    ui_line(ui, head.x, head.y + head.h - 1, head.x + head.w, head.y + head.h - 1,
            UI_BORDER_SOFT, 1);

    UiRect view = ui_rect(body.x, body.y + header_h, view_w, view_h);
    int start = (int)floorf(app->meter_scroll / row_h);
    float offset = app->meter_scroll - start * row_h;
    float y = view.y - offset;
    ui_push_clip(ui, view);
    for (int i = start; i < count && y < view.y + view.h; i++, y += row_h) {
        if (y + row_h < view.y) continue;
        MeterSample m = app_meter_at(app, i);
        bool newest = (i == count - 1);
        bool selected = (app->selected_meter_ts == m.ts_ms);
        UiRect row = ui_rect(view.x, y, view.w, row_h - 1);
        if (selected) ui_rect_fill(ui, row, UI_COLOR(30, 45, 56, 220));
        else if (ui_hit(ui, row))
            ui_rect_fill(ui, row, UI_COLOR(25, 37, 48, 190));
        else if (i % 2 == 0)
            ui_rect_fill(ui, row, UI_COLOR(14, 21, 28, 120));
        if (newest) ui_rect_fill(ui, ui_rect(row.x, row.y + 4, 2, row.h - 8),
                                 UI_ACCENT);

        char hz[64], conf[16];
        cs_group_u64(m.hz, hz, sizeof(hz));
        snprintf(conf, sizeof(conf), "%d%%", (int)(m.confidence * 100.0));
        const char *shot = ui_shot_name(&m);
        float x = view.x + 8 - app->meter_scroll_x;
        UiRect del = ui_rect(x + 1, row.y + 5, 16, 16);
        bool del_hover = ui_hit(ui, del);
        ui_round_rect(ui, del, 4, del_hover ? UI_COLOR(93, 40, 44, 255)
                                            : UI_COLOR(24, 32, 40, 210));
        ui_text_center(ui, FONT_MONO, del.x + del.w / 2, del.y + 2.5f, 10.0f,
                       del_hover ? UI_COLOR(255, 170, 170, 255) : UI_FAINT, "×");
        if (del_hover && ui->mouse_pressed) {
            app_request_remove_meter(app, i);
            ui_consume_click(ui);
            x += cols[0].width;
            continue;
        }
        x += cols[0].width;
        ui_text_ellipsis(ui, FONT_MONO, x, row.y + 8, 8.6f,
                         newest ? UI_ACCENT : UI_FAINT, cols[1].width - 10, m.time_text);
        x += cols[1].width;
        ui_text_ellipsis(ui, FONT_MONO, x, row.y + 8, 8.2f,
                         shot[0] ? UI_COLOR(150, 178, 198, 255) : UI_FAINT,
                         cols[2].width - 10, shot[0] ? shot : "—");
        x += cols[2].width;
        ui_text_right(ui, FONT_MONO, x + cols[3].width - 10, row.y + 7, 10.4f,
                      UI_TEXT, hz);
        x += cols[3].width;
        ui_text_right(ui, FONT_MONO, x + cols[4].width - 10, row.y + 8, 8.4f,
                      UI_FAINT, conf);
        x += cols[4].width;
        ui_text_ellipsis(ui, FONT_MONO, x, row.y + 8, 8.2f, UI_FAINT,
                         cols[5].width - 10, m.backend[0] ? m.backend : "reader");

        if (ui_hit(ui, row) && ui->mouse_pressed) {
            app->selected_meter_ts = m.ts_ms;
            app->selected_meter = i;
            if (m.path[0]) app_notify(app, false, "Captured image: %s", m.path);
            ui_consume_click(ui);
        }
    }
    ui_pop_clip(ui);

    list_scrollbar_v(ui, ui_rect(body.x + body.w - sb_w, head.y, sb_w,
                                 body.h - (has_h ? sb_h : 0)),
                     app->meter_scroll, content_h, view_h, &app->meter_scroll);
    if (has_h)
        list_scrollbar_h(ui, ui_rect(view.x, body.y + body.h - sb_h, view.w, sb_h),
                         app->meter_scroll_x, content_w, view_w, &app->meter_scroll_x);
}

static void draw_meter_panel(App *app, UiState *s, UiRect r)
{
    Ui *ui = &app->ui;
    ui_panel(ui, r, UI_PANEL);
    ui_text_font(ui, FONT_SEMIBOLD, r.x + 16, r.y + 13, 9.2f, UI_MUTED, "READINGS");
    char count[32];
    snprintf(count, sizeof(count), "%d", s->meter_count);
    draw_chip(ui, r.x + 16 + ui_text_width(ui, FONT_SEMIBOLD, 9.2f, "READINGS") + 10,
              r.y + 10, count, UI_MUTED, UI_COLOR(26, 37, 48, 255));
    draw_meter_rows(app, s, ui_rect(r.x + 8, r.y + 44, r.w - 16, r.h - 50));
}

static void draw_footer(App *app, UiState *s)
{
    Ui *ui = &app->ui;
    float h = (float)ui->view_h;
    ui_line(ui, 0, h - 28.5f, (float)ui->win_w, h - 28.5f, UI_BORDER_SOFT, 1);
    char left[256];
    const char *con = s->adb.state == ADB_STATE_CONNECTED
                          ? (s->adb.serial[0] ? s->adb.serial : "connected")
                          : "disconnected";
    snprintf(left, sizeof(left), "ADB · %s%s · %d samples", con,
             s->paused ? " · PAUSED" : "", s->cell_count);
    ui_text_font(ui, FONT_MONO, 18, h - 20, 8.4f, UI_FAINT, left);
    char center[CS_PATH_MAX + 32];
    if (app->selected_meter >= 0 && app->selected_meter_ts > 0) {
        MeterSample m = app_meter_at(app, app->selected_meter);
        if (m.path[0]) snprintf(center, sizeof(center), "Reading screenshot · %s", m.path);
        else if (app->session_dir[0])
            snprintf(center, sizeof(center), "Dataset · %s", app->session_dir);
        else snprintf(center, sizeof(center), "Working dir · %s", app->capture_dir);
    } else if (app->session_dir[0]) {
        snprintf(center, sizeof(center), "Dataset · %s", app->session_dir);
    } else {
        snprintf(center, sizeof(center), "Working dir · %s", app->capture_dir);
    }
    ui_text_ellipsis(ui, FONT_MONO, 330, h - 20, 8.4f, UI_FAINT, (float)ui->win_w - 700,
                     center);
    ui_text_right(ui, FONT_MONO, (float)ui->win_w - 18, h - 20, 8.4f, UI_FAINT,
                  "Space pause · R reconnect · C camera · E export");
}

static void draw_toast(App *app)
{
    if (!app->toast[0]) return;
    Ui *ui = &app->ui;
    float tw = ui_text_width(ui, FONT_MEDIUM, 10.8f, app->toast);
    float w = CS_MIN((float)ui->win_w - 80, tw + 46);
    UiRect r = ui_rect(((float)ui->win_w - w) / 2, (float)ui->view_h - 74, w, 34);
    ui_round_rect(ui, r, 10, UI_COLOR(12, 18, 25, 242));
    ui_round_rect(ui, ui_inset(r, 1), 9, UI_COLOR(17, 25, 34, 242));
    draw_dot(ui, r.x + 16, r.y + 17, app->toast_error ? UI_RED : UI_ACCENT);
    ui_text_ellipsis(ui, FONT_MEDIUM, r.x + 26, r.y + 10, 10.8f,
                     app->toast_error ? UI_COLOR(255, 180, 180, 255) : UI_TEXT, w - 38,
                     app->toast);
}

static void draw_step(Ui *ui, float x, float y, int number, const char *title, const char *desc)
{
    ui_circle(ui, x + 11, y + 10, 11, UI_COLOR(27, 49, 42, 255));
    char n[8];
    snprintf(n, sizeof(n), "%d", number);
    ui_text_center(ui, FONT_SEMIBOLD, x + 11, y + 3, 9.5f, UI_ACCENT, n);
    ui_text_font(ui, FONT_SEMIBOLD, x + 32, y - 1, 11.8f, UI_TEXT, title);
    ui_text_font(ui, FONT_UI, x + 32, y + 17, 9.8f, UI_MUTED, desc);
}

static void draw_modal(App *app, UiState *s)
{
    Ui *ui = &app->ui;
    float W = (float)ui->win_w, H = (float)ui->view_h;
    ui_rect_fill(ui, ui_rect(0, 0, W, H), UI_COLOR(3, 5, 8, 196));

    float cw = CS_MIN(790.0f, W - 70);
    float ch = CS_MIN(470.0f, H - 60);
    UiRect card = ui_rect((W - cw) / 2, (H - ch) / 2, cw, ch);
    ui_round_rect(ui, ui_rect(card.x - 2, card.y - 2, card.w + 4, card.h + 4), 18,
                  UI_COLOR(35, 55, 48, 160));
    ui_panel(ui, card, UI_PANEL);
    ui_round_rect(ui, ui_rect(card.x + 1, card.y + 1, card.w - 2, 4), 2, UI_ACCENT);

    float x = card.x + 30;
    float y = card.y + 26;
    ui_circle(ui, x + 23, y + 23, 23, UI_COLOR(26, 49, 42, 255));
    ui_round_rect(ui, ui_rect(x + 15, y + 9, 16, 28), 4, UI_ACCENT);
    ui_round_rect(ui, ui_rect(x + 18, y + 13, 10, 18), 2, UI_COLOR(9, 21, 17, 255));
    ui_circle(ui, x + 23, y + 34, 1.6f, UI_ACCENT);

    ui_text_font(ui, FONT_SEMIBOLD, x + 62, y + 2, 21.5f, UI_TEXT, "Connect your Android phone");
    ui_text_font(ui, FONT_UI, x + 62, y + 32, 10.8f, UI_MUTED,
                 "Live cell data is read over ADB. No root required.");

    float sy = y + 76;
    draw_step(ui, x, sy, 1, "Enable Developer options",
              "Settings → About phone → tap Build number seven times.");
    draw_step(ui, x, sy + 46, 2, "Turn on USB debugging",
              "Settings → System → Developer options → USB debugging.");
    draw_step(ui, x, sy + 92, 3, "Plug in with a data cable",
              "Unlock the phone, choose File transfer / MTP, and accept any prompt.");
    draw_step(ui, x, sy + 138, 4, "Authorize this computer",
              "Tap Allow on “Allow USB debugging?” and tick “Always allow”.");

    float status_y = sy + 188;
    float status_h = 118;
    UiRect st = ui_rect(x, status_y, card.w - 60, status_h);
    ui_round_rect(ui, st, 10, UI_COLOR(12, 18, 25, 255));
    ui_rect_outline(ui, st, UI_BORDER_SOFT, 1);

    bool connected = s->adb.state == ADB_STATE_CONNECTED;
    float ix = st.x + 20, iy = st.y + 20;
    if (s->modal_checking && !connected) {
        float a = (float)((SDL_GetTicks() % 900) / 900.0 * 360.0);
        ui_arc(ui, ix + 12, iy + 12, 11, a, a + 280, 2.4f, UI_ACCENT);
    } else if (connected) {
        ui_circle(ui, ix + 12, iy + 12, 12, UI_COLOR(28, 77, 60, 255));
        ui_line(ui, ix + 6, iy + 12, ix + 10, iy + 17, UI_ACCENT, 2);
        ui_line(ui, ix + 10, iy + 17, ix + 18, iy + 7, UI_ACCENT, 2);
    } else {
        ui_circle(ui, ix + 12, iy + 12, 12, UI_COLOR(57, 44, 26, 255));
        ui_text_center(ui, FONT_BOLD, ix + 12, iy + 3, 13, UI_AMBER, "!");
    }

    char title[256], sub[1600];
    UiColor tcol = UI_TEXT;
    if (s->modal_checking && !connected) {
        cs_strlcpy(title, "Looking for your phone…", sizeof(title));
        snprintf(sub, sizeof(sub),
                 "Running `adb devices`. Keep the phone unlocked and watch for the "
                 "authorization prompt.");
        tcol = UI_ACCENT;
    } else if (connected) {
        snprintf(title, sizeof(title), "Connected to %s",
                 s->device_label[0] ? s->device_label : s->adb.serial);
        snprintf(sub, sizeof(sub), "ADB serial %s. The window will close automatically.",
                 s->adb.serial);
        tcol = UI_ACCENT;
    } else if (!s->adb.attempted) {
        cs_strlcpy(title, "Ready when you are", sizeof(title));
        cs_strlcpy(sub,
                   "Follow the four steps, then click Check connection. CellScope retries "
                   "automatically while this window is open.",
                   sizeof(sub));
    } else {
        cs_strlcpy(title, s->adb.detail[0] ? s->adb.detail : "No phone detected",
                   sizeof(title));
        snprintf(sub, sizeof(sub), "%s", s->adb.hint[0] ? s->adb.hint : "");
        tcol = UI_RED;
    }
    ui_text_ellipsis(ui, FONT_SEMIBOLD, st.x + 44, st.y + 12, 11.8f, tcol, st.w - 56, title);
    ui_text_wrapped_max(ui, st.x + 44, st.y + 32, st.w - 58, 9.2f, UI_MUTED, 1.5f, 3,
                        sub);

    float by = card.y + card.h - 56;
    UiRect check = ui_rect(x, by, 172, 38);
    UiRect copy = ui_rect(x + 184, by, 150, 38);
    if (ui_button(ui, check, s->modal_checking ? "Checking…" : "Check connection",
                  BUTTON_PRIMARY, !s->modal_checking)) {
        app_request_check(app);
    }
    if (ui_button(ui, copy, "Copy diagnostics", BUTTON_SECONDARY, true)) {
        app_copy_diagnostics(app);
    }
    UiRect skip = ui_rect(card.x + card.w - 210, by + 8, 180, 24);
    if (ui_button(ui, skip, "Continue without phone", BUTTON_QUIET, true)) {
        SDL_LockMutex(app->mu);
        app->modal_open = false;
        SDL_UnlockMutex(app->mu);
        app_notify(app, false, "Continuing without a phone. Use Reconnect anytime.");
    }
}

static void draw_remove_modal(App *app, UiState *s)
{
    Ui *ui = &app->ui;
    float W = (float)ui->win_w, H = (float)ui->view_h;
    ui_rect_fill(ui, ui_rect(0, 0, W, H), UI_COLOR(3, 5, 8, 200));
    float cw = CS_MIN(460.0f, W - 60);
    float ch = 214;
    UiRect card = ui_rect((W - cw) / 2, (H - ch) / 2, cw, ch);
    ui_round_rect(ui, ui_rect(card.x - 2, card.y - 2, card.w + 4, card.h + 4), 14,
                  UI_COLOR(72, 34, 38, 150));
    ui_panel(ui, card, UI_PANEL);
    ui_round_rect(ui, ui_rect(card.x + 1, card.y + 1, card.w - 2, 3), 2, UI_RED);

    MeterSample m;
    memset(&m, 0, sizeof(m));
    if (s->remove_meter_index >= 0) m = app_meter_at(app, s->remove_meter_index);
    char eng[64], hz[64];
    cs_format_eng(m.hz, eng, sizeof(eng));
    cs_format_hz(m.hz, hz, sizeof(hz));

    ui_text_font(ui, FONT_SEMIBOLD, card.x + 24, card.y + 22, 18.0f, UI_TEXT,
                 "Remove this reading?");
    ui_text_font(ui, FONT_UI, card.x + 24, card.y + 52, 10.2f, UI_MUTED,
                 "This cannot be undone. The screenshot file stays on disk.");
    UiRect val = ui_rect(card.x + 24, card.y + 78, card.w - 48, 48);
    ui_round_rect(ui, val, 8, UI_COLOR(11, 17, 23, 255));
    ui_rect_outline(ui, val, UI_BORDER_SOFT, 1);
    ui_text_ellipsis(ui, FONT_MONO, val.x + 12, val.y + 7, 17.0f, UI_TEXT, val.w - 24,
                     eng);
    char sub[256];
    snprintf(sub, sizeof(sub), "%s Hz · %s · %d%%", hz,
             m.time_text[0] ? m.time_text : "—", (int)(m.confidence * 100.0));
    ui_text_ellipsis(ui, FONT_MONO, val.x + 12, val.y + 28, 7.8f, UI_FAINT,
                     val.w - 24, sub);

    float by = card.y + card.h - 54;
    UiRect cancel = ui_rect(card.x + card.w - 250, by, 110, 34);
    UiRect remove = ui_rect(card.x + card.w - 126, by, 102, 34);
    if (ui_button(ui, cancel, "Cancel", BUTTON_SECONDARY, true))
        app_cancel_remove_meter(app);
    if (ui_button(ui, remove, "Remove", BUTTON_DANGER, true))
        app_confirm_remove_meter(app);
}

static void draw_settings_modal(App *app, UiState *s)
{
    (void)s;
    Ui *ui = &app->ui;
    float W = (float)ui->win_w, H = (float)ui->view_h;
    ui_rect_fill(ui, ui_rect(0, 0, W, H), UI_COLOR(3, 5, 8, 205));
    float cw = CS_MIN(740.0f, W - 50), ch = CS_MIN(620.0f, H - 50);
    UiRect card = ui_rect((W - cw) / 2, (H - ch) / 2, cw, ch);
    ui_round_rect(ui, ui_rect(card.x - 2, card.y - 2, card.w + 4, card.h + 4), 16,
                  UI_COLOR(35, 55, 48, 150));
    ui_panel(ui, card, UI_PANEL);
    ui_round_rect(ui, ui_rect(card.x + 1, card.y + 1, card.w - 2, 3), 2, UI_ACCENT);
    ui_text_font(ui, FONT_SEMIBOLD, card.x + 26, card.y + 22, 20.0f, UI_TEXT,
                 "Settings");
    ui_text_font(ui, FONT_UI, card.x + 26, card.y + 50, 9.6f, UI_MUTED,
                 "All fields from settings.json. Changes apply after restart.");
    ui_text_ellipsis(ui, FONT_MONO, card.x + 26, card.y + 66, 7.8f, UI_FAINT,
                     card.w - 52, app->settings_target);

    static const char *labels[9] = {
        "version", "reader_backend", "hf_model", "hf_model_path",
        "hf_port", "hf_device", "camera_index", "upload_api_url", "upload_email",
    };
    float y = card.y + 88;
    for (int i = 0; i < 9; i++) {
        UiRect input = ui_rect(card.x + 170, y, card.w - 196, 30);
        bool focus = (app->settings_focus == i);
        ui_text_font(ui, FONT_MONO, card.x + 26, y + 8, 8.4f, UI_FAINT, labels[i]);
        ui_round_rect(ui, input, 6, UI_COLOR(8, 14, 19, 255));
        ui_rect_outline(ui, input, focus ? UI_ACCENT : UI_BORDER_SOFT, focus ? 1.5f : 1);
        ui_text_ellipsis(ui, FONT_MONO, input.x + 9, input.y + 8, 9.0f,
                         focus ? UI_TEXT : UI_MUTED, input.w - 18,
                         app->settings_edit[i]);
        if (ui_hit(ui, input) && ui->mouse_pressed) {
            app->settings_focus = i;
            ui_consume_click(ui);
        }
        y += 42;
    }

    if (app->settings_error[0])
        ui_text_ellipsis(ui, FONT_MONO, card.x + 26, y + 4, 8.0f, UI_RED,
                         card.w - 52, app->settings_error);

    float by = card.y + card.h - 52;
    UiRect cancel = ui_rect(card.x + card.w - 250, by, 110, 34);
    UiRect save = ui_rect(card.x + card.w - 126, by, 100, 34);
    if (ui_button(ui, cancel, "Cancel", BUTTON_SECONDARY, true))
        app_close_settings_modal(app);
    if (ui_button(ui, save, "Save", BUTTON_PRIMARY, true))
        app_save_settings_modal(app);
}

static void draw_model_modal(App *app, UiState *s)
{
    (void)s;
    Ui *ui = &app->ui;
    float W = (float)ui->win_w, H = (float)ui->view_h;
    ui_rect_fill(ui, ui_rect(0, 0, W, H), UI_COLOR(3, 5, 8, 200));
    float cw = CS_MIN(680.0f, W - 60);
    float ch = CS_MIN(620.0f, H - 60);
    UiRect card = ui_rect((W - cw) / 2, (H - ch) / 2, cw, ch);
    ui_round_rect(ui, ui_rect(card.x - 2, card.y - 2, card.w + 4, card.h + 4), 15,
                  UI_COLOR(35, 55, 48, 150));
    ui_panel(ui, card, UI_PANEL);
    ui_round_rect(ui, ui_rect(card.x + 1, card.y + 1, card.w - 2, 3), 2, UI_ACCENT);
    ui_text_font(ui, FONT_SEMIBOLD, card.x + 24, card.y + 22, 19.0f, UI_TEXT,
                 "Local reader model");
    ui_text_font(ui, FONT_UI, card.x + 24, card.y + 50, 9.4f, UI_MUTED,
                 "Choose a model already downloaded for local inference.");
    ui_text_ellipsis(ui, FONT_MONO, card.x + 24, card.y + 68, 7.8f, UI_FAINT,
                     card.w - 48,
                     app->settings.hf_model_path[0] ? app->settings.hf_model_path
                                                     : "No model path configured");

    float y = card.y + 96;
    int shown = 0;
    for (int i = 0; i < app->model_count && shown < 12; i++, shown++) {
        bool current = strcmp(app->model_paths[i], app->settings.hf_model_path) == 0;
        UiRect row = ui_rect(card.x + 20, y, card.w - 40, 36);
        bool hover = ui_hit(ui, row);
        ui_round_rect(ui, row, 7,
                      current ? UI_COLOR(25, 49, 41, 235)
                              : hover ? UI_COLOR(26, 37, 48, 220)
                                      : UI_COLOR(12, 19, 26, 220));
        if (current)
            ui_round_rect(ui, ui_rect(row.x, row.y + 6, 2, row.h - 12), 1, UI_ACCENT);
        ui_text_ellipsis(ui, FONT_MONO, row.x + 12, row.y + 11, 9.6f,
                         current ? UI_ACCENT : UI_TEXT, row.w - 70,
                         app->model_repos[i]);
        if (current)
            ui_text_right(ui, FONT_MONO, row.x + row.w - 12, row.y + 12, 7.6f,
                          UI_ACCENT, "CURRENT");
        else
            ui_text_right(ui, FONT_MONO, row.x + row.w - 12, row.y + 12, 7.6f,
                          UI_FAINT, "SELECT");
        if (hover && ui->mouse_pressed) {
            app_select_hf_model(app, i);
            ui_consume_click(ui);
            return;
        }
        y += 40;
    }
    if (app->model_count == 0)
        ui_text_center(ui, FONT_UI, card.x + card.w / 2, card.y + 150, 11.0f, UI_FAINT,
                       "No downloaded Hugging Face models found.");

    UiRect cancel = ui_rect(card.x + card.w - 132, card.y + card.h - 50, 104, 32);
    if (ui_button(ui, cancel, "Cancel", BUTTON_SECONDARY, true))
        app_close_model_modal(app);
}

static void draw_upload_modal(App *app, UiState *s)
{
    (void)s;
    Ui *ui = &app->ui;
    float W = (float)ui->win_w, H = (float)ui->view_h;
    ui_rect_fill(ui, ui_rect(0, 0, W, H), UI_COLOR(3, 5, 8, 200));
    float cw = CS_MIN(660.0f, W - 60), ch = 320;
    UiRect card = ui_rect((W - cw) / 2, (H - ch) / 2, cw, ch);
    ui_round_rect(ui, ui_rect(card.x - 2, card.y - 2, card.w + 4, card.h + 4), 14,
                  UI_COLOR(35, 55, 48, 150));
    ui_panel(ui, card, UI_PANEL);
    ui_round_rect(ui, ui_rect(card.x + 1, card.y + 1, card.w - 2, 3), 2, UI_BLUE);
    ui_text_font(ui, FONT_SEMIBOLD, card.x + 26, card.y + 22, 19.0f, UI_TEXT,
                 "Upload dataset");
    char api[512], email[256], status[512];
    int queued = 0;
    SDL_LockMutex(app->mu);
    cs_strlcpy(api, app->settings.upload_api_url, sizeof(api));
    cs_strlcpy(email, app->settings.upload_email, sizeof(email));
    cs_strlcpy(status, app->upload_status, sizeof(status));
    queued = app->upload_job_count;
    SDL_UnlockMutex(app->mu);
    if (queued > 0) {
        char q[64];
        snprintf(q, sizeof(q), "%d queued", queued);
        ui_text_right(ui, FONT_MONO, card.x + card.w - 26, card.y + 90, 8.0f,
                      UI_AMBER, q);
    }

    char line[1024];
    snprintf(line, sizeof(line), "API · %s", api[0] ? api : "not configured");
    ui_text_ellipsis(ui, FONT_MONO, card.x + 26, card.y + 58, 8.4f, UI_FAINT,
                     card.w - 52, line);
    snprintf(line, sizeof(line), "Email · %s", email[0] ? email : "not configured");
    ui_text_ellipsis(ui, FONT_MONO, card.x + 26, card.y + 76, 8.4f, UI_FAINT,
                     card.w - 52, line);
    ui_text_wrapped_max(ui, card.x + 26, card.y + 104, card.w - 52, 9.8f, UI_MUTED,
                        1.5f, 4,
                        "The first upload enrolls this device. Confirm the Brevo "
                        "verification email once, then upload signed datasets without "
                        "re-verifying every time.");
    if (status[0])
        ui_text_wrapped_max(ui, card.x + 26, card.y + 174, card.w - 52, 9.4f,
                            UI_ACCENT, 1.45f, 3, status);

    SDL_LockMutex(app->mu);
    UploadHistoryEntry history[2];
    int hist_count = app->upload_history_count;
    int start = hist_count > 2 ? hist_count - 2 : 0;
    int shown = hist_count - start;
    for (int i = 0; i < shown; i++) history[i] = app->upload_history[start + i];
    SDL_UnlockMutex(app->mu);
    float hy = card.y + 230;
    for (int i = shown - 1; i >= 0; i--) {
        char row[768];
        char when[32];
        cs_format_time_ms(history[i].ts_ms, when, sizeof(when));
        snprintf(row, sizeof(row), "%s  %-9s  %s  %s", when, history[i].status,
                 history[i].archive, history[i].message);
        ui_text_ellipsis(ui, FONT_MONO, card.x + 26, hy, 7.8f,
                         strcmp(history[i].status, "accepted") == 0 ? UI_FAINT : UI_AMBER,
                         card.w - 52, row);
        hy += 16;
    }

    float by = card.y + card.h - 52;
    UiRect close = ui_rect(card.x + card.w - 118, by, 92, 34);
    UiRect enroll = ui_rect(card.x + card.w - 342, by, 132, 34);
    UiRect upload = ui_rect(card.x + card.w - 200, by, 72, 34);
    UiRect reset = ui_rect(card.x + 26, by, 126, 34);
    if (ui_button(ui, close, "Close", BUTTON_SECONDARY, true)) app_close_upload_modal(app);
    if (ui_button(ui, enroll, "Enroll email", BUTTON_SECONDARY, api[0] && email[0]))
        app_enroll_device(app);
    if (ui_button(ui, upload, "Upload", BUTTON_PRIMARY, api[0] && email[0]))
        app_upload_dataset(app);
    if (ui_button(ui, reset, "Reset identity", BUTTON_QUIET, true))
        app_reset_identity(app);
}

static void draw_tab_button(App *app, float x, float y, float w, const char *label,
                            int tab)
{
    Ui *ui = &app->ui;
    UiRect r = ui_rect(x, y, w, 32);
    bool active = app->active_tab == tab;
    bool hover = ui_hover(ui, r);
    UiColor bg = active ? UI_COLOR(24, 35, 45, 255)
                        : (hover ? UI_COLOR(18, 27, 35, 255) : UI_COLOR(12, 18, 24, 255));
    ui_round_rect(ui, r, 8, bg);
    if (active) ui_round_rect(ui, ui_rect(x + 8, y + 30, w - 16, 2), 1, UI_ACCENT);
    ui_text_center(ui, FONT_SEMIBOLD, x + w / 2, y + 9, 11.5f,
                   active ? UI_TEXT : UI_MUTED, label);
    if (hover && ui->mouse_pressed) {
        app_set_tab(app, tab);
        ui_consume_click(ui);
    }
}

static void draw_tab_bar(App *app)
{
    Ui *ui = &app->ui;
    float w = (float)ui->win_w;
    ui_rect_fill(ui, ui_rect(0, 0, w, TABBAR_H), UI_COLOR(8, 12, 17, 255));
    ui_line(ui, 0, TABBAR_H - 0.5f, w, TABBAR_H - 0.5f, UI_BORDER_SOFT, 1);

    float x = 18;
    draw_tab_button(app, x, 8, 94, "RF Check", APP_TAB_RF);
    x += 100;
    draw_tab_button(app, x, 8, 146, "Symptom Report", APP_TAB_SYMPTOM);

    float by = 9, bh = 30, gap = 8;
    float bx = w - 18;
    UiRect export_r = ui_rect(bx - 78, by, 78, bh);
    if (ui_button(ui, export_r, "Export", BUTTON_SECONDARY, true)) app_export_dataset(app);
    bx -= 78 + gap;
    UiRect upload_r = ui_rect(bx - 70, by, 70, bh);
    if (ui_button(ui, upload_r, "Upload", BUTTON_SECONDARY, true))
        app_open_upload_modal(app);
    bx -= 70 + gap;
    UiRect settings_r = ui_rect(bx - 78, by, 78, bh);
    if (ui_button(ui, settings_r, "Settings", BUTTON_SECONDARY, true))
        app_open_settings_modal(app);

}

static void draw_symptom_placeholder(App *app)
{
    Ui *ui = &app->ui;
    float w = (float)ui->win_w, h = (float)ui->view_h;
    float cx = w * 0.5f;

    /* Inset divider: a dark groove with a faint highlight, no card/panel. */
    ui_rect_fill(ui, ui_rect(cx - 1, 0, 2, h), UI_COLOR(4, 7, 10, 255));
    ui_line(ui, cx - 2.5f, 0, cx - 2.5f, h, UI_COLOR(21, 30, 38, 150), 1);
    ui_line(ui, cx + 1.5f, 0, cx + 1.5f, h, UI_COLOR(45, 59, 71, 120), 1);

    ui_text(ui, 28, 30, 20, UI_TEXT, "Symptom Report");
    ui_text(ui, 28, 62, 11, UI_MUTED,
            "The anatomical body map will appear in this half.");
    ui_text(ui, 28, 82, 9.5f, UI_FAINT,
            "Symptom cards will stack in the right half.");
    ui_text(ui, 28, h - 26, 9.5f, UI_FAINT,
            "Ctrl/Cmd+1  RF Check     Ctrl/Cmd+2  Symptom Report");
}

void app_draw(App *app)
{
    Ui *ui = &app->ui;
    UiState s;
    snapshot(app, &s);

    ui_background(ui);
    ui_set_viewport(ui, 0, ui->win_h);
    ui->block_interactive = s.settings_modal_open || s.upload_modal_open;
    draw_tab_bar(app);

    if (s.active_tab == APP_TAB_RF) {
        Layout l;
        ui_set_viewport(ui, TABBAR_H, ui->win_h - (int)TABBAR_H);
        layout(app, &l);
        ui->block_interactive = s.modal_open || s.remove_meter_open ||
                                s.settings_modal_open || s.upload_modal_open ||
                                s.model_modal_open;
        draw_header(app, &s);
        draw_transport(app, &s);
        draw_runtime_banner(app, &s);
        draw_camera_panel(app, &s, l.camera);
        draw_cell_panel(app, &s, l.cells);
        draw_meter_panel(app, &s, l.meters);
        draw_footer(app, &s);
        ui->block_interactive = false;
        if (s.modal_open) draw_modal(app, &s);
        if (s.remove_meter_open) draw_remove_modal(app, &s);
        if (s.model_modal_open) draw_model_modal(app, &s);
    } else {
        ui_set_viewport(ui, TABBAR_H, ui->win_h - (int)TABBAR_H);
        ui->block_interactive = s.settings_modal_open || s.upload_modal_open;
        draw_symptom_placeholder(app);
        ui->block_interactive = false;
    }

    ui_set_viewport(ui, 0, ui->win_h);
    if (s.settings_modal_open) draw_settings_modal(app, &s);
    if (s.upload_modal_open) draw_upload_modal(app, &s);
    draw_toast(app);
}
