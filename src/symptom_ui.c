#include "symptom_ui.h"
#include "anatomy_assets.h"
#include "anatomy_3d.h"
#include "symptom_catalog.h"
#include "ui.h"

#include <ctype.h>
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
    c->sub_count = 1;
    c->active_sub = 0;
    c->subs[0].severity = 5;
    c->subs[0].onset_hours = 1;
    c->subs[0].duration_hours = 1;
    c->subs[0].frequency = 0;
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
    if (app->symptom_edit_card == index) {
        app->symptom_edit_card = -1;
        app->symptom_edit_sub = -1;
        app->symptom_edit_field = -1;
        SDL_StopTextInput(app->win);
    }
}


static int fuzzy_score(const char *query, const char *candidate)
{
    if (!query || !*query) return 1;
    char q[SYMPTOM_NAME_MAX], c[SYMPTOM_NAME_MAX];
    size_t qn = strlen(query), cn = strlen(candidate);
    if (qn >= sizeof(q)) qn = sizeof(q) - 1;
    if (cn >= sizeof(c)) cn = sizeof(c) - 1;
    for (size_t i = 0; i < qn; i++) q[i] = (char)tolower((unsigned char)query[i]);
    for (size_t i = 0; i < cn; i++) c[i] = (char)tolower((unsigned char)candidate[i]);
    q[qn] = c[cn] = '\0';
    if (strncmp(q, c, qn) == 0) return 1000 - (int)qn;
    char *sub = strstr(c, q);
    if (sub) return 500 - (int)(sub - c);
    size_t j = 0;
    for (size_t i = 0; i < cn && j < qn; i++)
        if (c[i] == q[j]) j++;
    return j == qn ? 100 : 0;
}

static int search_collect(const char *query, const char **out, int max)
{
    int scores[SYMPTOM_CATALOG_MAX];
    const char *names[SYMPTOM_CATALOG_MAX];
    int count = 0;
    for (int i = 0; i < symptom_catalog_count(); i++) {
        const SymptomDef *def = symptom_catalog_at(i);
        if (!def) continue;
        int sc = fuzzy_score(query, def->name);
        if (sc <= 0) continue;
        int at = count;
        while (at > 0 && scores[at - 1] < sc) {
            scores[at] = scores[at - 1];
            names[at] = names[at - 1];
            at--;
        }
        scores[at] = sc;
        names[at] = def->name;
        if (count < SYMPTOM_CATALOG_MAX) count++;
    }
    if (count > max) count = max;
    for (int i = 0; i < count; i++) out[i] = names[i];
    return count;
}

static char *edit_buffer(App *app)
{
    if (app->symptom_edit_card < 0 || app->symptom_edit_sub < 0 ||
        app->symptom_edit_field < 0)
        return NULL;
    SymptomCard *c = &app->symptom_cards[app->symptom_edit_card];
    if (app->symptom_edit_sub >= c->sub_count) return NULL;
    SymptomSubsection *sub = &c->subs[app->symptom_edit_sub];
    switch (app->symptom_edit_field) {
        case 0: return sub->query;
        case 1: return sub->description;
        case 2: return sub->triggers;
        case 3: return sub->relief;
        case 4: return sub->notes;
        default: return NULL;
    }
}

static void begin_edit(App *app, int card, int sub, int field)
{
    if (card < 0 || card >= app->symptom_card_count) return;
    SymptomCard *c = &app->symptom_cards[card];
    if (sub < 0 || sub >= c->sub_count) return;
    app->symptom_edit_card = card;
    app->symptom_edit_sub = sub;
    app->symptom_edit_field = field;
    app->symptom_search_sel = 0;
    c->active_sub = sub;
    if (field == 0 && c->subs[sub].query[0] == '\0')
        cs_strlcpy(c->subs[sub].query, c->subs[sub].name, sizeof(c->subs[sub].query));
    SDL_StartTextInput(app->win);
}

static void end_edit(App *app)
{
    app->symptom_edit_card = -1;
    app->symptom_edit_sub = -1;
    app->symptom_edit_field = -1;
    SDL_StopTextInput(app->win);
}

