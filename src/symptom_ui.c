#include "symptom_ui.h"
#include "anatomy_assets.h"
#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    const char *label;
    int view; /* 0 front, 1 back */
    float x, y, w, h;
} BodyRegion;

static const BodyRegion REGIONS[] = {
    /* front */
    {"Head", 0, 0.38f, 0.02f, 0.24f, 0.14f},
    {"Neck", 0, 0.44f, 0.14f, 0.12f, 0.08f},
    {"Left shoulder", 0, 0.27f, 0.22f, 0.12f, 0.08f},
    {"Right shoulder", 0, 0.61f, 0.22f, 0.12f, 0.08f},
    {"Left chest", 0, 0.36f, 0.23f, 0.14f, 0.11f},
    {"Right chest", 0, 0.50f, 0.23f, 0.14f, 0.11f},
    {"Left abdomen", 0, 0.36f, 0.34f, 0.14f, 0.11f},
    {"Right abdomen", 0, 0.50f, 0.34f, 0.14f, 0.11f},
    {"Groin", 0, 0.43f, 0.45f, 0.14f, 0.07f},
    {"Left upper leg", 0, 0.35f, 0.50f, 0.14f, 0.15f},
    {"Right upper leg", 0, 0.51f, 0.50f, 0.14f, 0.15f},
    {"Left knee", 0, 0.36f, 0.64f, 0.13f, 0.06f},
    {"Right knee", 0, 0.51f, 0.64f, 0.13f, 0.06f},
    {"Left lower leg", 0, 0.36f, 0.70f, 0.13f, 0.15f},
    {"Right lower leg", 0, 0.51f, 0.70f, 0.13f, 0.15f},
    {"Left ankle", 0, 0.37f, 0.85f, 0.11f, 0.05f},
    {"Right ankle", 0, 0.52f, 0.85f, 0.11f, 0.05f},
    {"Left foot", 0, 0.35f, 0.90f, 0.14f, 0.06f},
    {"Right foot", 0, 0.51f, 0.90f, 0.14f, 0.06f},
    {"Left arm", 0, 0.21f, 0.26f, 0.10f, 0.28f},
    {"Left elbow", 0, 0.21f, 0.54f, 0.10f, 0.06f},
    {"Left wrist", 0, 0.21f, 0.60f, 0.10f, 0.05f},
    {"Left hand", 0, 0.19f, 0.65f, 0.13f, 0.10f},
    {"Right arm", 0, 0.69f, 0.26f, 0.10f, 0.28f},
    {"Right elbow", 0, 0.69f, 0.54f, 0.10f, 0.06f},
    {"Right wrist", 0, 0.69f, 0.60f, 0.10f, 0.05f},
    {"Right hand", 0, 0.68f, 0.65f, 0.13f, 0.10f},
    /* back */
    {"Head", 1, 0.38f, 0.02f, 0.24f, 0.14f},
    {"Neck", 1, 0.44f, 0.14f, 0.12f, 0.08f},
    {"Left shoulder", 1, 0.27f, 0.22f, 0.12f, 0.08f},
    {"Right shoulder", 1, 0.61f, 0.22f, 0.12f, 0.08f},
    {"Left upper back", 1, 0.36f, 0.23f, 0.14f, 0.11f},
    {"Right upper back", 1, 0.50f, 0.23f, 0.14f, 0.11f},
    {"Left lower back", 1, 0.36f, 0.34f, 0.14f, 0.11f},
    {"Right lower back", 1, 0.50f, 0.34f, 0.14f, 0.11f},
    {"Left buttock", 1, 0.37f, 0.45f, 0.13f, 0.10f},
    {"Right buttock", 1, 0.50f, 0.45f, 0.13f, 0.10f},
    {"Left upper leg", 1, 0.35f, 0.50f, 0.14f, 0.15f},
    {"Right upper leg", 1, 0.51f, 0.50f, 0.14f, 0.15f},
    {"Left knee", 1, 0.36f, 0.64f, 0.13f, 0.06f},
    {"Right knee", 1, 0.51f, 0.64f, 0.13f, 0.06f},
    {"Left lower leg", 1, 0.36f, 0.70f, 0.13f, 0.15f},
    {"Right lower leg", 1, 0.51f, 0.70f, 0.13f, 0.15f},
    {"Left ankle", 1, 0.37f, 0.85f, 0.11f, 0.05f},
    {"Right ankle", 1, 0.52f, 0.85f, 0.11f, 0.05f},
    {"Left foot", 1, 0.35f, 0.90f, 0.14f, 0.06f},
    {"Right foot", 1, 0.51f, 0.90f, 0.14f, 0.06f},
    {"Left arm", 1, 0.21f, 0.26f, 0.10f, 0.28f},
    {"Left elbow", 1, 0.21f, 0.54f, 0.10f, 0.06f},
    {"Left wrist", 1, 0.21f, 0.60f, 0.10f, 0.05f},
    {"Left hand", 1, 0.19f, 0.65f, 0.13f, 0.10f},
    {"Right arm", 1, 0.69f, 0.26f, 0.10f, 0.28f},
    {"Right elbow", 1, 0.69f, 0.54f, 0.10f, 0.06f},
    {"Right wrist", 1, 0.69f, 0.60f, 0.10f, 0.05f},
    {"Right hand", 1, 0.68f, 0.65f, 0.13f, 0.10f},
};

