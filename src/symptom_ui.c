#include "symptom_ui.h"
#include "anatomy_assets.h"
#include "anatomy_3d.h"
#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static const char *card_title(const SymptomCard *c)
{
    if (c->kind == SYMPTOM_CARD_WHOLE_BODY) return "Whole body";
    if (c->kind == SYMPTOM_CARD_NON_PHYSICAL) return "Non-physical";
    if (c->kind == SYMPTOM_CARD_MUSCLE) return c->code[0] ? c->code : "Muscle";
    return "Symptom";
}

static void add_card(App *app, SymptomCardKind kind)
{
    if (app->symptom_card_count >= SYMPTOM_MAX_CARDS) return;
    SymptomCard *c = &app->symptom_cards[app->symptom_card_count];
    memset(c, 0, sizeof(*c));
    c->kind = kind;
    c->region = -1;
    app->symptom_focus_card = app->symptom_card_count;
    app->symptom_card_count++;
}

void symptom_add_muscle_card(App *app, const AnatomyPick *pick)
{
    if (!app || !pick || app->symptom_card_count >= SYMPTOM_MAX_CARDS) return;
    add_card(app, SYMPTOM_CARD_MUSCLE);
    SymptomCard *c = &app->symptom_cards[app->symptom_card_count - 1];
    cs_strlcpy(c->code, pick->code, sizeof(c->code));
    cs_strlcpy(c->region_name, pick->region, sizeof(c->region_name));
    c->side = pick->side;
    c->point_x = pick->x;
    c->point_y = pick->y;
    c->point_z = pick->z;
    c->has_point = true;
    app->symptom_focus_card = app->symptom_card_count - 1;
}

static void remove_card(App *app, int index)
{
    if (index < 0 || index >= app->symptom_card_count) return;
    memmove(&app->symptom_cards[index], &app->symptom_cards[index + 1],
            sizeof(app->symptom_cards[0]) * (app->symptom_card_count - index - 1));
    app->symptom_card_count--;
    if (app->symptom_focus_card >= app->symptom_card_count)
        app->symptom_focus_card = app->symptom_card_count - 1;
}

static void draw_card_list(App *app, float x, float y, float w, float h)
{
    Ui *ui = &app->ui;
    ui_text(ui, x, y, 15, UI_TEXT, "Symptom cards");
    ui_text(ui, x, y + 22, 9.5f, UI_MUTED,
            "Click a muscle on the 3D model to add a card.");

    float list_y = y + 46;
    float list_h = h - 46;
    UiRect list = ui_rect(x, list_y, w, list_h);
    if (ui_hit(ui, list) && ui->wheel != 0) {
        app->symptom_scroll -= (int)ui->wheel * 36;
        if (app->symptom_scroll < 0) app->symptom_scroll = 0;
        ui->wheel = 0;
    }
    int card_h = 64, gap = 9;
    int total = app->symptom_card_count * (card_h + gap);
    int max_scroll = total - (int)list_h;
    if (max_scroll < 0) max_scroll = 0;
    if (app->symptom_scroll > max_scroll) app->symptom_scroll = max_scroll;

    ui_push_clip(ui, list);
    float cy = list_y - app->symptom_scroll;
    for (int i = 0; i < app->symptom_card_count; i++) {
        SymptomCard *c = &app->symptom_cards[i];
        UiRect r = ui_rect(x, cy, w, card_h);
        bool focused = i == app->symptom_focus_card;
        ui_round_rect(ui, r, 8, focused ? UI_COLOR(30, 44, 55, 255) : UI_PANEL_HI);
        ui_round_rect(ui, ui_rect(r.x, r.y, 3, r.h), 2,
                      focused ? UI_ACCENT : UI_BORDER);
        ui_text_ellipsis(ui, FONT_SEMIBOLD, r.x + 14, r.y + 10, 11.5f, UI_TEXT,
                         r.w - 100, card_title(c));
        char sub[128];
        if (c->kind == SYMPTOM_CARD_MUSCLE) {
            snprintf(sub, sizeof(sub), "%s · %s · 3D %.2f, %.2f, %.2f",
                     c->region_name[0] ? c->region_name : "Muscle",
                     c->side ? "male" : "female",
                     c->point_x, c->point_y, c->point_z);
        } else if (c->kind == SYMPTOM_CARD_WHOLE_BODY) {
            cs_strlcpy(sub, "Entire body symptom", sizeof(sub));
        } else if (c->kind == SYMPTOM_CARD_NON_PHYSICAL) {
            cs_strlcpy(sub, "Non-physical symptom", sizeof(sub));
        } else {
            cs_strlcpy(sub, "Symptom", sizeof(sub));
        }
        ui_text_ellipsis(ui, FONT_UI, r.x + 14, r.y + 31, 9.5f, UI_MUTED,
                         r.w - 110, sub);
        ui_text(ui, r.x + 14, r.y + 46, 9.0f, UI_FAINT,
                "Symptom controls arrive in the next feature.");
        UiRect rm = ui_rect(r.x + r.w - 78, r.y + 15, 64, 28);
        if (ui_button(ui, rm, "Remove", BUTTON_SECONDARY, true)) {
            remove_card(app, i);
            break;
        }
        if (ui_hit(ui, ui_rect(r.x, r.y, r.w - 84, r.h)) && ui->mouse_pressed)
            app->symptom_focus_card = i;
        cy += card_h + gap;
    }
    ui_pop_clip(ui);
    if (app->symptom_card_count == 0)
        ui_text(ui, x, list_y + 12, 10.5f, UI_FAINT,
                "Select a muscle on the 3D model to create the first card.");
}