void symptom_ui_cancel_edit(App *app)
{
    if (app->symptom_edit_card >= 0) end_edit(app);
}

static bool field_active(App *app, int card, int sub, int field)
{
    return app->symptom_edit_card == card && app->symptom_edit_sub == sub &&
           app->symptom_edit_field == field;
}

static float sub_height(App *app, SymptomCard *c, int index)
{
    SymptomSubsection *sub = &c->subs[index];
    float h = 24 + 34;
    if (field_active(app, (int)(c - app->symptom_cards), index, 0)) {
        const char *results[5];
        h += search_collect(sub->query, results, 5) * 21;
    }
    if (sub->name[0]) {
        if (sub->controls & SYM_CTRL_SEVERITY) h += 30;
        if (sub->controls & (SYM_CTRL_ONSET | SYM_CTRL_DURATION)) h += 30;
        if (sub->controls & SYM_CTRL_PATTERN) h += 34;
        if (sub->controls & SYM_CTRL_DESCRIPTION) h += 40;
        if (sub->controls & SYM_CTRL_TRIGGERS) h += 40;
        if (sub->controls & SYM_CTRL_RELIEF) h += 40;
        if (sub->controls & SYM_CTRL_NOTES) h += 40;
        h += 12;
    }
    return h;
}

static float card_height(App *app, SymptomCard *c)
{
    if (c->minimized) return 56;
    float h = 56;
    for (int i = 0; i < c->sub_count; i++) h += sub_height(app, c, i);
    h += 40;
    return h;
}

static void draw_field(App *app, UiRect r, int field, int card, int sub, const char *label,
                       const char *value, const char *placeholder)
{
    Ui *ui = &app->ui;
    bool active = field_active(app, card, sub, field);
    ui_text_font(ui, FONT_MONO, r.x, r.y - 12, 8.2f, active ? UI_ACCENT : UI_FAINT, label);
    ui_round_rect(ui, r, 5, UI_COLOR(8, 14, 19, 255));
    ui_rect_outline(ui, r, active ? UI_ACCENT : UI_BORDER_SOFT, 1);
    ui_text_ellipsis(ui, FONT_UI, r.x + 8, r.y + 7, 9.2f, value[0] ? UI_TEXT : UI_FAINT,
                     r.w - 16, value[0] ? value : placeholder);
    if (ui_hit(ui, r) && ui->mouse_pressed) {
        begin_edit(app, card, sub, field);
        ui_consume_click(ui);
    }
}