#define REGION_COUNT ((int)CS_ARRAY_LEN(REGIONS))

typedef struct {
    float bx, by, bw, bh;
    float scale;
    float cx, cy;
} BodyMap;

static float bmx(const BodyMap *m, float x)
{
    return m->bx + (0.5f + (x - m->cx) * m->scale) * m->bw;
}

static float bmy(const BodyMap *m, float y)
{
    return m->by + (0.5f + (y - m->cy) * m->scale) * m->bh;
}

static UiRect region_rect(const BodyMap *m, const BodyRegion *r)
{
    return ui_rect(bmx(m, r->x), bmy(m, r->y), r->w * m->bw * m->scale,
                   r->h * m->bh * m->scale);
}

static int region_at(const BodyMap *m, float mx, float my, int view)
{
    int hit = -1;
    for (int i = 0; i < REGION_COUNT; i++) {
        const BodyRegion *r = &REGIONS[i];
        if (r->view != view) continue;
        UiRect rr = region_rect(m, r);
        if (ui_rect_contains(rr, mx, my)) hit = i;
    }
    return hit;
}

static bool card_is_region(const SymptomCard *c) { return c->kind == SYMPTOM_CARD_REGION; }

static bool region_selected(App *app, int region)
{
    for (int i = 0; i < app->symptom_card_count; i++) {
        const SymptomCard *c = &app->symptom_cards[i];
        if (c->kind == SYMPTOM_CARD_REGION && c->region == region) return true;
        if (c->kind == SYMPTOM_CARD_WHOLE_BODY) return true;
    }
    return false;
}

static bool has_whole_body(App *app)
{
    for (int i = 0; i < app->symptom_card_count; i++)
        if (app->symptom_cards[i].kind == SYMPTOM_CARD_WHOLE_BODY) return true;
    return false;
}

static const char *card_title(const SymptomCard *c)
{
    if (c->kind == SYMPTOM_CARD_WHOLE_BODY) return "Whole body";
    if (c->kind == SYMPTOM_CARD_NON_PHYSICAL) return "Non-physical";
    if (c->region >= 0 && c->region < REGION_COUNT) return REGIONS[c->region].label;
    return "Body region";
}

static bool marker_too_close(App *app, int region, float x, float y)
{
    const BodyRegion *nr = &REGIONS[region];
    for (int i = 0; i < app->symptom_card_count; i++) {
        const SymptomCard *c = &app->symptom_cards[i];
        if (!card_is_region(c) || c->region < 0) continue;
        if (REGIONS[c->region].view != nr->view) continue;
        float dx = c->marker_x - x, dy = c->marker_y - y;
        if (dx * dx + dy * dy < 0.0016f) return true;
    }
    return false;
}

