#include "symptom_report.h"

#include <stdio.h>
#include <string.h>

#include "util.h"

static void put_json_string(FILE *f, const char *s)
{
    fputc('"', f);
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        switch (*p) {
            case '"': fputs("\\\"", f); break;
            case '\\': fputs("\\\\", f); break;
            case '\n': fputs("\\n", f); break;
            case '\r': fputs("\\r", f); break;
            case '\t': fputs("\\t", f); break;
            default:
                if (*p < 0x20) fprintf(f, "\\u%04x", *p);
                else fputc(*p, f);
        }
    }
    fputc('"', f);
}

static const char *kind_name(SymptomCardKind kind)
{
    switch (kind) {
        case SYMPTOM_CARD_MUSCLE: return "muscle";
        case SYMPTOM_CARD_WHOLE_BODY: return "whole_body";
        case SYMPTOM_CARD_NON_PHYSICAL: return "non_physical";
        default: return "unknown";
    }
}

static const char *pattern_name(int f)
{
    switch (f) {
        case 1: return "intermittent";
        case 2: return "occasional";
        default: return "constant";
    }
}

bool symptom_report_write_locked(App *app)
{
    if (!app || !app->session_active || !app->session_dir[0]) return false;
    char path[CS_PATH_MAX], tmp[CS_PATH_MAX + 8];
    cs_path_join(path, sizeof(path), app->session_dir, "symptom-report.json");
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        app->symptom_write_failed = true;
        return false;
    }
    fputs("{\n  \"schema\": \"cellscope-symptom-report/1\",\n  \"dataset\": ", f);
    put_json_string(f, app->session_name);
    fprintf(f, ",\n  \"created_at_unix_ms\": %lld,\n  \"cards\": [\n",
            (long long)app->session_started_ms);
    for (int i = 0; i < app->symptom_card_count; i++) {
        const SymptomCard *c = &app->symptom_cards[i];
        if (i) fputs(",\n", f);
        fputs("    {\n      \"kind\": ", f);
        put_json_string(f, kind_name(c->kind));
        fputs(",\n      \"code\": ", f);
        put_json_string(f, c->code);
        fputs(",\n      \"region\": ", f);
        put_json_string(f, c->region_name);
        fprintf(f, ",\n      \"side\": \"%s\",\n", c->side ? "male" : "female");
        fputs("      \"point\": {\"x\": ", f);
        fprintf(f, "%.5f, \"y\": %.5f, \"z\": %.5f},\n", c->point_x, c->point_y, c->point_z);
        fprintf(f, "      \"minimized\": %s,\n      \"symptoms\": [\n",
                c->minimized ? "true" : "false");
        for (int s = 0; s < c->sub_count; s++) {
            const SymptomSubsection *ss = &c->subs[s];
            if (s) fputs(",\n", f);
            fputs("        {\"name\": ", f);
            put_json_string(f, ss->name);
            fprintf(f, ", \"controls\": %u, \"severity\": %d, \"onset_hours\": %d, "
                       "\"duration_hours\": %d, \"pattern\": \"%s\",\n         ",
                    ss->controls, ss->severity, ss->onset_hours, ss->duration_hours,
                    pattern_name(ss->frequency));
            fputs("\"description\": ", f);
            put_json_string(f, ss->description);
            fputs(", \"triggers\": ", f);
            put_json_string(f, ss->triggers);
            fputs(", \"relief\": ", f);
            put_json_string(f, ss->relief);
            fputs(", \"notes\": ", f);
            put_json_string(f, ss->notes);
            fputs("}", f);
        }
        fputs("\n      ]\n    }", f);
    }
    fputs("\n  ]\n}\n", f);
    bool ok = fflush(f) == 0 && fclose(f) == 0;
    if (!ok) {
        remove(tmp);
        app->symptom_write_failed = true;
        return false;
    }
    remove(path);
    if (rename(tmp, path) != 0) {
        remove(tmp);
        app->symptom_write_failed = true;
        return false;
    }
    app->symptom_report_dirty = false;
    app->symptom_write_failed = false;
    return true;
}

void symptom_report_mark_dirty(App *app)
{
    if (app) app->symptom_report_dirty = true;
}

void symptom_report_flush(App *app)
{
    if (!app || !app->symptom_report_dirty) return;
    SDL_LockMutex(app->mu);
    if (app->session_active && app->session_dir[0]) symptom_report_write_locked(app);
    SDL_UnlockMutex(app->mu);
}

void symptom_report_reset_locked(App *app)
{
    if (!app) return;
    app->symptom_card_count = 0;
    app->symptom_focus_card = -1;
    app->symptom_scroll = 0;
    app->symptom_edit_card = -1;
    app->symptom_edit_sub = -1;
    app->symptom_edit_field = -1;
    app->symptom_report_dirty = true;
    app->symptom_write_failed = false;
}

void symptom_report_reset(App *app)
{
    if (!app) return;
    SDL_LockMutex(app->mu);
    symptom_report_reset_locked(app);
    SDL_UnlockMutex(app->mu);
    SDL_StopTextInput(app->win);
}