static void draw_card(App *app, SymptomCard *c, int index, UiRect r)
{
    Ui *ui = &app->ui;
    bool focused = index == app->symptom_focus_card;
    ui_round_rect(ui, r, 8, focused ? UI_COLOR(30, 44, 55, 255) : UI_PANEL_HI);
    ui_round_rect(ui, ui_rect(r.x, r.y, 3, r.h), 2, focused ? UI_ACCENT : UI_BORDER);

    ui_text_ellipsis(ui, FONT_SEMIBOLD, r.x + 14, r.y + 10, 11.5f, UI_TEXT,
                     r.w - 172, card_title(c));
    char sub[128];
    if (c->kind == SYMPTOM_CARD_MUSCLE) {
        snprintf(sub, sizeof(sub), "%s · %s",
                 c->region_name[0] ? c->region_name : "Muscle",
                 c->side ? "male" : "female");
    } else if (c->kind == SYMPTOM_CARD_WHOLE_BODY) {
        cs_strlcpy(sub, "Entire body symptom", sizeof(sub));
    } else {
        cs_strlcpy(sub, "Non-physical symptom", sizeof(sub));
    }
    ui_text_ellipsis(ui, FONT_UI, r.x + 14, r.y + 30, 9.2f, UI_MUTED,
                     r.w - 172, sub);

    UiRect min = ui_rect(r.x + r.w - 158, r.y + 14, 30, 26);
    if (ui_button(ui, min, c->minimized ? "+" : "−", BUTTON_SECONDARY, true)) {
        c->minimized = !c->minimized;
        if (c->minimized && app->symptom_edit_card == index) end_edit(app);
    }
    UiRect rm = ui_rect(r.x + r.w - 120, r.y + 14, 72, 26);
    if (ui_button(ui, rm, "Remove", BUTTON_SECONDARY, true)) {
        remove_card(app, index);
        return;
    }
    if (ui_hit(ui, ui_rect(r.x, r.y, r.w - 164, 52)) && ui->mouse_pressed) {
        app->symptom_focus_card = index;
        ui_consume_click(ui);
    }
    if (c->minimized) return;

    float y = r.y + 56;
    for (int si = 0; si < c->sub_count; si++) {
        SymptomSubsection *ss = &c->subs[si];
        ui_text_font(ui, FONT_SEMIBOLD, r.x + 14, y, 9.5f, UI_FAINT, "Symptom");
        y += 22;

        UiRect search = ui_rect(r.x + 14, y, r.w - 28, 28);
        bool searching = field_active(app, index, si, 0);
        ui_round_rect(ui, search, 5, UI_COLOR(8, 14, 19, 255));
        ui_rect_outline(ui, search, searching ? UI_ACCENT : UI_BORDER_SOFT, 1);
        const char *shown = searching && ss->query[0] ? ss->query : ss->name;
        ui_text_ellipsis(ui, FONT_UI, search.x + 8, search.y + 7, 9.4f,
                         shown[0] ? UI_TEXT : UI_FAINT, search.w - 16,
                         shown[0] ? shown : "Search symptoms…");
        if (ui_hit(ui, search) && ui->mouse_pressed) {
            begin_edit(app, index, si, 0);
            ui_consume_click(ui);
        }
        y += 34;

        if (searching) {
            const char *results[5];
            int n = search_collect(ss->query, results, 5);
            for (int ri = 0; ri < n; ri++) {
                UiRect rr = ui_rect(r.x + 14, y, r.w - 28, 20);
                bool sel = ri == app->symptom_search_sel;
                if (sel) ui_round_rect(ui, rr, 4, UI_ACCENT_SOFT);
                ui_text_ellipsis(ui, FONT_UI, rr.x + 8, rr.y + 4, 9.0f,
                                 sel ? UI_TEXT : UI_MUTED, rr.w - 16, results[ri]);
                if (ui_hit(ui, rr) && ui->mouse_pressed) {
                    cs_strlcpy(ss->name, results[ri], sizeof(ss->name));
                    ss->query[0] = '\0';
                    const SymptomDef *def = symptom_catalog_find(ss->name);
                    ss->controls = def ? def->controls : SYM_CTRL_ALL;
                    app->symptom_focus_card = index;
                    end_edit(app);
                    ui_consume_click(ui);
                }
                y += 21;
            }
            if (n == 0) {
                ui_text(ui, r.x + 22, y + 2, 9.0f, UI_MUTED, "No matching symptom");
                y += 21;
            }
        }

        if (ss->name[0]) {
            float half = (r.w - 36) * 0.5f;
            char val[32];
            if (ss->controls & SYM_CTRL_SEVERITY) {
                UiRect minus = ui_rect(r.x + 14, y, 26, 24);
                UiRect plus = ui_rect(r.x + 44, y, 26, 24);
                ui_text_font(ui, FONT_UI, r.x + 80, y + 5, 9.4f, UI_MUTED, "Severity");
                snprintf(val, sizeof(val), "%d / 10", ss->severity);
                ui_text_font(ui, FONT_MONO, r.x + 148, y + 5, 9.4f, UI_TEXT, val);
                if (ui_button(ui, minus, "−", BUTTON_QUIET, ss->severity > 0))
                    ss->severity--;
                if (ui_button(ui, plus, "+", BUTTON_QUIET, ss->severity < 10))
                    ss->severity++;
                y += 30;
            }

            if (ss->controls & (SYM_CTRL_ONSET | SYM_CTRL_DURATION)) {
                if (ss->controls & SYM_CTRL_ONSET) {
                    UiRect ominus = ui_rect(r.x + 14, y, 26, 24);
                    UiRect oplus = ui_rect(r.x + 44, y, 26, 24);
                    ui_text_font(ui, FONT_UI, r.x + 80, y + 5, 9.2f, UI_MUTED, "Onset");
                    snprintf(val, sizeof(val), "%dh ago", ss->onset_hours);
                    ui_text_font(ui, FONT_MONO, r.x + 128, y + 5, 9.2f, UI_TEXT, val);
                    if (ui_button(ui, ominus, "−", BUTTON_QUIET, ss->onset_hours > 0))
                        ss->onset_hours--;
                    if (ui_button(ui, oplus, "+", BUTTON_QUIET, ss->onset_hours < 24))
                        ss->onset_hours++;
                }
                if (ss->controls & SYM_CTRL_DURATION) {
                    float dx = (ss->controls & SYM_CTRL_ONSET) ? half + 18 : 14;
                    UiRect dminus = ui_rect(r.x + dx, y, 26, 24);
                    UiRect dplus = ui_rect(r.x + dx + 30, y, 26, 24);
                    ui_text_font(ui, FONT_UI, r.x + dx + 66, y + 5, 9.2f, UI_MUTED, "Duration");
                    snprintf(val, sizeof(val), "%dh", ss->duration_hours);
                    ui_text_font(ui, FONT_MONO, r.x + dx + 128, y + 5, 9.2f, UI_TEXT, val);
                    if (ui_button(ui, dminus, "−", BUTTON_QUIET, ss->duration_hours > 0))
                        ss->duration_hours--;
                    if (ui_button(ui, dplus, "+", BUTTON_QUIET, ss->duration_hours < 24))
                        ss->duration_hours++;
                }
                y += 30;
            }

            if (ss->controls & SYM_CTRL_PATTERN) {
                static const char *kFreq[] = {"Constant", "Intermittent", "Occasional"};
                char flabel[48];
                snprintf(flabel, sizeof(flabel), "Pattern: %s", kFreq[ss->frequency % 3]);
                UiRect freq = ui_rect(r.x + 14, y, 166, 24);
                if (ui_button(ui, freq, flabel, BUTTON_SECONDARY, true)) {
                    ss->frequency = (ss->frequency + 1) % 3;
                    app->symptom_focus_card = index;
                }
                y += 34;
            }

            static const struct { unsigned bit; const char *label; const char *hint; } kFields[] = {
                {SYM_CTRL_DESCRIPTION, "Description", "Add detail…"},
                {SYM_CTRL_TRIGGERS, "Triggers", "Add detail…"},
                {SYM_CTRL_RELIEF, "Relief", "Add detail…"},
                {SYM_CTRL_NOTES, "Notes", "Anything else?"},
            };
            char *vals[] = {ss->description, ss->triggers, ss->relief, ss->notes};
            for (int f = 0; f < 4; f++) {
                if (!(ss->controls & kFields[f].bit)) continue;
                UiRect fr = ui_rect(r.x + 14, y + 12, r.w - 28, 26);
                draw_field(app, fr, f + 1, index, si, kFields[f].label, vals[f], kFields[f].hint);
                y += 40;
            }
            y += 8;
        } else {
            ui_text(ui, r.x + 14, y + 2, 9.2f, UI_FAINT,
                    "Choose a symptom to add controls.");
            y += 24;
        }
    }

    UiRect add = ui_rect(r.x + 14, y, r.w - 28, 30);
    if (c->sub_count < SYMPTOM_MAX_SUBS) {
        if (ui_button(ui, add, "Add another symptom", BUTTON_QUIET, true)) {
            int si = c->sub_count++;
            memset(&c->subs[si], 0, sizeof(c->subs[si]));
            c->subs[si].severity = 5;
            c->subs[si].onset_hours = 1;
            c->subs[si].duration_hours = 1;
            app->symptom_focus_card = index;
            begin_edit(app, index, si, 0);
        }
    } else {
        ui_text(ui, r.x + 14, y + 7, 9.0f, UI_FAINT,
                "Maximum of 4 symptoms per card");
    }
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
        app->symptom_scroll -= (int)ui->wheel * 42;
        if (app->symptom_scroll < 0) app->symptom_scroll = 0;
        ui->wheel = 0;
    }
    float total = 0;
    for (int i = 0; i < app->symptom_card_count; i++)
        total += card_height(app, &app->symptom_cards[i]) + 9;
    int max_scroll = (int)total - (int)list_h;
    if (max_scroll < 0) max_scroll = 0;
    if (app->symptom_scroll > max_scroll) app->symptom_scroll = max_scroll;

    ui_push_clip(ui, list);
    float cy = list_y - app->symptom_scroll;
    for (int i = 0; i < app->symptom_card_count; i++) {
        SymptomCard *c = &app->symptom_cards[i];
        float ch = card_height(app, c);
        draw_card(app, c, i, ui_rect(x, cy, w, ch));
        if (i >= app->symptom_card_count || &app->symptom_cards[i] != c) break;
        cy += ch + 9;
    }
    ui_pop_clip(ui);
    if (app->symptom_card_count == 0)
        ui_text(ui, x, list_y + 12, 10.5f, UI_FAINT,
                "Select a muscle on the 3D model to create the first card.");
}