static bool choose_marker(App *app, int region, float px, float py, float *mx, float *my)
{
    *mx = px;
    *my = py;
    if (!marker_too_close(app, region, px, py)) return true;
    static const float dx[] = {0.035f, -0.035f, 0, 0, 0.035f, -0.035f, 0.035f, -0.035f};
    static const float dy[] = {0, 0, 0.035f, -0.035f, 0.035f, 0.035f, -0.035f, -0.035f};
    for (int ring = 0; ring < 3; ring++) {
        for (int i = 0; i < 8; i++) {
            float x = px + dx[i] * (float)(ring + 1);
            float y = py + dy[i] * (float)(ring + 1);
            if (x < 0.02f || x > 0.98f || y < 0.02f || y > 0.98f) continue;
            if (!marker_too_close(app, region, x, y)) {
                *mx = x;
                *my = y;
                return true;
            }
        }
    }
    return false;
}

static void add_card(App *app, SymptomCardKind kind, int region, float px, float py,
                     bool has_point)
{
    if (app->symptom_card_count >= SYMPTOM_MAX_CARDS) return;
    SymptomCard *c = &app->symptom_cards[app->symptom_card_count];
    memset(c, 0, sizeof(*c));
    c->kind = kind;
    c->region = region;
    c->point_x = px;
    c->point_y = py;
    c->has_point = has_point;
    if (kind == SYMPTOM_CARD_REGION && region >= 0) {
        if (!choose_marker(app, region, px, py, &c->marker_x, &c->marker_y)) return;
        if (!has_point) {
            c->marker_x = REGIONS[region].x + REGIONS[region].w * 0.5f;
            c->marker_y = REGIONS[region].y + REGIONS[region].h * 0.5f;
            c->point_x = c->marker_x;
            c->point_y = c->marker_y;
        }
    }
    app->symptom_focus_card = app->symptom_card_count;
    app->symptom_card_count++;
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

static void body_part(Ui *ui, UiRect r, float radius, UiColor fill, UiColor edge)
{
    ui_round_rect(ui, r, radius, edge);
    ui_round_rect(ui, ui_inset(r, 1), radius > 1 ? radius - 1 : radius, fill);
}

static void draw_body(const BodyMap *m, App *app)
{
    Ui *ui = &app->ui;
    UiColor skin = UI_COLOR(48, 62, 77, 255);
    UiColor edge = UI_COLOR(24, 35, 46, 255);
    UiColor muscle = UI_COLOR(79, 98, 116, 150);

    /* Deliberately neutral: no chest or genital detail, no gender markers. */
    UiRect head = ui_rect(bmx(m, 0.41f), bmy(m, 0.02f), 0.18f * m->bw, 0.13f * m->bh);
    body_part(ui, head, 0.075f * m->bw, skin, edge);
    body_part(ui, ui_rect(bmx(m, 0.448f), bmy(m, 0.14f), 0.104f * m->bw, 0.075f * m->bh),
              0.025f * m->bw, skin, edge);
    body_part(ui, ui_rect(bmx(m, 0.335f), bmy(m, 0.19f), 0.33f * m->bw, 0.285f * m->bh),
              0.075f * m->bw, skin, edge);

    for (int side = 0; side < 2; side++) {
        float sx = side ? 0.665f : 0.335f;
        float ax = side ? 0.665f : 0.235f;
        float hx = side ? 0.675f : 0.205f;
        float lx = side ? 0.51f : 0.35f;
        /* shoulder cap */
        ui_circle(ui, bmx(m, sx), bmy(m, 0.225f), 0.052f * m->bw, edge);
        ui_circle(ui, bmx(m, sx), bmy(m, 0.225f), 0.045f * m->bw, skin);
        /* arm */
        body_part(ui, ui_rect(bmx(m, ax), bmy(m, 0.235f), 0.105f * m->bw, 0.39f * m->bh),
                  0.052f * m->bw, skin, edge);
        /* hand */
        body_part(ui, ui_rect(bmx(m, hx), bmy(m, 0.61f), 0.12f * m->bw, 0.105f * m->bh),
                  0.05f * m->bw, skin, edge);
        /* leg */
        body_part(ui, ui_rect(bmx(m, lx), bmy(m, 0.465f), 0.145f * m->bw, 0.45f * m->bh),
                  0.07f * m->bw, skin, edge);
        /* foot */
        body_part(ui, ui_rect(bmx(m, lx - 0.005f), bmy(m, 0.90f), 0.155f * m->bw,
                              0.062f * m->bh),
                  0.03f * m->bw, skin, edge);
    }

    /* Sparse muscle contours. */
    ui_line(ui, bmx(m, 0.5f), bmy(m, 0.235f), bmx(m, 0.5f), bmy(m, 0.455f), muscle, 1.2f);
    if (app->symptom_body_view == 0) {
        for (int i = 0; i < 4; i++) {
            float y = 0.255f + i * 0.05f;
            ui_line(ui, bmx(m, 0.355f), bmy(m, y), bmx(m, 0.645f), bmy(m, y), muscle, 0.9f);
        }
        ui_line(ui, bmx(m, 0.38f), bmy(m, 0.27f), bmx(m, 0.44f), bmy(m, 0.44f), muscle, 1.0f);
        ui_line(ui, bmx(m, 0.62f), bmy(m, 0.27f), bmx(m, 0.56f), bmy(m, 0.44f), muscle, 1.0f);
    } else {
        ui_line(ui, bmx(m, 0.5f), bmy(m, 0.22f), bmx(m, 0.5f), bmy(m, 0.46f), muscle, 1.4f);
        ui_line(ui, bmx(m, 0.38f), bmy(m, 0.27f), bmx(m, 0.46f), bmy(m, 0.40f), muscle, 1.0f);
        ui_line(ui, bmx(m, 0.62f), bmy(m, 0.27f), bmx(m, 0.54f), bmy(m, 0.40f), muscle, 1.0f);
    }
    for (int side = 0; side < 2; side++) {
        float ax = side ? 0.715f : 0.245f;
        float lx = side ? 0.565f : 0.395f;
        ui_line(ui, bmx(m, ax), bmy(m, 0.27f), bmx(m, ax), bmy(m, 0.60f), muscle, 0.9f);
        ui_line(ui, bmx(m, lx), bmy(m, 0.50f), bmx(m, lx), bmy(m, 0.87f), muscle, 0.9f);
        ui_line(ui, bmx(m, lx - 0.025f), bmy(m, 0.65f), bmx(m, lx + 0.025f), bmy(m, 0.65f),
                muscle, 0.9f);
        ui_line(ui, bmx(m, lx - 0.02f), bmy(m, 0.84f), bmx(m, lx + 0.02f), bmy(m, 0.84f),
                muscle, 0.9f);
    }
}

static void draw_region_overlays(const BodyMap *m, App *app, int hover)
{
    Ui *ui = &app->ui;
    bool whole = has_whole_body(app);
    for (int i = 0; i < REGION_COUNT; i++) {
        const BodyRegion *r = &REGIONS[i];
        if (r->view != app->symptom_body_view) continue;
        UiRect rr = region_rect(m, r);
        if (whole || region_selected(app, i)) {
            ui_round_rect(ui, rr, 4, UI_COLOR(225, 72, 82, 180));
        } else if (app->symptom_zoom_region == i) {
            ui_round_rect(ui, rr, 4, UI_COLOR(225, 72, 82, 70));
            ui_round_rect(ui, rr, 4, UI_COLOR(225, 120, 120, 120));
        } else if (i == hover) {
            ui_round_rect(ui, rr, 4, UI_COLOR(225, 170, 78, 65));
        }
    }
}

static void commit_pending(App *app)
{
    if (app->symptom_pending_region < 0) return;
    add_card(app, SYMPTOM_CARD_REGION, app->symptom_pending_region,
             app->symptom_pending_x, app->symptom_pending_y, true);
    app->symptom_pending_region = -1;
}

static void draw_markers(const BodyMap *m, App *app)
{
    Ui *ui = &app->ui;
    for (int i = 0; i < app->symptom_card_count; i++) {
        SymptomCard *c = &app->symptom_cards[i];
        if (!card_is_region(c) || c->region < 0) continue;
        const BodyRegion *r = &REGIONS[c->region];
        if (r->view != app->symptom_body_view) continue;
        float px = bmx(m, c->point_x), py = bmy(m, c->point_y);
        float mx = bmx(m, c->marker_x), my = bmy(m, c->marker_y);
        if (fabsf(px - mx) > 1.0f || fabsf(py - my) > 1.0f)
            ui_line(ui, px, py, mx, my, UI_COLOR(225, 72, 82, 180), 1.2f);
        ui_circle(ui, mx, my, 8, UI_COLOR(225, 72, 82, 255));
        ui_circle(ui, mx, my, 10, UI_COLOR(255, 229, 229, 160));
    }
}

static void draw_card_list(App *app, float x, float y, float w, float h)
{
    Ui *ui = &app->ui;
    ui_text(ui, x, y, 15, UI_TEXT, "Symptom cards");
    ui_text(ui, x, y + 22, 9.5f, UI_MUTED, "Each click on the body adds a card.");

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
        if (c->kind == SYMPTOM_CARD_REGION && c->region >= 0) {
            snprintf(sub, sizeof(sub), "%s view · point stored",
                     REGIONS[c->region].view == 0 ? "Front" : "Back");
        } else {
            cs_strlcpy(sub, c->kind == SYMPTOM_CARD_WHOLE_BODY
                                ? "All 55 selections"
                                : "No body region",
                       sizeof(sub));
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
                "Select a body region to create the first card.");
}

static void draw_view_toggle(App *app, UiRect r, const char *label, int view)
{
    Ui *ui = &app->ui;
    bool active = app->symptom_body_view == view;
    bool hover = ui_hover(ui, r);
    UiColor bg = active ? UI_COLOR(32, 48, 60, 255)
                        : (hover ? UI_COLOR(18, 27, 35, 255) : UI_COLOR(12, 18, 24, 255));
    UiColor border = active ? UI_ACCENT : UI_BORDER;
    ui_round_rect(ui, r, 8, border);
    ui_round_rect(ui, ui_inset(r, 1), 7, bg);
    ui_text_center(ui, FONT_SEMIBOLD, r.x + r.w / 2, r.y + 9, 11.0f,
                   active ? UI_TEXT : UI_MUTED, label);
    if (!active && hover && ui->mouse_pressed) {
        app->symptom_body_view = view;
        app->symptom_zoom_region = -1;
        app->symptom_pending_region = -1;
        ui_consume_click(ui);
    }
}

void symptom_ui_draw(App *app)
{
    Ui *ui = &app->ui;
    float w = (float)ui->win_w, h = (float)ui->view_h;
    float split = w * 0.5f;
    float left_w = split;

    /* Divider */
    ui_rect_fill(ui, ui_rect(split - 1, 0, 2, h), UI_COLOR(4, 7, 10, 255));
    ui_line(ui, split - 2.5f, 0, split - 2.5f, h, UI_COLOR(21, 30, 38, 150), 1);
    ui_line(ui, split + 1.5f, 0, split + 1.5f, h, UI_COLOR(45, 59, 71, 120), 1);

    /* Top-left symptom actions */
    UiRect whole = ui_rect(18, 14, 198, 30);
    if (ui_button(ui, whole, "Add whole body symptom", BUTTON_SECONDARY, true)) {
        add_card(app, SYMPTOM_CARD_WHOLE_BODY, -1, 0, 0, false);
    }
    UiRect nonphysical = ui_rect(18, 50, 198, 30);
    if (ui_button(ui, nonphysical, "Add non-physical symptom", BUTTON_SECONDARY, true)) {
        add_card(app, SYMPTOM_CARD_NON_PHYSICAL, -1, 0, 0, false);
    }

    /* Front/back toggles */
    float tx = left_w - 172;
    draw_view_toggle(app, ui_rect(tx, 14, 72, 30), "Front", 0);
    draw_view_toggle(app, ui_rect(tx + 80, 14, 72, 30), "Back", 1);

    float body_y = 96;
    float body_h = h - body_y - 18;
    float body_w = body_h * 0.48f;
    if (body_w > left_w - 100) body_w = left_w - 100;
    float body_x = (left_w - body_w) * 0.5f;

    BodyMap m = {body_x, body_y, body_w, body_h, 1.0f, 0.5f, 0.5f};
    if (app->symptom_zoom_region >= 0 && app->symptom_zoom_region < REGION_COUNT) {
        const BodyRegion *zr = &REGIONS[app->symptom_zoom_region];
        m.scale = 2.2f;
        m.cx = zr->x + zr->w * 0.5f;
        m.cy = zr->y + zr->h * 0.5f;
    }
    float mx = ui->mx;
    float my = ui->my - ui->origin_y;
    int hover = -1;
    if (app->symptom_zoom_region < 0 && mx >= m.bx && mx <= m.bx + m.bw &&
        my >= m.by && my <= m.by + m.bh) {
        int hit = region_at(&m, mx, my, app->symptom_body_view);
        if (hit >= 0) hover = hit;
    }
    UiRect body_clip = ui_rect(0, body_y - 4, left_w, body_h + 8);
    ui_push_clip(ui, body_clip);
    draw_region_overlays(&m, app, hover);
    draw_body(&m, app);
    draw_region_overlays(&m, app, hover);
    draw_markers(&m, app);
    ui_pop_clip(ui);

    /* Marker clicks focus their card. */
    if (app->symptom_zoom_region < 0 && ui->mouse_pressed && mx >= m.bx && mx <= m.bx + m.bw &&
        my >= m.by && my <= m.by + m.bh) {
        for (int i = 0; i < app->symptom_card_count; i++) {
            SymptomCard *c = &app->symptom_cards[i];
            if (!card_is_region(c) || c->region < 0 ||
                REGIONS[c->region].view != app->symptom_body_view) continue;
            float cx = bmx(&m, c->marker_x), cy = bmy(&m, c->marker_y);
            if ((mx - cx) * (mx - cx) + (my - cy) * (my - cy) <= 144) {
                app->symptom_focus_card = i;
                ui_consume_click(ui);
                return;
            }
        }
    }

    if (app->symptom_zoom_region >= 0) {
        UiRect close = ui_rect(split - 96, body_y + 8, 72, 28);
        if (ui_button(ui, close, "Close", BUTTON_SECONDARY, true)) {
            app->symptom_zoom_region = -1;
            return;
        }
        if (ui->mouse_pressed && mx >= m.bx && mx <= m.bx + m.bw && my >= m.by &&
            my <= m.by + m.bh) {
            float nx = m.cx + (mx - (m.bx + m.bw * 0.5f)) / (m.bw * m.scale);
            float ny = m.cy + (my - (m.by + m.bh * 0.5f)) / (m.bh * m.scale);
            if (nx >= 0 && nx <= 1 && ny >= 0 && ny <= 1) {
                add_card(app, SYMPTOM_CARD_REGION, app->symptom_zoom_region, nx, ny, true);
                ui_consume_click(ui);
            }
        }
    } else if (ui->mouse_pressed && mx >= m.bx && mx <= m.bx + m.bw && my >= m.by &&
               my <= m.by + m.bh) {
        int hit = region_at(&m, mx, my, app->symptom_body_view);
        if (hit >= 0) {
            int64_t now = cs_epoch_ms();
            if (app->symptom_pending_region == hit && now - app->symptom_pending_ms < 330) {
                app->symptom_zoom_region = hit;
                app->symptom_pending_region = -1;
            } else {
                commit_pending(app);
                app->symptom_pending_region = hit;
                app->symptom_pending_ms = now;
                app->symptom_pending_x = (mx - m.bx) / m.bw;
                app->symptom_pending_y = (my - m.by) / m.bh;
            }
            ui_consume_click(ui);
        }
    }

    if (app->symptom_pending_region >= 0 && cs_epoch_ms() - app->symptom_pending_ms > 260)
        commit_pending(app);

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
            if (ui_button(ui, retry, "Retry", BUTTON_SECONDARY, true)) {
                anatomy_assets_start(app);
            }
        }
    } else if (ready && status[0]) {
        ui_text(ui, 22, h - 24, 8.5f, UI_FAINT, status);
    }
}