void symptom_ui_draw(App *app)
{
    Ui *ui = &app->ui;
    float w = (float)ui->win_w, h = (float)ui->view_h;
    float split = w * 0.5f;
    float left_w = split;
    bool has3d = anatomy_3d_available(app->anatomy);

    /* Divider */
    ui_rect_fill(ui, ui_rect(split - 1, 0, 2, h), UI_COLOR(4, 7, 10, 255));
    ui_line(ui, split - 2.5f, 0, split - 2.5f, h, UI_COLOR(21, 30, 38, 150), 1);
    ui_line(ui, split + 1.5f, 0, split + 1.5f, h, UI_COLOR(45, 59, 71, 120), 1);

    if (has3d) {
        GpuTexture *tex = anatomy_3d_texture(app->anatomy);
        if (tex)
            gpu_renderer_texture_rect(ui->r, tex,
                                      (GpuRect){0, ui->origin_y, left_w, h}, UI_WHITE);
    } else {
        ui_text_center(ui, FONT_SEMIBOLD, left_w * 0.5f, h * 0.42f, 16.0f, UI_TEXT,
                       "Anatomical model unavailable");
        ui_text_center(ui, FONT_UI, left_w * 0.5f, h * 0.42f + 26, 10.5f, UI_MUTED,
                       "Check the download status below or retry.");
    }

    /* Actions sit above the viewport, at its top corners. */
    UiRect whole = ui_rect(18, 14, 198, 30);
    if (ui_button(ui, whole, "Add whole body symptom", BUTTON_SECONDARY, true))
        add_card(app, SYMPTOM_CARD_WHOLE_BODY);
    UiRect nonphysical = ui_rect(left_w - 18 - 198, 14, 198, 30);
    if (ui_button(ui, nonphysical, "Add non-physical symptom", BUTTON_SECONDARY, true))
        add_card(app, SYMPTOM_CARD_NON_PHYSICAL);

    if (has3d) {
        int sel = anatomy_3d_selection(app->anatomy);
        ui_text_center(ui, FONT_MONO, left_w * 0.30f, h - 22, 10.0f,
                       sel == 0 ? UI_ACCENT : UI_MUTED, "Female");
        ui_text_center(ui, FONT_MONO, left_w * 0.70f, h - 22, 10.0f,
                       sel == 1 ? UI_ACCENT : UI_MUTED, "Male");
        float mx = 0, my = 0;
        if (anatomy_3d_marker(app, &mx, &my)) {
            ui_circle(ui, mx, my, 10, UI_COLOR(255, 229, 229, 160));
            ui_circle(ui, mx, my, 7, UI_COLOR(225, 72, 82, 255));
            ui_circle(ui, mx, my, 3, UI_COLOR(255, 255, 255, 255));
        }
    }

    draw_card_list(app, split + 22, 18, w - split - 44, h - 36);

    bool downloading, failed, ready;
    int percent;
    char status[256];
    SDL_LockMutex(app->mu);
    downloading = app->anatomy_downloading;
    failed = app->anatomy_failed;
    ready = app->anatomy_ready;
    percent = app->anatomy_percent;
    cs_strlcpy(status, app->anatomy_status, sizeof(status));
    SDL_UnlockMutex(app->mu);
    if (downloading || failed) {
        float by = h - 58;
        UiRect bar = ui_rect(22, by, left_w - 44, 34);
        ui_round_rect(ui, bar, 8, UI_PANEL_HI);
        if (downloading) {
            ui_text_ellipsis(ui, FONT_MONO, bar.x + 12, bar.y + 8, 9.0f, UI_MUTED,
                             bar.w - 24, status[0] ? status : "Downloading anatomical model");
            UiRect track = ui_rect(bar.x + 12, bar.y + 24, bar.w - 24, 3);
            ui_round_rect(ui, track, 1.5f, UI_BORDER);
            float frac = (float)(percent < 0 ? 0 : percent > 100 ? 100 : percent) / 100.0f;
            ui_round_rect(ui, ui_rect(track.x, track.y, track.w * frac, track.h), 1.5f,
                          UI_ACCENT);
        } else {
            ui_text_ellipsis(ui, FONT_MONO, bar.x + 12, bar.y + 8, 9.0f, UI_RED,
                             bar.w - 100, status[0] ? status : "Anatomy download failed");
            UiRect retry = ui_rect(bar.x + bar.w - 82, bar.y + 4, 70, 26);
            if (ui_button(ui, retry, "Retry", BUTTON_SECONDARY, true))
                anatomy_assets_start(app);
        }
    } else if (ready && status[0]) {
        ui_text(ui, 22, h - 24, 8.5f, UI_FAINT, status);
    }
}