bool symptom_ui_handle_event(App *app, const SDL_Event *e)
{
    if (!app || !e) return false;
    if (app->symptom_edit_card < 0) return false;
    if (e->type == SDL_EVENT_TEXT_INPUT) {
        char *buf = edit_buffer(app);
        if (!buf) return true;
        size_t n = strlen(buf), add = strlen(e->text.text);
        size_t cap = app->symptom_edit_field == 0 ? SYMPTOM_NAME_MAX : SYMPTOM_TEXT_MAX;
        if (n + add >= cap) add = cap - 1 - n;
        memcpy(buf + n, e->text.text, add);
        buf[n + add] = '\0';
        return true;
    }
    if (e->type != SDL_EVENT_KEY_DOWN) return false;
    char *buf = edit_buffer(app);
    if (!buf) {
        end_edit(app);
        return true;
    }
    switch (e->key.key) {
        case SDLK_ESCAPE:
            end_edit(app);
            return true;
        case SDLK_BACKSPACE: {
            size_t n = strlen(buf);
            if (n > 0) buf[n - 1] = '\0';
            return true;
        }
        case SDLK_TAB:
            app->symptom_edit_field = (app->symptom_edit_field + 1) % 5;
            return true;
        case SDLK_RETURN:
            if (app->symptom_edit_field == 0) {
                const char *results[5];
                SymptomCard *c = &app->symptom_cards[app->symptom_edit_card];
                SymptomSubsection *ss = &c->subs[app->symptom_edit_sub];
                int n = search_collect(ss->query, results, 5);
                if (n > 0) {
                    int idx = app->symptom_search_sel;
                    if (idx < 0 || idx >= n) idx = 0;
                    cs_strlcpy(ss->name, results[idx], sizeof(ss->name));
                    ss->query[0] = '\0';
                    const SymptomDef *def = symptom_catalog_find(ss->name);
                    ss->controls = def ? def->controls : SYM_CTRL_ALL;
                }
            }
            end_edit(app);
            return true;
        case SDLK_UP:
        case SDLK_DOWN:
            if (app->symptom_edit_field == 0) {
                SymptomCard *c = &app->symptom_cards[app->symptom_edit_card];
                SymptomSubsection *ss = &c->subs[app->symptom_edit_sub];
                const char *results[5];
                int n = search_collect(ss->query, results, 5);
                if (n > 0) {
                    app->symptom_search_sel += e->key.key == SDLK_DOWN ? 1 : -1;
                    if (app->symptom_search_sel < 0) app->symptom_search_sel = n - 1;
                    if (app->symptom_search_sel >= n) app->symptom_search_sel = 0;
                }
                return true;
            }
            return false;
        default:
            return false;
    }
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
