#include "app.h"
#include "cloud.h"
#include "anatomy_assets.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <dirent.h>
#include <sys/stat.h>
#else
#include <windows.h>
#endif

#ifdef _WIN32
#include <direct.h>
#define getcwd _getcwd
#else
#include <unistd.h>
#endif

#include "proc.h"
#include "stb_image.h"
#include "stb_image_write.h"
#include "wifi.h"
#include "camera_platform.h"

static void app_log(App *app, const char *fmt, ...);
static void app_export_json_to(App *app, const char *path);
static int cloud_status_thread(void *arg);
static int upload_worker_thread(void *arg);
static bool has_json_suffix(const char *path);
static const char *file_basename(const char *path);
static int SDLCALL cellscope_adb_thread(void *data);
static int SDLCALL cellscope_vlm_thread(void *data);
static int SDLCALL cellscope_bootstrap_thread(void *data);
static void session_write_locked(App *app);
static bool session_begin_locked(App *app);
static void session_end_locked(App *app);
static bool session_rotate_locked(App *app);

static void cell_push_locked(App *app, const CellSample *s)
{
    CellSample copy = *s;
    copy.seq = ++app->cell_seq;
    if (app->cell_count < CELL_MAX) {
        int idx = (app->cell_head + app->cell_count) % CELL_MAX;
        app->cells[idx] = copy;
        app->cell_count++;
    } else {
        app->cells[app->cell_head] = copy;
        app->cell_head = (app->cell_head + 1) % CELL_MAX;
    }
    if (app->session_active) session_write_locked(app);
}

static void meter_push_locked(App *app, const MeterSample *m)
{
    MeterSample copy = *m;
    copy.seq = ++app->meter_seq;
    if (app->meter_count < METER_MAX) {
        int idx = (app->meter_head + app->meter_count) % METER_MAX;
        app->meters[idx] = copy;
        app->meter_count++;
    } else {
        app->meters[app->meter_head] = copy;
        app->meter_head = (app->meter_head + 1) % METER_MAX;
    }
    if (app->session_active) session_write_locked(app);
}

static void visibility_reset_locked(App *app)
{
    VisibilitySession *v = &app->visibility;
    for (int i = 0; i < v->frame_count; i++) free(v->frames[i].rgb);
    free(v->frames);
    memset(v, 0, sizeof(*v));
    /* A new visibility event must not reuse candidates captured while the
     * previous meter screen was present. Otherwise a stale frame can win the
     * new consensus and duplicate the previous frequency. */
    for (int i = 0; i < BURST_MAX; i++) {
        free(app->burst[i].rgb);
        memset(&app->burst[i], 0, sizeof(app->burst[i]));
    }
    app->burst_count = 0;
    app->burst_next = 0;
    app->reader.burst_count = 0;
}

static double visibility_tolerance(double hz)
{
    double t = fabs(hz) * 0.0002; /* 0.02% relative */
    return t < 1000.0 ? 1000.0 : t;
}

static double visibility_frame_quality(const VisibilityFrame *f)
{
    return (f->confidence > 0 ? f->confidence : 0.5) * 0.7 + f->frame_score * 0.3;
}

static void visibility_store_frame_locked(VisibilitySession *v, const MeterReading *r,
                                          const uint8_t *rgb, int w, int h,
                                          int64_t frame_ts, float frame_score,
                                          int64_t extracted_ms)
{
    if (!rgb || w <= 0 || h <= 0 || v->frame_count >= VIS_FRAME_MAX) return;
    if (v->frame_count == v->frame_cap) {
        int cap = v->frame_cap ? v->frame_cap * 2 : 16;
        if (cap > VIS_FRAME_MAX) cap = VIS_FRAME_MAX;
        VisibilityFrame *grown = realloc(v->frames, (size_t)cap * sizeof(*grown));
        if (!grown) return;
        v->frames = grown;
        v->frame_cap = cap;
    }
    VisibilityFrame *f = &v->frames[v->frame_count];
    memset(f, 0, sizeof(*f));
    size_t bytes = (size_t)w * h * 3;
    f->rgb = malloc(bytes);
    if (!f->rgb) return;
    memcpy(f->rgb, rgb, bytes);
    f->w = w;
    f->h = h;
    f->frame_score = frame_score;
    f->hz = r->hz;
    f->confidence = r->confidence;
    f->frame_ts_ms = frame_ts > 0 ? frame_ts : extracted_ms;
    f->extracted_ms = extracted_ms;
    cs_strlcpy(f->raw, r->raw, sizeof(f->raw));
    v->frame_count++;
}

/* Build frequency clusters from every frame retained for this visibility
 * event and choose the most probable reading. The winning frame is the
 * highest-confidence/quality frame in the winning cluster. */
static int visibility_pick_locked(App *app, bool require_consensus, int *out_readable,
                                  double *out_consensus)
{
    VisibilitySession *v = &app->visibility;
    enum { MAX_LOCAL = 64 };
    struct Local {
        double center;
        double weight;
        int count;
        int best;
        double best_quality;
        double last_weight;
    } clusters[MAX_LOCAL];
    int n = 0;
    int readable_count = 0;
    double total_weight = 0.0;

    for (int i = 0; i < v->frame_count; i++) {
        VisibilityFrame *f = &v->frames[i];
        if (f->hz <= 0) continue;
        readable_count++;
        double q = visibility_frame_quality(f);
        double tol = visibility_tolerance(f->hz);
        int ci = -1;
        double nearest = 1e300;
        for (int k = 0; k < n; k++) {
            double d = fabs(f->hz - clusters[k].center);
            if (d <= tol && d < nearest) {
                nearest = d;
                ci = k;
            }
        }
        if (ci < 0) {
            if (n >= MAX_LOCAL) continue;
            ci = n++;
            memset(&clusters[ci], 0, sizeof(clusters[ci]));
            clusters[ci].center = f->hz;
            clusters[ci].best = -1;
        }
        struct Local *c = &clusters[ci];
        double old = c->weight;
        c->weight += q;
        c->count++;
        c->center = (c->center * old + f->hz * q) / c->weight;
        if (q > c->best_quality) {
            c->best_quality = q;
            c->best = i;
        }
    }

    int win = -1;
    double win_score = -1.0;
    double second_weight = 0.0;
    for (int i = 0; i < n; i++) {
        total_weight += clusters[i].weight;
        double score = clusters[i].weight + 0.02 * (double)clusters[i].count;
        if (score > win_score) {
            if (win >= 0) second_weight = clusters[win].weight;
            win = i;
            win_score = score;
        } else if (clusters[i].weight > second_weight) {
            second_weight = clusters[i].weight;
        }
    }

    if (out_readable) *out_readable = readable_count;
    if (out_consensus)
        *out_consensus = (win >= 0 && total_weight > 0)
                             ? clusters[win].weight / total_weight
                             : 0.0;
    if (win < 0 || clusters[win].best < 0) return -1;

    if (require_consensus) {
        /* A short, clear look at the meter commonly yields only three VLM
         * frames before the display moves. Require three unanimous readings
         * so the row is committed in real time instead of waiting for the
         * screen-loss fallback. */
        if (readable_count < 3) return -1;
        if (clusters[win].count < 3) return -1;
        if (total_weight <= 0.0 || clusters[win].weight / total_weight < 0.80)
            return -1;
        if (second_weight > 0.0 &&
            clusters[win].weight - second_weight < 0.20 * clusters[win].weight)
            return -1;
    }
    return clusters[win].best;
}

static bool visibility_emit_locked(App *app, int frame_index, double consensus,
                                   int readable_count, bool consensus_verified)
{
    VisibilitySession *v = &app->visibility;
    /* One consensus per visibility event. A new event is only created after
     * visibility_commit_locked has observed the screen leave and reset v. */
    if (!v->active || v->committed ||
        frame_index < 0 || frame_index >= v->frame_count)
        return false;
    VisibilityFrame *f = &v->frames[frame_index];
    if (f->hz <= 0) return false;

    char dir[CS_PATH_MAX], capture[CS_PATH_MAX] = {0};
    cs_strlcpy(dir,
               app->session_active && app->session_dir[0] ? app->session_dir
                                                          : app->capture_dir,
               sizeof(dir));
    int64_t ts = f->frame_ts_ms > 0
                     ? f->frame_ts_ms
                     : (f->extracted_ms > 0 ? f->extracted_ms : cs_epoch_ms());
    char stamp[40];
    cs_format_date_stamp(ts, stamp, sizeof(stamp));
    snprintf(capture, sizeof(capture), "%s/meter_%s_%03d.png", dir, stamp,
             (int)(ts % 1000));
    if (f->rgb && f->w > 0 && f->h > 0) {
        if (!stbi_write_png(capture, f->w, f->h, 3, f->rgb, f->w * 3))
            capture[0] = '\0';
    } else {
        capture[0] = '\0';
    }

    MeterSample ms;
    memset(&ms, 0, sizeof(ms));
    ms.ts_ms = ts;
    cs_format_time_ms(ms.ts_ms, ms.time_text, sizeof(ms.time_text));
    ms.hz = (uint64_t)(f->hz + 0.5);
    /* Consensus rows use the winning cluster's actual weight share, not the
     * model's per-frame default (usually 0.75). A screen-loss fallback row
     * without a verified consensus keeps the frame's model confidence. */
    double confidence = consensus_verified ? consensus : f->confidence;
    if (!(confidence > 0.0)) confidence = f->confidence;
    if (confidence < 0.0) confidence = 0.0;
    if (confidence > 1.0) confidence = 1.0;
    ms.confidence = confidence;
    cs_strlcpy(ms.raw, f->raw, sizeof(ms.raw));
    cs_strlcpy(ms.backend, app->vlm.label, sizeof(ms.backend));
    cs_strlcpy(ms.path, capture, sizeof(ms.path));
    cs_strlcpy(ms.shot_name, file_basename(capture), sizeof(ms.shot_name));
    meter_push_locked(app, &ms);
    app_log(app,
            "consensus reading recorded: %llu Hz, frame_ts=%lld extracted=%lld, "
            "readable=%d stored=%d consensus=%.2f",
            (unsigned long long)ms.hz, (long long)f->frame_ts_ms,
            (long long)f->extracted_ms, readable_count, v->frame_count, consensus);
    app->reader.accepted++;
    cs_strlcpy(app->reader.capture_path, capture, sizeof(app->reader.capture_path));
    app->reader.visible = true;
    app->reader.display_until_ms = cs_now_ms() + 3000;
    app->reader.hz = (double)ms.hz;
    app->reader.confidence = confidence;
    cs_strlcpy(app->reader.raw, ms.raw, sizeof(app->reader.raw));
    char eng[64];
    cs_format_eng(ms.hz, eng, sizeof(eng));
    snprintf(app->reader.status, sizeof(app->reader.status),
             "Recorded %s · %d stored frames", eng, v->frame_count);
    v->committed = true;
    return true;
}

static void visibility_commit_locked(App *app)
{
    VisibilitySession *v = &app->visibility;
    if (!v->active) return;
    if (!v->committed) {
        int readable = 0;
        double consensus = 0.0;
        int idx = visibility_pick_locked(app, false, &readable, &consensus);
        if (idx >= 0) visibility_emit_locked(app, idx, consensus, readable, false);
    }
    visibility_reset_locked(app);
}

static void visibility_try_consensus_locked(App *app)
{
    VisibilitySession *v = &app->visibility;
    if (!v->active || v->committed) return;
    int readable = 0;
    double consensus = 0.0;
    int idx = visibility_pick_locked(app, true, &readable, &consensus);
    if (idx >= 0) visibility_emit_locked(app, idx, consensus, readable, true);
}

static void visibility_update_locked(App *app, const MeterReading *r, const uint8_t *rgb,
                                     int w, int h, int64_t ts, float frame_score)
{
    VisibilitySession *v = &app->visibility;
    if (!r->visible) {
        visibility_commit_locked(app);
        return;
    }
    if (!v->active) {
        visibility_reset_locked(app);
        v->active = true;
        v->started_ms = cs_epoch_ms();
    }
    /* Ignore any further visible-frame readings after this event has already
     * reached consensus. Only the screen-loss path resets the session. */
    if (v->committed) return;
    v->total_readings++;
    if (r->hz <= 0) return;
    v->readable_readings++;
    int64_t extracted = cs_epoch_ms();
    /* Keep the complete readable-frame history for the visibility interval,
     * including frames after consensus; the list is freed when the display
     * leaves. */
    visibility_store_frame_locked(v, r, rgb, w, h, ts, frame_score, extracted);
    if (!v->committed) visibility_try_consensus_locked(app);
}

void app_add_cell(App *app, const CellSample *sample)
{
    SDL_LockMutex(app->mu);
    cell_push_locked(app, sample);
    SDL_UnlockMutex(app->mu);
}

void app_add_meter(App *app, const MeterSample *sample)
{
    SDL_LockMutex(app->mu);
    meter_push_locked(app, sample);
    SDL_UnlockMutex(app->mu);
}

void app_open_settings_modal(App *app)
{
    SDL_LockMutex(app->mu);
    AppSettings *s = &app->settings;
    snprintf(app->settings_edit[0], CS_PATH_MAX, "%d", s->version);
    cs_strlcpy(app->settings_edit[1], s->reader_backend, CS_PATH_MAX);
    cs_strlcpy(app->settings_edit[2], s->hf_model, CS_PATH_MAX);
    cs_strlcpy(app->settings_edit[3], s->hf_model_path, CS_PATH_MAX);
    snprintf(app->settings_edit[4], CS_PATH_MAX, "%d", s->hf_port);
    cs_strlcpy(app->settings_edit[5], s->hf_device, CS_PATH_MAX);
    snprintf(app->settings_edit[6], CS_PATH_MAX, "%d", s->camera_index);
    cs_strlcpy(app->settings_edit[7], s->upload_api_url, CS_PATH_MAX);
    cs_strlcpy(app->settings_edit[8], s->upload_email, CS_PATH_MAX);
    cs_strlcpy(app->settings_target, s->path, sizeof(app->settings_target));
    app->settings_focus = 0;
    app->settings_error[0] = '\0';
    app->settings_modal_open = true;
    app->upload_modal_open = false;
    SDL_UnlockMutex(app->mu);
    SDL_StartTextInput(app->win);
}

void app_close_settings_modal(App *app)
{
    SDL_LockMutex(app->mu);
    app->settings_modal_open = false;
    app->settings_error[0] = '\0';
    SDL_UnlockMutex(app->mu);
    SDL_StopTextInput(app->win);
}

void app_save_settings_modal(App *app)
{
    AppSettings tmp;
    char err[512] = {0};
    SDL_LockMutex(app->mu);
    tmp = app->settings;
    cs_strlcpy(tmp.path, app->settings_target, sizeof(tmp.path));
    char *end = NULL;
    long version = strtol(app->settings_edit[0], &end, 10);
    if (!end || end == app->settings_edit[0] || version < 0 || version > 1000000) {
        cs_strlcpy(app->settings_error, "Version must be a non-negative integer.",
                   sizeof(app->settings_error));
        SDL_UnlockMutex(app->mu);
        return;
    }
    tmp.version = (int)version;
    cs_strlcpy(tmp.reader_backend, app->settings_edit[1], sizeof(tmp.reader_backend));
    cs_strlcpy(tmp.hf_model, app->settings_edit[2], sizeof(tmp.hf_model));
    cs_strlcpy(tmp.hf_model_path, app->settings_edit[3], sizeof(tmp.hf_model_path));
    cs_strlcpy(tmp.hf_device, app->settings_edit[5], sizeof(tmp.hf_device));
    end = NULL;
    long port = strtol(app->settings_edit[4], &end, 10);
    if (!end || end == app->settings_edit[4] || port < 1024 || port > 65535) {
        cs_strlcpy(app->settings_error, "HF port must be between 1024 and 65535.",
                   sizeof(app->settings_error));
        SDL_UnlockMutex(app->mu);
        return;
    }
    tmp.hf_port = (int)port;
    end = NULL;
    long camera = strtol(app->settings_edit[6], &end, 10);
    if (!end || end == app->settings_edit[6] || camera < 0 || camera > 16) {
        cs_strlcpy(app->settings_error, "Camera index must be between 0 and 16.",
                   sizeof(app->settings_error));
        SDL_UnlockMutex(app->mu);
        return;
    }
    tmp.camera_index = (int)camera;
    cs_strlcpy(tmp.upload_api_url, app->settings_edit[7], sizeof(tmp.upload_api_url));
    cs_strlcpy(tmp.upload_email, app->settings_edit[8], sizeof(tmp.upload_email));
    if (strcmp(tmp.reader_backend, "minimind") != 0 &&
        strcmp(tmp.reader_backend, "hf") != 0) {
        cs_strlcpy(app->settings_error, "Reader backend must be \"minimind\" or \"hf\".",
                   sizeof(app->settings_error));
        SDL_UnlockMutex(app->mu);
        return;
    }
    bool ok = settings_save(&tmp, err, sizeof(err));
    if (!ok) {
        snprintf(app->settings_error, sizeof(app->settings_error), "%s", err);
        SDL_UnlockMutex(app->mu);
        return;
    }
    app->settings = tmp;
    app->settings_modal_open = false;
    app->settings_error[0] = '\0';
    SDL_UnlockMutex(app->mu);
    SDL_StopTextInput(app->win);
    app_notify(app, false, "Settings saved to %s. Restart CellScope to apply.",
               app->settings_target);
}

static void upload_history_path(App *app, char *out, size_t cap)
{
    cs_path_join(out, cap, app->capture_dir, "upload-history.tsv");
}

static void save_upload_history_locked(App *app)
{
    char path[CS_PATH_MAX];
    upload_history_path(app, path, sizeof(path));
    FILE *f = fopen(path, "wb");
    if (!f) return;
    for (int i = 0; i < app->upload_history_count; i++) {
        const UploadHistoryEntry *e = &app->upload_history[i];
        char msg[512];
        cs_strlcpy(msg, e->message, sizeof(msg));
        for (char *p = msg; *p; p++) if (*p == '\t' || *p == '\n' || *p == '\r') *p = ' ';
        fprintf(f, "%lld\t%s\t%s\t%s\n", (long long)e->ts_ms, e->status, e->archive, msg);
    }
    fclose(f);
}

static void load_upload_history(App *app)
{
    char path[CS_PATH_MAX];
    upload_history_path(app, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        char *fields[4] = {0};
        int nf = 0;
        char *start = line;
        for (char *p = line; nf < 4; p++) {
            if (*p == '\t' || *p == '\n' || *p == '\r' || *p == '\0') {
                bool end = *p == '\0';
                *p = '\0';
                fields[nf++] = start;
                if (end) break;
                start = p + 1;
            }
        }
        char *ts = fields[0], *status = fields[1], *archive = fields[2], *message = fields[3];
        if (!ts || !status || !archive) continue;
        int idx = app->upload_history_count;
        if (idx >= (int)CS_ARRAY_LEN(app->upload_history)) {
            memmove(&app->upload_history[0], &app->upload_history[1],
                    sizeof(app->upload_history[0]) * (CS_ARRAY_LEN(app->upload_history) - 1));
            idx = (int)CS_ARRAY_LEN(app->upload_history) - 1;
        } else {
            app->upload_history_count++;
        }
        UploadHistoryEntry *e = &app->upload_history[idx];
        memset(e, 0, sizeof(*e));
        e->ts_ms = strtoll(ts, NULL, 10);
        cs_strlcpy(e->status, status, sizeof(e->status));
        cs_strlcpy(e->archive, archive, sizeof(e->archive));
        cs_strlcpy(e->message, message ? message : "", sizeof(e->message));
    }
    fclose(f);
}

static void append_upload_history_locked(App *app, const char *status,
                                         const char *archive, const char *message)
{
    if (app->upload_history_count >= (int)CS_ARRAY_LEN(app->upload_history)) {
        memmove(&app->upload_history[0], &app->upload_history[1],
                sizeof(app->upload_history[0]) * (CS_ARRAY_LEN(app->upload_history) - 1));
        app->upload_history_count = (int)CS_ARRAY_LEN(app->upload_history) - 1;
    }
    UploadHistoryEntry *e = &app->upload_history[app->upload_history_count++];
    memset(e, 0, sizeof(*e));
    e->ts_ms = cs_epoch_ms();
    cs_strlcpy(e->status, status, sizeof(e->status));
    cs_strlcpy(e->archive, archive, sizeof(e->archive));
    cs_strlcpy(e->message, message ? message : "", sizeof(e->message));
    save_upload_history_locked(app);
}

static void upload_queue_path(App *app, char *out, size_t cap)
{
    cs_path_join(out, cap, app->capture_dir, "upload-queue.tsv");
}

static void save_upload_queue_locked(App *app)
{
    char path[CS_PATH_MAX];
    upload_queue_path(app, path, sizeof(path));
    FILE *f = fopen(path, "wb");
    if (!f) return;
    for (int i = 0; i < app->upload_job_count; i++)
        fprintf(f, "%s\t%s\n", app->upload_jobs[i].archive, app->upload_jobs[i].sig);
    fclose(f);
}

static void load_upload_queue(App *app)
{
    char path[CS_PATH_MAX];
    upload_queue_path(app, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char line[CS_PATH_MAX * 2];
    while (fgets(line, sizeof(line), f) && app->upload_job_count < (int)CS_ARRAY_LEN(app->upload_jobs)) {
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab++ = '\0';
        char *nl = strpbrk(tab, "\r\n");
        if (nl) *nl = '\0';
        UploadJob *j = &app->upload_jobs[app->upload_job_count++];
        cs_strlcpy(j->archive, line, sizeof(j->archive));
        cs_strlcpy(j->sig, tab, sizeof(j->sig));
    }
    fclose(f);
}

static int upload_worker_thread(void *arg)
{
    App *app = (App *)arg;
    while (!SDL_GetAtomicInt(&app->quit)) {
        char api[512], archive[CS_PATH_MAX], sig[CS_PATH_MAX];
        SDL_LockMutex(app->mu);
        bool has = app->upload_job_count > 0;
        if (has) {
            cs_strlcpy(api, app->settings.upload_api_url, sizeof(api));
            cs_strlcpy(archive, app->upload_jobs[0].archive, sizeof(archive));
            cs_strlcpy(sig, app->upload_jobs[0].sig, sizeof(sig));
        }
        SDL_UnlockMutex(app->mu);
        if (!has) {
            SDL_Delay(3000);
            continue;
        }

        char message[512] = {0};
        bool ok = cloud_upload_archive(api, archive, sig, message, sizeof(message));
        SDL_LockMutex(app->mu);
        cs_strlcpy(app->upload_status, message, sizeof(app->upload_status));
        append_upload_history_locked(app, ok ? "accepted" : "failed",
                                     file_basename(archive), message);
        if (ok) {
            memmove(&app->upload_jobs[0], &app->upload_jobs[1],
                    sizeof(app->upload_jobs[0]) * (app->upload_job_count - 1));
            app->upload_job_count--;
        }
        save_upload_queue_locked(app);
        SDL_UnlockMutex(app->mu);
        SDL_Delay(ok ? 1000 : 15000);
    }
    return 0;
}

void app_enroll_device(App *app)
{
    char message[512] = {0};
    char api[512], email[256];
    SDL_LockMutex(app->mu);
    cs_strlcpy(api, app->settings.upload_api_url, sizeof(api));
    cs_strlcpy(email, app->settings.upload_email, sizeof(email));
    SDL_UnlockMutex(app->mu);
    bool ok = cloud_enroll(api, email, message, sizeof(message));
    SDL_LockMutex(app->mu);
    cs_strlcpy(app->upload_status, message, sizeof(app->upload_status));
    SDL_UnlockMutex(app->mu);
    app_notify(app, !ok, "%s", message);
}

void app_upload_dataset(App *app)
{
    char api[512];
    SDL_LockMutex(app->mu);
    cs_strlcpy(api, app->settings.upload_api_url, sizeof(api));
    int sample_count = app->cell_count;
    int reading_count = app->meter_count;
    app->upload_status[0] = '\0';
    app->silent_export = true;
    SDL_UnlockMutex(app->mu);

    if (sample_count == 0 && reading_count == 0) {
        SDL_LockMutex(app->mu);
        app->silent_export = false;
        cs_strlcpy(app->upload_status,
                   "Dataset is empty: no cell samples or readings. Nothing was uploaded.",
                   sizeof(app->upload_status));
        SDL_UnlockMutex(app->mu);
        app_notify(app, true, "Dataset is empty; nothing was compressed or uploaded.");
        return;
    }

    app_export_dataset(app);

    SDL_LockMutex(app->mu);
    app->silent_export = false;
    char archive[CS_PATH_MAX], sig[CS_PATH_MAX];
    cs_strlcpy(archive, app->last_export_archive, sizeof(archive));
    cs_strlcpy(sig, app->last_export_sig, sizeof(sig));
    SDL_UnlockMutex(app->mu);

    if (!archive[0] || !sig[0]) {
        SDL_LockMutex(app->mu);
        cs_strlcpy(app->upload_status, "Could not create a signed dataset archive.",
                   sizeof(app->upload_status));
        SDL_UnlockMutex(app->mu);
        return;
    }

    SDL_LockMutex(app->mu);
    if (app->upload_job_count < (int)CS_ARRAY_LEN(app->upload_jobs)) {
        UploadJob *job = &app->upload_jobs[app->upload_job_count++];
        cs_strlcpy(job->archive, archive, sizeof(job->archive));
        cs_strlcpy(job->sig, sig, sizeof(job->sig));
        save_upload_queue_locked(app);
        bool rotated = session_rotate_locked(app);
        snprintf(app->upload_status, sizeof(app->upload_status),
                 rotated ? "Queued %s for upload. New dataset started."
                         : "Queued %s for upload, but a new dataset could not be started.",
                 file_basename(archive));
    } else {
        cs_strlcpy(app->upload_status, "Upload queue is full.", sizeof(app->upload_status));
    }
    int queued = app->upload_job_count;
    SDL_UnlockMutex(app->mu);
    app_notify(app, false, "Queued for upload (%d pending).", queued);
    (void)api;
}

void app_reset_identity(App *app)
{
    char api[512], message[512] = {0};
    SDL_LockMutex(app->mu);
    cs_strlcpy(api, app->settings.upload_api_url, sizeof(api));
    SDL_UnlockMutex(app->mu);
    bool ok = cloud_reset_identity(api, message, sizeof(message));
    SDL_LockMutex(app->mu);
    cs_strlcpy(app->upload_status, message, sizeof(app->upload_status));
    SDL_UnlockMutex(app->mu);
    app_notify(app, !ok, "%s", message);
}

void app_set_tab(App *app, int tab)
{
    if (tab != APP_TAB_RF && tab != APP_TAB_SYMPTOM) return;
    SDL_LockMutex(app->mu);
    bool leaving_rf = app->active_tab == APP_TAB_RF && tab != APP_TAB_RF;
    app->active_tab = tab;
    bool start_anatomy = tab == APP_TAB_SYMPTOM;
    if (leaving_rf) {
        app->modal_open = false;
        app->modal_checking = false;
        app->remove_meter_open = false;
        app->model_modal_open = false;
    }
    SDL_UnlockMutex(app->mu);
    if (start_anatomy) anatomy_assets_start(app);
}

int app_get_tab(App *app)
{
    SDL_LockMutex(app->mu);
    int tab = app->active_tab;
    SDL_UnlockMutex(app->mu);
    return tab;
}

void app_open_upload_modal(App *app)
{
    SDL_LockMutex(app->mu);
    app->upload_modal_open = true;
    app->settings_modal_open = false;
    SDL_UnlockMutex(app->mu);
}

void app_close_upload_modal(App *app)
{
    SDL_LockMutex(app->mu);
    app->upload_modal_open = false;
    SDL_UnlockMutex(app->mu);
}

typedef struct {
    char repo[256];
    char path[CS_PATH_MAX];
} ModelEntry;

static int model_entry_cmp(const void *a, const void *b)
{
    const ModelEntry *ma = a;
    const ModelEntry *mb = b;
    return strcmp(ma->repo, mb->repo);
}

void app_open_model_modal(App *app)
{
    char root[CS_PATH_MAX];
    SDL_LockMutex(app->mu);
    if (app->runtime.dir[0]) cs_strlcpy(root, app->runtime.dir, sizeof(root));
    else {
        char data[CS_PATH_MAX];
        if (!cs_app_data_dir(data, sizeof(data))) cs_strlcpy(data, ".", sizeof(data));
        cs_path_join(root, sizeof(root), data, "runtime");
    }
    SDL_UnlockMutex(app->mu);

    char models[CS_PATH_MAX];
    cs_path_join(models, sizeof(models), root, "hf-models");
    ModelEntry entries[16];
    int count = 0;
#ifdef _WIN32
    char pattern[CS_PATH_MAX];
    snprintf(pattern, sizeof(pattern), "%s\\*", models);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
            char path[CS_PATH_MAX], cfg[CS_PATH_MAX];
            cs_path_join(path, sizeof(path), models, fd.cFileName);
            cs_path_join(cfg, sizeof(cfg), path, "config.json");
            if (!cs_file_exists(cfg) || count >= 16) continue;
            char repo[256];
            cs_strlcpy(repo, fd.cFileName, sizeof(repo));
            char *sep = strstr(repo, "__");
            if (sep) { *sep = '/'; memmove(sep + 1, sep + 2, strlen(sep + 2) + 1); }
            cs_strlcpy(entries[count].repo, repo, sizeof(entries[count].repo));
            cs_strlcpy(entries[count].path, path, sizeof(entries[count].path));
            count++;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    DIR *d = opendir(models);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL && count < 16) {
            if (de->d_name[0] == '.') continue;
            char path[CS_PATH_MAX], cfg[CS_PATH_MAX];
            cs_path_join(path, sizeof(path), models, de->d_name);
            cs_path_join(cfg, sizeof(cfg), path, "config.json");
            if (!cs_is_dir(path) || !cs_file_exists(cfg)) continue;
            char repo[256];
            cs_strlcpy(repo, de->d_name, sizeof(repo));
            char *sep = strstr(repo, "__");
            if (sep) {
                *sep = '/';
                memmove(sep + 1, sep + 2, strlen(sep + 2) + 1);
            }
            cs_strlcpy(entries[count].repo, repo, sizeof(entries[count].repo));
            cs_strlcpy(entries[count].path, path, sizeof(entries[count].path));
            count++;
        }
        closedir(d);
    }
#endif
    qsort(entries, (size_t)count, sizeof(entries[0]), model_entry_cmp);
    SDL_LockMutex(app->mu);
    app->model_count = count;
    for (int i = 0; i < count; i++) {
        cs_strlcpy(app->model_repos[i], entries[i].repo, sizeof(app->model_repos[i]));
        cs_strlcpy(app->model_paths[i], entries[i].path, sizeof(app->model_paths[i]));
    }
    app->model_modal_open = true;
    app->settings_modal_open = false;
    app->upload_modal_open = false;
    SDL_UnlockMutex(app->mu);
}

void app_close_model_modal(App *app)
{
    SDL_LockMutex(app->mu);
    app->model_modal_open = false;
    SDL_UnlockMutex(app->mu);
}

void app_select_hf_model(App *app, int index)
{
    char repo[256], path[CS_PATH_MAX], err[512] = {0};
    SDL_LockMutex(app->mu);
    if (index < 0 || index >= app->model_count) {
        SDL_UnlockMutex(app->mu);
        return;
    }
    cs_strlcpy(repo, app->model_repos[index], sizeof(repo));
    cs_strlcpy(path, app->model_paths[index], sizeof(path));
    AppSettings next = app->settings;
    cs_strlcpy(next.reader_backend, "hf", sizeof(next.reader_backend));
    cs_strlcpy(next.hf_model, repo, sizeof(next.hf_model));
    cs_strlcpy(next.hf_model_path, path, sizeof(next.hf_model_path));
    if (!settings_save(&next, err, sizeof(err))) {
        SDL_UnlockMutex(app->mu);
        app_notify(app, true, "Could not save the reader model: %s", err);
        return;
    }
    app->settings = next;
    app->model_modal_open = false;
    app->prefer_minimind = false;
    app->vlm_switched = false;
    app->reload_runtime = true;
    app->retry_runtime = true;
    SDL_UnlockMutex(app->mu);
    app_notify(app, false, "Loading local reader model %s…", repo);
}

void app_request_remove_meter(App *app, int index)
{
    SDL_LockMutex(app->mu);
    if (index >= 0 && index < app->meter_count) {
        app->remove_meter_open = true;
        app->remove_meter_index = index;
    }
    SDL_UnlockMutex(app->mu);
}

void app_cancel_remove_meter(App *app)
{
    SDL_LockMutex(app->mu);
    app->remove_meter_open = false;
    app->remove_meter_index = -1;
    SDL_UnlockMutex(app->mu);
}

void app_confirm_remove_meter(App *app)
{
    SDL_LockMutex(app->mu);
    int index = app->remove_meter_index;
    if (!app->remove_meter_open || index < 0 || index >= app->meter_count) {
        app->remove_meter_open = false;
        app->remove_meter_index = -1;
        SDL_UnlockMutex(app->mu);
        return;
    }
    int phys = (app->meter_head + index) % METER_MAX;
    uint64_t seq = app->meters[phys].seq;
    for (int j = index; j < app->meter_count - 1; j++) {
        int dst = (app->meter_head + j) % METER_MAX;
        int src = (app->meter_head + j + 1) % METER_MAX;
        app->meters[dst] = app->meters[src];
    }
    app->meter_count--;
    if (app->selected_meter == index) {
        app->selected_meter = -1;
        app->selected_meter_ts = 0;
    } else if (app->selected_meter > index) {
        app->selected_meter--;
    }
    app->remove_meter_open = false;
    app->remove_meter_index = -1;
    if (app->session_active) session_write_locked(app);
    SDL_UnlockMutex(app->mu);
    app_notify(app, false, "Removed reading %llu Hz", (unsigned long long)seq);
}

int app_cell_count(App *app)
{
    SDL_LockMutex(app->mu);
    int n = app->cell_count;
    SDL_UnlockMutex(app->mu);
    return n;
}

CellSample app_cell_at(App *app, int index)
{
    CellSample s;
    memset(&s, 0, sizeof(s));
    SDL_LockMutex(app->mu);
    if (index >= 0 && index < app->cell_count) {
        int idx = (app->cell_head + index) % CELL_MAX;
        s = app->cells[idx];
    }
    SDL_UnlockMutex(app->mu);
    return s;
}

int app_meter_count(App *app)
{
    SDL_LockMutex(app->mu);
    int n = app->meter_count;
    SDL_UnlockMutex(app->mu);
    return n;
}

MeterSample app_meter_at(App *app, int index)
{
    MeterSample m;
    memset(&m, 0, sizeof(m));
    SDL_LockMutex(app->mu);
    if (index >= 0 && index < app->meter_count) {
        int idx = (app->meter_head + index) % METER_MAX;
        m = app->meters[idx];
    }
    SDL_UnlockMutex(app->mu);
    return m;
}

static void app_log(App *app, const char *fmt, ...)
{
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    fprintf(stderr, "cellscope: %s\n", line);
    if (app->log_path[0]) {
        FILE *f = fopen(app->log_path, "a");
        if (f) {
            fprintf(f, "%lld %s\n", (long long)cs_epoch_ms(), line);
            fclose(f);
        }
    }
}

static void find_asset_dir(App *app)
{
    const char *env = getenv("CELLSCOPE_ASSETS");
    char candidate[CS_PATH_MAX];
    const char *base = SDL_GetBasePath();
    if (env && *env && cs_file_exists(env)) {
        cs_strlcpy(app->asset_dir, env, sizeof(app->asset_dir));
        return;
    }
    if (base) {
        cs_path_join(candidate, sizeof(candidate), base, "assets");
        cs_path_join(candidate, sizeof(candidate), candidate, "fonts/Inter-Regular.ttf");
        if (cs_file_exists(candidate)) {
            char dir[CS_PATH_MAX];
            cs_strlcpy(dir, base, sizeof(dir));
            cs_strlcat(dir, "assets", sizeof(dir));
            cs_strlcpy(app->asset_dir, dir, sizeof(app->asset_dir));
            return;
        }
        cs_strlcpy(candidate, base, sizeof(candidate));
        cs_strlcat(candidate, "../Resources/assets/fonts/Inter-Regular.ttf", sizeof(candidate));
        if (cs_file_exists(candidate)) {
            cs_strlcpy(app->asset_dir, base, sizeof(app->asset_dir));
            cs_strlcat(app->asset_dir, "../Resources/assets", sizeof(app->asset_dir));
            return;
        }
    }
#ifdef CELLSCOPE_SOURCE_ASSETS
    cs_strlcpy(app->asset_dir, CELLSCOPE_SOURCE_ASSETS, sizeof(app->asset_dir));
#else
    cs_strlcpy(app->asset_dir, "assets", sizeof(app->asset_dir));
#endif
}

static void default_capture_dir(App *app)
{
    if (app->capture_dir[0]) return;
    char base[CS_PATH_MAX];
    if (cs_app_data_dir(base, sizeof(base))) {
        cs_strlcpy(app->capture_dir, base, sizeof(app->capture_dir));
    } else {
        const char *bp = SDL_GetBasePath();
        cs_path_join(app->capture_dir, sizeof(app->capture_dir), bp ? bp : ".",
                     "captures");
    }
}

void app_print_usage(void)
{
    printf(
        "CellScope %s — live Android cell data + frequency-meter VLM reader\n\n"
        "Usage: cellscope [options]\n\n"
        "  --train DIR            Train a meter reader from DIR (images + one CSV)\n"
        "  --train-csv FILE       CSV with filename,freq (default: auto-find in DIR)\n"
        "  --train-out DIR        Training output (default: DIR/cellscope_training)\n"
        "  --train-epochs N       SFT epochs (default 2)\n"
        "  --train-batch N        Batch size (default 4)\n"
        "  --train-lr F           Learning rate (default 5e-6)\n"
        "  --train-unit UNIT      Unit for bare CSV numbers (default MHz)\n"
        "  --train-device DEV     cuda:0 | mps | cpu (default: upstream auto)\n"
        "  --train-augment N      Extra augmented variants per image (default 0)\n"
        "  --train-dry-run        Validate CSV + build parquet, do not train\n"
        "  --train-resume         Resume from the MiniMind checkpoint\n"
        "  --demo                 Synthetic phone + synthetic meter feed (no hardware)\n"
        "  --autoplay             Start capture immediately (default: wait for play)\n"
        "  --demo-phone           Synthetic cell samples only\n"
        "  --readme-fixture MODE  README screenshot fixture: main, live, best, symptom\n"
        "  --image PATH           Use a still image instead of a webcam (testing)\n"
        "  --export-json PATH     Write both lists to JSON and exit (testing/automation)\n"
        "  --download-model REPO  Download a Hugging Face VLM and make it the reader\n"
        "  --no-modal             Skip the startup connection modal\n"
        "  --no-camera            Do not open the webcam (headless/modal testing)\n"
        "  --no-runtime           Do not bootstrap the local MiniMind runtime\n"
        "  --setup-runtime        Install the local Python + MiniMind assets and exit\n"
        "  --auto-connect         Try to connect to adb immediately at startup\n"
        "  --vlm MODE             auto | ollama | llama | mock | off\n"
        "  --vlm-url URL          default http://127.0.0.1:11434 (Ollama)\n"
        "  --vlm-model NAME       e.g. moondream, qwen2.5vl:3b, SmolVLM-256M-Instruct\n"
        "  --unit UNIT            fallback unit when the meter shows no unit (MHz)\n"
        "  --camera N             camera index (default 0)\n"
        "  --adb PATH             path to the adb executable\n"
        "  --capture-dir PATH     root for datasets, screenshots, logs, and exports\n"
        "  --screenshot PATH      save a window screenshot and exit (testing)\n"
        "  --screenshot-delay MS  wait before the screenshot (default 2500)\n"
        "  --export-now           export the current dataset and exit (testing)\n"
        "  --run-ms MS            exit after this many milliseconds\n"
        "  --help                 show this help\n\n"
        "By default the first run installs a private Python 3.11 + MiniMind runtime in\n"
        "~/.cellscope (macOS/Linux) or %%LOCALAPPDATA%%/CellScope (Windows), then starts\n"
        "the local bridge on port 8090. No --vlm arguments are needed. Use --vlm to attach an external\n"
        "backend instead (this disables the automatic runtime).\n\n"
        "Readers (small local VLMs, no cloud):\n"
        "  Ollama\n"
        "    ollama pull moondream && ollama serve\n"
        "    cellscope --vlm ollama --vlm-model moondream\n"
        "  llama.cpp with SmolVLM (256M, ~290 MB)\n"
        "    llama-server -hf ggml-org/SmolVLM-256M-Instruct-GGUF --port 8080\n"
        "    cellscope --vlm llama --vlm-url http://127.0.0.1:8080\n"

        "Local Hugging Face reader:\n"
        "  export HF_TOKEN=hf_...\n"
        "  cellscope --download-model HuggingFaceTB/SmolVLM-256M-Instruct\n"
        "  # writes settings.json; the next launch serves it locally on port 8091\n\n"
        "Environment: CELLSCOPE_VLM, CELLSCOPE_VLM_URL, CELLSCOPE_VLM_MODEL,\n"
        "             CELLSCOPE_DEFAULT_UNIT, CELLSCOPE_CAPTURE_DIR, CELLSCOPE_SETTINGS,\n"
        "             CELLSCOPE_HOME\n"
        "             HF_TOKEN / HUGGINGFACE_HUB_TOKEN, ADB\n",
        CELLSCOPE_VERSION);
}

static bool parse_int(const char *s, int *out)
{
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (!s || !*s || !end || *end) return false;
    *out = (int)v;
    return true;
}

AppArgsResult app_parse_args(App *app, int argc, char **argv, char *err, size_t err_cap)
{
    memset(app, 0, sizeof(*app));
    cs_strlcpy(app->default_unit, "MHz", sizeof(app->default_unit));
    app->camera_index = 0;
    app->screenshot_delay_ms = 2500;
    app->quit_after_screenshot = true;
    app->vlm_explicit = false;
    cs_strlcpy(app->train.minimind_dir, "third_party/minimind-v",
               sizeof(app->train.minimind_dir));
    cs_strlcpy(app->train.base_weight, "llm", sizeof(app->train.base_weight));
    cs_strlcpy(app->train.save_weight, "cellscope_meter", sizeof(app->train.save_weight));
    cs_strlcpy(app->train.unit, "MHz", sizeof(app->train.unit));
    app->train.epochs = 2;
    app->train.batch_size = 4;
    app->train.augment = 0;
    app->train.hidden_size = 768;
    app->train.num_layers = 8;
    app->train.max_seq_len = 768;
    app->train.num_workers = 0;
    app->train.learning_rate = 5e-6;

    const char *env = getenv("CELLSCOPE_VLM");
    if (env) cs_strlcpy(app->vlm_mode, env, sizeof(app->vlm_mode));
    env = getenv("CELLSCOPE_VLM_URL");
    if (env) cs_strlcpy(app->vlm_url, env, sizeof(app->vlm_url));
    env = getenv("CELLSCOPE_VLM_MODEL");
    if (env) cs_strlcpy(app->vlm_model, env, sizeof(app->vlm_model));
    env = getenv("CELLSCOPE_DEFAULT_UNIT");
    if (env) cs_strlcpy(app->default_unit, env, sizeof(app->default_unit));
    env = getenv("CELLSCOPE_CAPTURE_DIR");
    if (env) cs_strlcpy(app->capture_dir, env, sizeof(app->capture_dir));
    env = getenv("ADB");
    if (env) cs_strlcpy(app->adb_override, env, sizeof(app->adb_override));

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            return APP_ARGS_HELP;
        } else if (strcmp(a, "--version") == 0) {
            printf("CellScope %s\n", CELLSCOPE_VERSION);
            return APP_ARGS_VERSION;
        } else if (strcmp(a, "--train") == 0 && i + 1 < argc) {
            cs_strlcpy(app->train.dataset_dir, argv[++i],
                       sizeof(app->train.dataset_dir));
            app->train_mode = true;
        } else if (strcmp(a, "--train-csv") == 0 && i + 1 < argc) {
            cs_strlcpy(app->train.csv_path, argv[++i], sizeof(app->train.csv_path));
            app->train_mode = true;
        } else if (strcmp(a, "--train-out") == 0 && i + 1 < argc) {
            cs_strlcpy(app->train.out_dir, argv[++i], sizeof(app->train.out_dir));
        } else if (strcmp(a, "--train-minimind") == 0 && i + 1 < argc) {
            cs_strlcpy(app->train.minimind_dir, argv[++i],
                       sizeof(app->train.minimind_dir));
        } else if (strcmp(a, "--train-base") == 0 && i + 1 < argc) {
            cs_strlcpy(app->train.base_weight, argv[++i], sizeof(app->train.base_weight));
        } else if (strcmp(a, "--train-save-weight") == 0 && i + 1 < argc) {
            cs_strlcpy(app->train.save_weight, argv[++i],
                       sizeof(app->train.save_weight));
        } else if (strcmp(a, "--train-unit") == 0 && i + 1 < argc) {
            cs_strlcpy(app->train.unit, argv[++i], sizeof(app->train.unit));
        } else if (strcmp(a, "--train-epochs") == 0 && i + 1 < argc) {
            if (!parse_int(argv[++i], &app->train.epochs) || app->train.epochs <= 0) {
                snprintf(err, err_cap, "Invalid --train-epochs");
                return APP_ARGS_ERROR;
            }
        } else if (strcmp(a, "--train-batch") == 0 && i + 1 < argc) {
            if (!parse_int(argv[++i], &app->train.batch_size) || app->train.batch_size <= 0) {
                snprintf(err, err_cap, "Invalid --train-batch");
                return APP_ARGS_ERROR;
            }
        } else if (strcmp(a, "--train-lr") == 0 && i + 1 < argc) {
            char *end = NULL;
            app->train.learning_rate = strtod(argv[++i], &end);
            if (!end || *end || app->train.learning_rate <= 0) {
                snprintf(err, err_cap, "Invalid --train-lr");
                return APP_ARGS_ERROR;
            }
        } else if (strcmp(a, "--train-device") == 0 && i + 1 < argc) {
            cs_strlcpy(app->train.device, argv[++i], sizeof(app->train.device));
        } else if (strcmp(a, "--train-hidden") == 0 && i + 1 < argc) {
            if (!parse_int(argv[++i], &app->train.hidden_size) || app->train.hidden_size <= 0) {
                snprintf(err, err_cap, "Invalid --train-hidden");
                return APP_ARGS_ERROR;
            }
        } else if (strcmp(a, "--train-layers") == 0 && i + 1 < argc) {
            if (!parse_int(argv[++i], &app->train.num_layers) || app->train.num_layers <= 0) {
                snprintf(err, err_cap, "Invalid --train-layers");
                return APP_ARGS_ERROR;
            }
        } else if (strcmp(a, "--train-augment") == 0 && i + 1 < argc) {
            if (!parse_int(argv[++i], &app->train.augment) || app->train.augment < 0) {
                snprintf(err, err_cap, "Invalid --train-augment");
                return APP_ARGS_ERROR;
            }
        } else if (strcmp(a, "--train-workers") == 0 && i + 1 < argc) {
            if (!parse_int(argv[++i], &app->train.num_workers) ||
                app->train.num_workers < 0) {
                snprintf(err, err_cap, "Invalid --train-workers");
                return APP_ARGS_ERROR;
            }
        } else if (strcmp(a, "--train-dry-run") == 0) {
            app->train.dry_run = true;
        } else if (strcmp(a, "--train-resume") == 0) {
            app->train.resume = true;
        } else if (strcmp(a, "--demo") == 0) {
            app->demo = true;
            app->demo_phone = true;
        } else if (strcmp(a, "--demo-phone") == 0) {
            app->demo_phone = true;
        } else if (strcmp(a, "--readme-fixture") == 0 && i + 1 < argc) {
            const char *mode = argv[++i];
            app->readme_fixture = true;
            app->readme_fixture_mode = strcmp(mode, "live") == 0 ? 2
                                     : strcmp(mode, "best") == 0 ? 3
                                     : strcmp(mode, "remove") == 0 ? 5
                                     : strcmp(mode, "settings") == 0 ? 6
                                     : strcmp(mode, "upload") == 0 ? 7
                                     : strcmp(mode, "model") == 0 ? 8
                                     : strcmp(mode, "symptom") == 0 ? 9
                                     : strcmp(mode, "symptom-card") == 0 ? 10
                                     : strcmp(mode, "symptom-zoom") == 0 ? 11
                                                                 : 1;
        } else if (strcmp(a, "--autoplay") == 0) {
            app->autoplay = true;
        } else if (strcmp(a, "--no-modal") == 0) {
            app->no_modal = true;
        } else if (strcmp(a, "--no-camera") == 0) {
            app->no_camera = true;
        } else if (strcmp(a, "--relaunched") == 0) {
            /* Internal guard used by the macOS LaunchServices re-exec. */
            app->relaunched = true;
        } else if (strcmp(a, "--setup-runtime") == 0) {
            app->setup_runtime_mode = true;
        } else if (strcmp(a, "--no-runtime") == 0) {
            app->no_runtime = true;
        } else if (strcmp(a, "--auto-connect") == 0) {
            app->auto_connect = true;
        } else if (strcmp(a, "--download-model") == 0 && i + 1 < argc) {
            cs_strlcpy(app->download_model, argv[++i], sizeof(app->download_model));
        } else if (strcmp(a, "--export-json") == 0 && i + 1 < argc) {
            cs_strlcpy(app->export_json_path, argv[++i], sizeof(app->export_json_path));
        } else if (strcmp(a, "--image") == 0 && i + 1 < argc) {
            cs_strlcpy(app->image_path, argv[++i], sizeof(app->image_path));
            app->image_mode = true;
            app->no_modal = true;
        } else if (strcmp(a, "--vlm") == 0 && i + 1 < argc) {
            cs_strlcpy(app->vlm_mode, argv[++i], sizeof(app->vlm_mode));
            app->vlm_explicit = true;
        } else if (strcmp(a, "--vlm-url") == 0 && i + 1 < argc) {
            cs_strlcpy(app->vlm_url, argv[++i], sizeof(app->vlm_url));
            app->vlm_explicit = true;
        } else if (strcmp(a, "--vlm-model") == 0 && i + 1 < argc) {
            cs_strlcpy(app->vlm_model, argv[++i], sizeof(app->vlm_model));
            app->vlm_explicit = true;
        } else if (strcmp(a, "--unit") == 0 && i + 1 < argc) {
            cs_strlcpy(app->default_unit, argv[++i], sizeof(app->default_unit));
        } else if (strcmp(a, "--camera") == 0 && i + 1 < argc) {
            app->camera_cli = true;
            if (!parse_int(argv[++i], &app->camera_index)) {
                snprintf(err, err_cap, "Invalid camera index");
                return APP_ARGS_ERROR;
            }
        } else if (strcmp(a, "--adb") == 0 && i + 1 < argc) {
            cs_strlcpy(app->adb_override, argv[++i], sizeof(app->adb_override));
        } else if (strcmp(a, "--capture-dir") == 0 && i + 1 < argc) {
            cs_strlcpy(app->capture_dir, argv[++i], sizeof(app->capture_dir));
        } else if (strcmp(a, "--screenshot") == 0 && i + 1 < argc) {
            cs_strlcpy(app->screenshot_path, argv[++i], sizeof(app->screenshot_path));
        } else if (strcmp(a, "--screenshot-delay") == 0 && i + 1 < argc) {
            int v = 0;
            if (!parse_int(argv[++i], &v) || v < 0) {
                snprintf(err, err_cap, "Invalid screenshot delay");
                return APP_ARGS_ERROR;
            }
            app->screenshot_delay_ms = v;
        } else if (strcmp(a, "--export-now") == 0) {
            app->export_now = true;
        } else if (strcmp(a, "--run-ms") == 0 && i + 1 < argc) {
            int v = 0;
            if (!parse_int(argv[++i], &v) || v < 0) {
                snprintf(err, err_cap, "Invalid run duration");
                return APP_ARGS_ERROR;
            }
            app->run_ms = v;
        } else {
            snprintf(err, err_cap, "Unknown or incomplete option: %s", a);
            return APP_ARGS_ERROR;
        }
    }

    if (app->train_mode) {
        if (!app->train.dataset_dir[0]) {
            snprintf(err, err_cap, "--train requires a dataset folder");
            return APP_ARGS_ERROR;
        }
        return APP_ARGS_OK;
    }
    if (app->readme_fixture) {
        app->demo = false;
        app->demo_phone = false;
        app->image_mode = true;
        app->no_modal = true;
        app->no_runtime = true;
        if (!app->image_path[0])
            cs_strlcpy(app->image_path, "example.png", sizeof(app->image_path));
    }
    if (app->demo || app->demo_phone) {
        if (app->demo_phone && app->image_mode) app->image_mode = false;
        app->no_modal = true;
    }
    if (app->demo && !app->vlm_explicit) {
        cs_strlcpy(app->vlm_mode, "mock", sizeof(app->vlm_mode));
    }
    if (app->image_mode && !cs_file_exists(app->image_path)) {
        snprintf(err, err_cap, "Image not found: %s", app->image_path);
        return APP_ARGS_ERROR;
    }
    if (app->vlm_mode[0] && strcmp(app->vlm_mode, "auto") != 0 &&
        strcmp(app->vlm_mode, "ollama") != 0 && strcmp(app->vlm_mode, "llama") != 0 &&
        strcmp(app->vlm_mode, "llama-server") != 0 &&
        strcmp(app->vlm_mode, "mock") != 0 && strcmp(app->vlm_mode, "off") != 0) {
        snprintf(err, err_cap,
                 "Unsupported --vlm mode: %s (use auto, ollama, llama, mock, or off)",
                 app->vlm_mode);
        return APP_ARGS_ERROR;
    }
    return APP_ARGS_OK;
}


static uint64_t random_u64(void)
{
    uint64_t v = 0;
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        if (fread(&v, 1, sizeof(v), f) != sizeof(v)) v = 0;
        fclose(f);
    }
    if (!v) v = (uint64_t)cs_epoch_ms() ^ ((uint64_t)(uintptr_t)&v << 17) ^ 0x9e3779b97f4a7c15ULL;
    return v;
}

static uint64_t load_wifi_salt(const char *capture_dir)
{
    char dirs[2][CS_PATH_MAX];
    int ndirs = 0;
    char appdata[CS_PATH_MAX];
    if (cs_app_data_dir(appdata, sizeof(appdata))) {
        cs_mkdirs(appdata);
        cs_strlcpy(dirs[ndirs++], appdata, sizeof(dirs[0]));
    }
    if (capture_dir && *capture_dir) {
        cs_strlcpy(dirs[ndirs++], capture_dir, sizeof(dirs[0]));
    }
    uint64_t v = random_u64();
    char hex[32];
    snprintf(hex, sizeof(hex), "%016llx\n", (unsigned long long)v);
    for (int i = 0; i < ndirs; i++) {
        char path[CS_PATH_MAX];
        cs_path_join(path, sizeof(path), dirs[i], ".wifi_salt");
        uint8_t *data = NULL;
        size_t len = 0;
        if (cs_read_file(path, &data, &len) && len > 0) {
            uint64_t parsed = strtoull((char *)data, NULL, 16);
            free(data);
            if (parsed) return parsed;
        }
        if (cs_write_file(path, hex, strlen(hex))) return v;
    }
    return v;
}

static CellSample make_demo_cell(int64_t ts, int seq, uint64_t salt)
{
    CellSample s;
    memset(&s, 0, sizeof(s));
    s.ts_ms = ts;
    cs_strlcpy(s.plmn, "20408", sizeof(s.plmn));
    cs_strlcpy(s.mcc, "204", sizeof(s.mcc));
    cs_strlcpy(s.mnc, "08", sizeof(s.mnc));
    cs_strlcpy(s.country, "Netherlands", sizeof(s.country));
    cs_strlcpy(s.operator_name, "KPN", sizeof(s.operator_name));
    cs_strlcpy(s.tac, "0", sizeof(s.tac));
    cs_strlcpy(s.rat, "5G NSA", sizeof(s.rat));
    cs_strlcpy(s.enb, "31301", sizeof(s.enb));
    cs_strlcpy(s.cid, "22", sizeof(s.cid));
    cs_strlcpy(s.pci, "671 / 389", sizeof(s.pci));
    cs_strlcpy(s.ta, "11", sizeof(s.ta));
    cs_strlcpy(s.arfcn, "432030 / 3600", sizeof(s.arfcn));
    cs_strlcpy(s.band, "N1 / L8", sizeof(s.band));
    cs_strlcpy(s.bw, "10 / 20 / 20", sizeof(s.bw));
    s.ca = true;
    cs_strlcpy(s.f_dl, "2160.2 / 940.0", sizeof(s.f_dl));
    cs_strlcpy(s.f_ul, "1970.2 / 895.0", sizeof(s.f_ul));
    WifiAp aps[3];
    aps[0].id = wifi_anonymize("cellscope-demo-home", salt);
    aps[0].rssi = -42;
    aps[1].id = wifi_anonymize("cellscope-demo-neighbour", salt);
    aps[1].rssi = -67;
    aps[2].id = wifi_anonymize("cellscope-demo-cafe", salt);
    aps[2].rssi = -81;
    wifi_format(aps, 3, s.wifi, sizeof(s.wifi));
    s.demo = true;
    cs_strlcpy(s.source, seq % 7 == 0 ? "Pixel 8 · demo" : "Pixel 8",
               sizeof(s.source));
    return s;
}

void app_seed_demo(App *app)
{
    int64_t now = cs_epoch_ms();
    SDL_LockMutex(app->mu);
    for (int i = 11; i >= 0; i--) {
        CellSample s = make_demo_cell(now - (int64_t)i * 1000, 11 - i, app->wifi_salt);
        cell_push_locked(app, &s);
    }
    for (int i = 4; i >= 0; i--) {
        MeterSample m;
        memset(&m, 0, sizeof(m));
        m.ts_ms = now - (int64_t)i * 2300 - 600;
        cs_format_time_ms(m.ts_ms, m.time_text, sizeof(m.time_text));
        m.hz = (uint64_t)(vlm_demo_hz(m.ts_ms) + 0.5);
        m.confidence = 0.94;
        cs_format_eng(m.hz, m.raw, sizeof(m.raw));
        cs_strlcpy(m.backend, "Demo reader", sizeof(m.backend));
        m.demo = true;
        meter_push_locked(app, &m);
        if (i == 0) {
            app->reader.visible = true;
            app->reader.hz = (double)m.hz;
            app->reader.confidence = m.confidence;
            cs_strlcpy(app->reader.raw, m.raw, sizeof(app->reader.raw));
            cs_strlcpy(app->reader.backend, "Demo reader", sizeof(app->reader.backend));
            cs_strlcpy(app->reader.status, "Recorded · stable synthetic reading",
                       sizeof(app->reader.status));
            app->reader.kind = VLM_BACKEND_MOCK;
            app->reader.is_vlm = true;
            cs_strlcpy(app->reader.detail, "Synthetic demo feed.", sizeof(app->reader.detail));
        }
    }
    app->cell_follow = true;
    app->meter_follow = true;
    app->last_poll_ok_ms = now;
    SDL_UnlockMutex(app->mu);
}

static void readme_fixture_seed(App *app)
{
    int64_t now = cs_epoch_ms();
    struct CellFixture {
        const char *band, *rat, *enb, *cid, *pci, *ta, *arfcn, *bw, *dl, *ul;
        bool ca;
    } cells[] = {
        {"n1", "5G NSA", "31301", "22", "671 / 389", "11", "432030 / 3600",
         "10 / 20", "2160.2 / 940.0", "1970.2 / 895.0", true},
        {"3", "LTE", "31302", "101", "102", "7", "1650", "20", "1842.5",
         "1747.5", false},
        {"20", "LTE", "31303", "77", "233", "9", "6300", "10", "806.0", "847.0",
         false},
        {"8", "LTE", "31304", "44", "318", "6", "3748", "10", "942.5", "897.5",
         false},
        {"n78", "5G SA", "31305", "9", "512", "13", "641280", "100", "3489.6",
         "3489.6", true},
    };
    static const double meters[] = {
        2526.1208, 2526.2471, 2526.3586,
        2526.4412, 2526.5127, 2526.5744,
    };

    SDL_LockMutex(app->mu);
    app->adb.state = ADB_STATE_CONNECTED;
    app->adb.attempted = true;
    app->adb.device_count = 1;
    cs_strlcpy(app->adb.serial, "R58M31NXQAB", sizeof(app->adb.serial));
    cs_strlcpy(app->adb.model, "Pixel 8", sizeof(app->adb.model));
    cs_strlcpy(app->adb.detail, "ADB · USB · authorized", sizeof(app->adb.detail));
    app->adb.hint[0] = '\0';
    app->adb.last_check_ms = now;
    app->ever_connected = true;
    app->modal_checking = false;
    app->last_poll_ok_ms = cs_now_ms();
    cs_strlcpy(app->device_label, "Pixel 8 · KPN", sizeof(app->device_label));

    for (int i = 0; i < 5; i++) {
        CellSample c;
        memset(&c, 0, sizeof(c));
        c.ts_ms = now - (int64_t)(4 - i) * 1200;
        cs_strlcpy(c.plmn, "20408", sizeof(c.plmn));
        cs_strlcpy(c.mcc, "204", sizeof(c.mcc));
        cs_strlcpy(c.mnc, "08", sizeof(c.mnc));
        cs_strlcpy(c.country, "Netherlands", sizeof(c.country));
        cs_strlcpy(c.operator_name, "KPN", sizeof(c.operator_name));
        cs_strlcpy(c.tac, "4123", sizeof(c.tac));
        cs_strlcpy(c.rat, cells[i].rat, sizeof(c.rat));
        cs_strlcpy(c.enb, cells[i].enb, sizeof(c.enb));
        cs_strlcpy(c.cid, cells[i].cid, sizeof(c.cid));
        cs_strlcpy(c.pci, cells[i].pci, sizeof(c.pci));
        cs_strlcpy(c.ta, cells[i].ta, sizeof(c.ta));
        cs_strlcpy(c.arfcn, cells[i].arfcn, sizeof(c.arfcn));
        cs_strlcpy(c.band, cells[i].band, sizeof(c.band));
        cs_strlcpy(c.bw, cells[i].bw, sizeof(c.bw));
        c.ca = cells[i].ca;
        cs_strlcpy(c.f_dl, cells[i].dl, sizeof(c.f_dl));
        cs_strlcpy(c.f_ul, cells[i].ul, sizeof(c.f_ul));
        WifiAp aps[3];
        aps[0].id = wifi_anonymize("cellscope-readme-home", 0x52454144);
        aps[0].rssi = -41;
        aps[1].id = wifi_anonymize("cellscope-readme-neighbour", 0x52454144);
        aps[1].rssi = -66;
        aps[2].id = wifi_anonymize("cellscope-readme-cafe", 0x52454144);
        aps[2].rssi = -79;
        wifi_format(aps, 3, c.wifi, sizeof(c.wifi));
        cs_strlcpy(c.source, "Pixel 8", sizeof(c.source));
        cell_push_locked(app, &c);
    }

    for (int i = 0; i < (int)CS_ARRAY_LEN(meters); i++) {
        MeterSample m;
        memset(&m, 0, sizeof(m));
        m.ts_ms = now - (int64_t)(CS_ARRAY_LEN(meters) - 1 - i) * 1700;
        cs_format_time_ms(m.ts_ms, m.time_text, sizeof(m.time_text));
        m.hz = (uint64_t)(meters[i] * 1e6 + 0.5);
        m.confidence = 1.0;
        snprintf(m.raw, sizeof(m.raw), "%.4fMHz", meters[i]);
        cs_strlcpy(m.backend, "GLM-OCR", sizeof(m.backend));
        snprintf(m.shot_name, sizeof(m.shot_name), "meter_readme_%02d.png", i + 1);
        meter_push_locked(app, &m);
    }

    double latest = meters[CS_ARRAY_LEN(meters) - 1];
    app->reader.screen_visible = app->readme_fixture_mode == 3 ? false : true;
    app->reader.visible = app->readme_fixture_mode == 2 ? false : true;
    app->reader.live_valid = app->readme_fixture_mode != 3;
    app->reader.hz = latest * 1e6;
    app->reader.confidence = 1.0;
    app->reader.live_hz = latest * 1e6;
    app->reader.live_confidence = 1.0;
    app->reader.live_ms = now;
    app->reader.display_until_ms = app->reader.visible ? now + 300000 : 0;
    app->reader.burst_count = 6;
    app->reader.best_score = 0.94f;
    app->reader.frame_w = 1562;
    app->reader.frame_h = 1198;
    app->reader.last_ok_ms = now;
    app->reader.attempts = 6;
    app->reader.accepted = 6;
    app->reader.kind = VLM_BACKEND_LLAMA;
    app->reader.is_vlm = true;
    cs_strlcpy(app->reader.backend, "GLM-OCR", sizeof(app->reader.backend));
    cs_strlcpy(app->reader.detail, "zai-org/GLM-OCR · fixture", sizeof(app->reader.detail));
    cs_strlcpy(app->reader.raw, "2526.5744MHz", sizeof(app->reader.raw));
    cs_strlcpy(app->reader.live_raw, "2526.5744MHz", sizeof(app->reader.live_raw));
    if (app->readme_fixture_mode == 2)
        cs_strlcpy(app->reader.status, "Screen visible · live 2526.5744 MHz",
                   sizeof(app->reader.status));
    else if (app->readme_fixture_mode == 3)
        cs_strlcpy(app->reader.status, "Screen left · consensus recorded",
                   sizeof(app->reader.status));
    else
        cs_strlcpy(app->reader.status, "Screen visible · live 2526.5744 MHz · 6 candidates",
                   sizeof(app->reader.status));
    app->cell_follow = true;
    app->meter_follow = true;
    if (app->readme_fixture_mode == 5) {
        app->remove_meter_open = true;
        app->remove_meter_index = app->meter_count - 1;
    }
    if (app->readme_fixture_mode == 9 || app->readme_fixture_mode == 10 ||
        app->readme_fixture_mode == 11)
        app->active_tab = APP_TAB_SYMPTOM;
    if (app->readme_fixture_mode == 10) {
        app->symptom_card_count = 1;
        app->symptom_focus_card = 0;
        SymptomCard *c = &app->symptom_cards[0];
        c->kind = SYMPTOM_CARD_MUSCLE;
        cs_strlcpy(c->code, "VL", sizeof(c->code));
        cs_strlcpy(c->region_name, "Thigh", sizeof(c->region_name));
        c->side = 0;
        c->point_x = -0.428f;
        c->point_y = -0.134f;
        c->point_z = 0.172f;
        c->has_point = true;
    }
    SDL_UnlockMutex(app->mu);
}

static int cloud_status_thread(void *arg)
{
    App *app = (App *)arg;
    while (!SDL_GetAtomicInt(&app->quit)) {
        SDL_Delay(3000);
        if (SDL_GetAtomicInt(&app->quit)) break;

        char api[512], current[512];
        bool open = false;
        SDL_LockMutex(app->mu);
        open = app->upload_modal_open;
        cs_strlcpy(api, app->settings.upload_api_url, sizeof(api));
        cs_strlcpy(current, app->upload_status, sizeof(current));
        SDL_UnlockMutex(app->mu);
        if (!open || !api[0]) continue;

        char status[32] = {0}, message[512] = {0};
        if (!cloud_device_status(api, status, sizeof(status), message, sizeof(message))) continue;

        SDL_LockMutex(app->mu);
        bool waiting = !app->upload_status[0] ||
                       strstr(app->upload_status, "Check ") == app->upload_status ||
                       strstr(app->upload_status, "Waiting for email") == app->upload_status;
        if (strcmp(status, "active") == 0 && waiting)
            cs_strlcpy(app->upload_status, "Email verified. Ready to upload.",
                       sizeof(app->upload_status));
        else if (strcmp(status, "pending") == 0 && waiting)
            cs_strlcpy(app->upload_status, "Waiting for email verification...",
                       sizeof(app->upload_status));
        SDL_UnlockMutex(app->mu);
        (void)current;
    }
    return 0;
}

bool app_init(App *app, SDL_Window *win, GpuRenderer *r, char *err, size_t err_cap)
{
    app->win = win;
    app->r = r;
    app->started_ms = cs_now_ms();
    app->paused = !app->autoplay;
    app->symptom_focus_card = -1;
    if (getenv("CELLSCOPE_START_PAUSED")) app->paused = true;
    if (getenv("CELLSCOPE_AUTOPLAY")) app->paused = false;
    char settings_err[512] = {0};
    if (settings_load(&app->settings, settings_err, sizeof(settings_err))) {
        if (!app->camera_cli) app->camera_index = app->settings.camera_index;
    } else {
        app_log(app, "settings load failed: %s", settings_err);
    }
    if (strcmp(app->settings.reader_backend, "hf") == 0 &&
        app->settings.hf_model[0]) {
        snprintf(app->reader.backend, sizeof(app->reader.backend), "HF · %s",
                 app->settings.hf_model);
        cs_strlcpy(app->reader.detail, app->settings.hf_model_path,
                   sizeof(app->reader.detail));
        app->reader.kind = VLM_BACKEND_LLAMA;
        app->reader.is_vlm = true;
        cs_strlcpy(app->reader.status, "Loading the local reader model…",
                   sizeof(app->reader.status));
    } else {
        cs_strlcpy(app->reader.backend, "MiniMind · local", sizeof(app->reader.backend));
        app->reader.kind = VLM_BACKEND_LLAMA;
    }
    cs_strlcpy(app->local_reader_label, "MiniMind · local", sizeof(app->local_reader_label));
    cs_strlcpy(app->local_reader_model, "minimind-meter", sizeof(app->local_reader_model));
    app->wifi_salt = load_wifi_salt(app->capture_dir);
    app->selected_meter = -1;
    app->cell_follow = true;
    app->meter_follow = true;

    default_capture_dir(app);
    if (!cs_mkdirs(app->capture_dir)) {
        snprintf(err, err_cap, "Could not create capture directory: %s", app->capture_dir);
        return false;
    }
    cs_path_join(app->log_path, sizeof(app->log_path), app->capture_dir, "cellscope.log");
    load_upload_history(app);
    load_upload_queue(app);
    find_asset_dir(app);

    app->mu = SDL_CreateMutex();
    if (!app->mu) {
        snprintf(err, err_cap, "Could not create mutex");
        return false;
    }
    app->adb.state = ADB_STATE_UNKNOWN;
    adb_status_init(&app->adb);

    float scale = SDL_GetWindowDisplayScale(win);
    if (scale <= 0) scale = 1.0f;
    if (!ui_init(&app->ui, r, app->asset_dir, scale, err, err_cap)) return false;
    app->anatomy = anatomy_3d_create(app);

    if (!app->demo && !app->image_mode && !app->no_camera) {
        if (!app->paused) {
            char cam_err[512];
            if (camera_init(&app->cam, r, app->camera_index, cam_err,
                            sizeof(cam_err))) {
                app->camera_ok = true;
            } else {
                app->camera_ok = false;
                cs_strlcpy(app->camera_error, cam_err, sizeof(app->camera_error));
            }
        } else {
            cs_strlcpy(app->camera_error, "Press play to start the webcam.",
                       sizeof(app->camera_error));
        }
        app->last_camera_retry_ms = cs_now_ms();
    }

    if (app->image_mode) {
        int n = 0;
        unsigned char *pixels = stbi_load(app->image_path, &app->img_w, &app->img_h, &n, 3);
        if (!pixels) {
            snprintf(err, err_cap, "Could not load image: %s", app->image_path);
            return false;
        }
        app->img_rgb = pixels;
        app->img_tex = gpu_texture_create(r, app->img_w, app->img_h);
        if (app->img_tex) {
            unsigned char *rgba = malloc((size_t)app->img_w * app->img_h * 4);
            if (rgba) {
                for (int i = 0; i < app->img_w * app->img_h; i++) {
                    rgba[i * 4 + 0] = pixels[i * 3 + 0];
                    rgba[i * 4 + 1] = pixels[i * 3 + 1];
                    rgba[i * 4 + 2] = pixels[i * 3 + 2];
                    rgba[i * 4 + 3] = 255;
                }
                gpu_texture_update(app->img_tex, rgba, app->img_w * 4);
                free(rgba);
            }
        }
    }

    if (!app->readme_fixture) {
        adb_find(app->adb_override, app->adb_path, sizeof(app->adb_path));
        if (!app->adb_path[0]) {
            adb_check(NULL, &app->adb);
            app->adb.attempted = false;
            app->adb.last_check_ms = 0;
            app->adb.busy = false;
        }
    }

    vlm_config_init(&app->vlm, app->vlm_mode, app->vlm_url, app->vlm_model);
    app->last_probe_ms = 0;

    cs_strlcpy(app->project_root, cs_project_root(), sizeof(app->project_root));
    app->runtime_enabled = !app->no_runtime && !app->demo && !app->demo_phone &&
                           !app->image_mode && !app->vlm_explicit;
    if (app->runtime_enabled) {
        char rerr[1024] = {0};
        if (!runtime_init(&app->runtime, app->project_root, rerr, sizeof(rerr))) {
            app->runtime_enabled = false;
            SDL_LockMutex(app->mu);
            app->runtime_view.active = true;
            app->runtime_view.error = true;
            app->runtime_view.percent = 0;
            cs_strlcpy(app->runtime_view.message, "Local AI setup unavailable",
                       sizeof(app->runtime_view.message));
            cs_strlcpy(app->runtime_view.error_text, rerr,
                       sizeof(app->runtime_view.error_text));
            SDL_UnlockMutex(app->mu);
        }
    }

    app->modal_open = !app->no_modal && !app->demo && !app->image_mode;
    app->tracker = (MeterTracker){0};
    meter_tracker_init(&app->tracker);
    if (app->readme_fixture) readme_fixture_seed(app);
    if (app->active_tab == APP_TAB_SYMPTOM) anatomy_assets_start(app);
    if (app->paused) {
        SDL_LockMutex(app->mu);
        cs_strlcpy(app->reader.status, "Press play to begin capture",
                   sizeof(app->reader.status));
        SDL_UnlockMutex(app->mu);
    }

    if (app->demo || app->demo_phone) {
        SDL_LockMutex(app->mu);
        app->adb.state = ADB_STATE_CONNECTED;
        cs_strlcpy(app->adb.serial, "DEMO", sizeof(app->adb.serial));
        cs_strlcpy(app->adb.model, "Pixel 8 (demo)", sizeof(app->adb.model));
        cs_strlcpy(app->adb.detail, "Synthetic telephony source. No phone is connected.",
                   sizeof(app->adb.detail));
        cs_strlcpy(app->adb.hint,
                   "Run without --demo and connect a phone over ADB for live data.",
                   sizeof(app->adb.hint));
        app->ever_connected = true;
        cs_strlcpy(app->device_label, "Pixel 8 · demo", sizeof(app->device_label));
        SDL_UnlockMutex(app->mu);
    }
    if (app->demo && !app->paused) app_seed_demo(app);
    if (!app->train_mode && !app->setup_runtime_mode && !app->export_json_path[0]) {
        SDL_LockMutex(app->mu);
        bool session_ok = session_begin_locked(app);
        SDL_UnlockMutex(app->mu);
        if (!session_ok)
            app_notify(app, true, "Could not create a dataset folder under %s",
                       app->datasets_dir);
    }
    if (app->export_now) {
        app_export_dataset(app);
        SDL_SetAtomicInt(&app->quit, 1);
    }
    if (app->export_json_path[0]) {
        app_export_json_to(app, app->export_json_path);
        SDL_SetAtomicInt(&app->quit, 1);
    }

    if (app->auto_connect && !app->demo && !app->readme_fixture) app->want_check = true;
    if (app->readme_fixture_mode == 6) app_open_settings_modal(app);
    else if (app->readme_fixture_mode == 7) app_open_upload_modal(app);
    else if (app->readme_fixture_mode == 8) app_open_model_modal(app);

    if (!app->readme_fixture) {
        app->adb_thread = SDL_CreateThread(cellscope_adb_thread, "adb-poller", app);
        app->vlm_thread = SDL_CreateThread(cellscope_vlm_thread, "vlm-reader", app);
        if (app->runtime_enabled) {
            app->bootstrap_thread =
                SDL_CreateThread(cellscope_bootstrap_thread, "py-runtime", app);
        }
    }
    if (!app->readme_fixture &&
        (!app->adb_thread || !app->vlm_thread ||
         (app->runtime_enabled && !app->bootstrap_thread))) {
        snprintf(err, err_cap, "Could not create worker threads");
        return false;
    }

    app_log(app, "started: assets=%s captures=%s vlm=%s", app->asset_dir, app->capture_dir,
            app->vlm_mode[0] ? app->vlm_mode : "auto");
    app->upload_thread = SDL_CreateThread(upload_worker_thread, "upload-queue", app);
    if (!app->upload_thread) app_log(app, "could not start upload queue thread");
    app->cloud_status_thread = SDL_CreateThread(cloud_status_thread, "cloud-status", app);
    if (!app->cloud_status_thread)
        app_log(app, "could not start cloud status thread");
    return true;
}

void app_shutdown(App *app)
{
    if (!app) return;
    SDL_SetAtomicInt(&app->quit, 1);
    if (app->runtime_enabled) runtime_cancel(&app->runtime);
    if (app->bootstrap_thread) {
        SDL_WaitThread(app->bootstrap_thread, NULL);
        app->bootstrap_thread = NULL;
    }
    if (app->adb_thread) {
        SDL_WaitThread(app->adb_thread, NULL);
        app->adb_thread = NULL;
    }
    if (app->vlm_thread) {
        SDL_WaitThread(app->vlm_thread, NULL);
        app->vlm_thread = NULL;
    }
    if (app->anatomy_thread) anatomy_assets_shutdown(app);
    if (app->upload_thread) {
        SDL_WaitThread(app->upload_thread, NULL);
        app->upload_thread = NULL;
    }
    if (app->cloud_status_thread) {
        SDL_WaitThread(app->cloud_status_thread, NULL);
        app->cloud_status_thread = NULL;
    }
    if (app->mu) {
        SDL_LockMutex(app->mu);
        visibility_commit_locked(app);
        session_end_locked(app);
        SDL_UnlockMutex(app->mu);
    }
    camera_close(&app->cam);
    anatomy_3d_destroy(app->anatomy);
    app->anatomy = NULL;
    if (app->img_tex) gpu_texture_destroy(app->r, app->img_tex);
    if (app->preview_tex) gpu_texture_destroy(app->r, app->preview_tex);
    ui_shutdown(&app->ui);
    for (int i = 0; i < BURST_MAX; i++) free(app->burst[i].rgb);
    free(app->latest_rgb);
    free(app->motion_sig);
    free(app->img_rgb);
    if (app->mu) SDL_DestroyMutex(app->mu);
}

#define MOTION_GW 64
#define MOTION_GH 36

static void publish_frame(App *app, const uint8_t *rgb, int w, int h, int64_t ts)
{
    if (!rgb || w <= 0 || h <= 0) return;
    size_t bytes = (size_t)w * h * 3;
    FrameQuality q = frame_quality(rgb, w, h);
    uint8_t sig[MOTION_GW * MOTION_GH];
    frame_luma_signature(rgb, w, h, sig, MOTION_GW, MOTION_GH);

    SDL_LockMutex(app->mu);
    if (!app->motion_sig) {
        app->motion_sig = malloc(MOTION_GW * MOTION_GH);
        if (app->motion_sig) {
            memcpy(app->motion_sig, sig, MOTION_GW * MOTION_GH);
            app->motion_sig_n = MOTION_GW * MOTION_GH;
            app->have_motion_sig = true;
        }
    }
    float motion = app->have_motion_sig
                       ? signature_motion(sig, app->motion_sig, MOTION_GW * MOTION_GH)
                       : 0.0f;
    if (app->motion_sig) memcpy(app->motion_sig, sig, MOTION_GW * MOTION_GH);
    app->have_motion_sig = true;

    /* Keep exactly one latest frame for presence checks; never archive bad
     * frames. Frames that fail these legibility/stability gates are discarded
     * here, before the VLM queue. */
    uint8_t *latest = realloc(app->latest_rgb, bytes);
    if (latest) {
        memcpy(latest, rgb, bytes);
        app->latest_rgb = latest;
        app->latest_w = w;
        app->latest_h = h;
        app->latest_ts_ms = ts;
        app->latest_quality = q;
    }

    bool candidate = q.score >= 0.16f && q.sharpness >= 0.10f &&
                     q.contrast >= 0.14f && q.glare <= 0.40f &&
                     q.dark <= 0.40f && motion <= 0.12f;
    if (candidate) {
        int slot = app->burst_next;
        free(app->burst[slot].rgb);
        uint8_t *copy = malloc(bytes);
        if (copy) {
            memcpy(copy, rgb, bytes);
            app->burst[slot].rgb = copy;
            app->burst[slot].w = w;
            app->burst[slot].h = h;
            app->burst[slot].ts_ms = ts;
            app->burst[slot].seq = ++app->burst_seq;
            app->burst[slot].score = q.score;
            app->burst[slot].motion = motion;
            app->burst[slot].quality = q;
            app->burst[slot].used = false;
            app->burst_next = (slot + 1) % BURST_MAX;
            if (app->burst_count < BURST_MAX) app->burst_count++;
        } else {
            app->burst[slot].rgb = NULL;
        }
    }
    app->reader.burst_count = app->burst_count;
    SDL_UnlockMutex(app->mu);
}

bool app_retry_camera(App *app, bool notify)
{
    camera_close(&app->cam);
    app->camera_error[0] = '\0';
    char err[512] = {0};
    bool ok = camera_init(&app->cam, app->r, app->camera_index, err, sizeof(err));
    app->camera_ok = ok;
    if (!ok) cs_strlcpy(app->camera_error, err, sizeof(app->camera_error));
    if (notify || ok) app_log(app, "camera retry: %s%s", ok ? "opened" : "failed",
                              ok ? "" : err);
    if (notify) {
        if (ok) {
            if (app->cam.permission_pending)
                app_notify(app, false,
                           "Camera opened — waiting for the system permission dialog.");
            else
                app_notify(app, false, "Camera connected: %s", app->cam.name);
        } else {
            app_notify(app, true, "Camera: %s", err);
        }
    }
    return ok;
}

void app_update(App *app)
{
    if (app->readme_fixture) return;
    int64_t now = cs_now_ms();

    SDL_LockMutex(app->mu);
    if (app->reader.visible && app->reader.display_until_ms > 0 &&
        now >= app->reader.display_until_ms) {
        app->reader.visible = false;
        app->reader.display_until_ms = 0;
        app->reader.hz = 0;
        app->reader.confidence = 0;
        app->reader.raw[0] = '\0';
        if (!app->reader.screen_visible)
            cs_strlcpy(app->reader.status,
                       app->paused ? "Paused — press play to resume"
                                   : "Watching for a display…",
                       sizeof(app->reader.status));
    }
    SDL_UnlockMutex(app->mu);

    RuntimeView rv;
    SDL_LockMutex(app->mu);
    rv = app->runtime_view;
    SDL_UnlockMutex(app->mu);
    app->ui_banner_h = (rv.active || rv.error) ? 38 : 0;
    if (rv.server_ready && !app->runtime_toasted) {
        app->runtime_toasted = true;
        app_notify(app, false, "Local reader online — %s", rv.server_label);
    }
    if (!rv.server_ready && app->runtime_toasted) app->runtime_toasted = false;

    if (!app->paused) {
        if (app->image_mode && app->img_rgb) {
            if (now - app->last_snapshot_ms >= 400) {
                publish_frame(app, app->img_rgb, app->img_w, app->img_h, cs_epoch_ms());
                app->last_snapshot_ms = now;
            }
        } else if (!app->demo && app->camera_ok) {
            camera_update(&app->cam);
            if (app->cam.rgb && app->cam.rgb_ms != app->last_camera_rgb_ms &&
                now - app->last_snapshot_ms >= 40) {
                publish_frame(app, app->cam.rgb, app->cam.rgb_w, app->cam.rgb_h,
                              app->cam.rgb_ms);
                app->last_camera_rgb_ms = app->cam.rgb_ms;
                app->last_snapshot_ms = now;
            }
        }
    }

    if (!app->paused && !app->demo && !app->image_mode && !app->no_camera) {
        if (!app->camera_permission_requested && now - app->started_ms > 800 &&
            app->cam.permission_pending) {
            app->camera_permission_requested = true;
            app_log(app, "requesting system camera permission");
            SDL_RaiseWindow(app->win);
            camera_platform_request_access();
        }
        if (!app->camera_ok && app->cam.permission_pending &&
            now - app->last_camera_retry_ms > 1500) {
            app->last_camera_retry_ms = now;
            app_retry_camera(app, false);
        }
    }

    AdbStatus st;
    bool modal = false;
    bool checking = false;
    int64_t last_check = 0;
    SDL_LockMutex(app->mu);
    st = app->adb;
    modal = app->modal_open;
    checking = app->modal_checking;
    last_check = app->last_check_ms;
    SDL_UnlockMutex(app->mu);

    if (modal && st.state == ADB_STATE_CONNECTED) {
        SDL_LockMutex(app->mu);
        app->modal_open = false;
        app->modal_checking = false;
        SDL_UnlockMutex(app->mu);
        app_notify(app, false, "Phone connected — live cell data is flowing.");
        app_log(app, "phone connected: %s", st.serial);
    }

    if (app->auto_connect && !app->auto_connect_done && !app->demo) {
        SDL_LockMutex(app->mu);
        if (!app->adb.attempted) {
            app->want_check = true;
            app->modal_checking = true;
            app->auto_connect_done = true;
        } else {
            app->auto_connect_done = true;
        }
        SDL_UnlockMutex(app->mu);
    }

    if (!app->demo && !app->image_mode && !checking && st.attempted &&
        st.state != ADB_STATE_CONNECTED && st.state != ADB_STATE_NO_TOOL) {
        bool retry = false;
        if (modal && now - last_check > 4000) retry = true;
        if (!modal && app->ever_connected && now - last_check > 5000) retry = true;
        if (retry) {
            SDL_LockMutex(app->mu);
            app->want_check = true;
            app->modal_checking = true;
            SDL_UnlockMutex(app->mu);
        }
    }

    if (now - app->last_preview_check_ms > 400 && !app->demo) {
        app->last_preview_check_ms = now;
        char path[CS_PATH_MAX] = {0};
        SDL_LockMutex(app->mu);
        cs_strlcpy(path, app->reader.capture_path, sizeof(path));
        SDL_UnlockMutex(app->mu);
        if (path[0] && strcmp(path, app->preview_path) != 0) {
            int w = 0, h = 0, n = 0;
            unsigned char *pixels = stbi_load(path, &w, &h, &n, 3);
            if (pixels) {
                if (app->preview_tex) gpu_texture_destroy(app->r, app->preview_tex);
                app->preview_tex = gpu_texture_create(app->r, w, h);
                if (app->preview_tex) {
                    unsigned char *rgba = malloc((size_t)w * h * 4);
                    if (rgba) {
                        for (int i = 0; i < w * h; i++) {
                            rgba[i * 4 + 0] = pixels[i * 3 + 0];
                            rgba[i * 4 + 1] = pixels[i * 3 + 1];
                            rgba[i * 4 + 2] = pixels[i * 3 + 2];
                            rgba[i * 4 + 3] = 255;
                        }
                        gpu_texture_update(app->preview_tex, rgba, w * 4);
                        free(rgba);
                    }
                }
                cs_strlcpy(app->preview_path, path, sizeof(app->preview_path));
                stbi_image_free(pixels);
            }
        }
    }

    if (app->toast_until_ms && now > app->toast_until_ms) {
        app->toast_until_ms = 0;
        app->toast[0] = '\0';
    }
}



static void runtime_publish(App *app, bool active, bool ready, bool error, bool server_ready,
                            RuntimeStage stage, int percent, const char *message,
                            const char *detail, const char *error_text, const char *label)
{
    SDL_LockMutex(app->mu);
    RuntimeView *v = &app->runtime_view;
    v->active = active;
    v->ready = ready;
    v->error = error;
    v->server_ready = server_ready;
    v->stage = stage;
    v->percent = percent;
    cs_strlcpy(v->message, message ? message : "", sizeof(v->message));
    cs_strlcpy(v->detail, detail ? detail : "", sizeof(v->detail));
    cs_strlcpy(v->error_text, error_text ? error_text : "", sizeof(v->error_text));
    cs_strlcpy(v->server_label, label ? label : "", sizeof(v->server_label));
    SDL_UnlockMutex(app->mu);
}

static void runtime_progress_cb(void *ctx, const PythonRuntime *rt)
{
    App *app = ctx;
    runtime_publish(app, true, rt->ready, rt->stage == RT_STAGE_ERROR, false, rt->stage,
                    rt->percent, rt->message, rt->detail,
                    rt->stage == RT_STAGE_ERROR ? rt->error : "", NULL);
}

static void runtime_wait_retry(App *app)
{
    while (!SDL_GetAtomicInt(&app->quit)) {
        SDL_LockMutex(app->mu);
        bool retry = app->retry_runtime;
        app->retry_runtime = false;
        SDL_UnlockMutex(app->mu);
        if (retry) return;
        SDL_Delay(200);
    }
}

static int SDLCALL cellscope_bootstrap_thread(void *data)
{
    App *app = data;
    PythonRuntime *rt = &app->runtime;
    while (!SDL_GetAtomicInt(&app->quit)) {
        bool hf_requested = strcmp(app->settings.reader_backend, "hf") == 0;
        bool hf_usable = hf_requested && app->settings.hf_model_path[0] &&
                         cs_is_dir(app->settings.hf_model_path);
        if (hf_usable) {
            char cfg[CS_PATH_MAX];
            cs_path_join(cfg, sizeof(cfg), app->settings.hf_model_path, "config.json");
            if (!cs_file_exists(cfg)) hf_usable = false;
        }
        if (hf_requested && !hf_usable)
            app_log(app, "HF model configured but unavailable; using MiniMind fallback");

        runtime_publish(app, true, false, false, false, RT_STAGE_PREPARE, 0,
                        hf_usable ? "Preparing the Hugging Face reader…"
                                  : "Preparing local AI runtime…",
                        "", "", NULL);
        bool ok = runtime_ensure(rt, runtime_progress_cb, app, !hf_usable);
        if (SDL_GetAtomicInt(&app->quit)) break;
        if (!ok) {
            runtime_publish(app, true, false, true, false, RT_STAGE_ERROR, rt->percent,
                            "Local AI setup failed", rt->detail, rt->error, NULL);
            runtime_wait_retry(app);
            continue;
        }

        bool hf = hf_usable;
        bool hf_env_ready = false;
        if (hf) {
            char eerr[512] = {0};
            hf_env_ready = runtime_select_hf_env(rt, app->settings.hf_model, eerr,
                                                 sizeof(eerr));
            if (!hf_env_ready)
                app_log(app, "HF model environment unavailable (%s); using base runtime",
                        eerr[0] ? eerr : "not prepared");
            else
                app_log(app, "using model-specific Python: %s", rt->hf_python);
        }
        char label[192];
        char model_id[64] = "minimind-meter";
        char script[CS_PATH_MAX], log[CS_PATH_MAX];
        char detail[1200] = {0};
        char loading[512] = {0};
        const char *weight = NULL;
        const char *save_dir = NULL;
        int port = rt->port;

        if (hf) {
            snprintf(label, sizeof(label), "HF · %s", app->settings.hf_model[0]
                                                        ? app->settings.hf_model
                                                        : "local model");
            cs_strlcpy(model_id, "cellscope-hf", sizeof(model_id));
            port = app->settings.hf_port;
            cs_path_join(script, sizeof(script), rt->scripts, "serve_hf_vlm.py");
            cs_path_join(log, sizeof(log), rt->dir, "hf-server.log");
            snprintf(loading, sizeof(loading), "Loading %s…",
                     app->settings.hf_model[0] ? app->settings.hf_model : "HF model");
            snprintf(detail, sizeof(detail),
                     "Serving the downloaded Hugging Face snapshot from %s.",
                     app->settings.hf_model_path);
        } else {
            char custom[CS_PATH_MAX];
            bool custom_ok = runtime_custom_checkpoint(rt, custom, sizeof(custom));
            char outdir[CS_PATH_MAX];
            cs_path_join(outdir, sizeof(outdir), rt->minimind, "out");
            weight = custom_ok ? "cellscope_meter" : "sft_vlm";
            save_dir = custom_ok ? rt->training : outdir;
            cs_strlcpy(label, custom_ok ? "MiniMind · trained" : "MiniMind · baseline",
                       sizeof(label));
            cs_path_join(script, sizeof(script), rt->scripts, "serve_minimind_vlm.py");
            cs_path_join(log, sizeof(log), rt->dir, "minimind-server.log");
            if (custom_ok) {
                cs_strlcpy(loading, "Loading the MiniMind checkpoint…", sizeof(loading));
                cs_strlcpy(detail, "Using your trained cellscope_meter checkpoint.",
                           sizeof(detail));
            } else {
                cs_strlcpy(loading, "Loading the baseline MiniMind checkpoint…",
                           sizeof(loading));
                cs_strlcpy(detail,
                           "No trained checkpoint yet — using the baseline MiniMind SFT "
                           "model. Train with --train for meter accuracy.",
                           sizeof(detail));
            }
            if (hf_requested && !hf_usable) {
                cs_strlcat(detail, " The configured HF model was unavailable; falling back.",
                           sizeof(detail));
            }
        }

        runtime_publish(app, true, true, false, false, RT_STAGE_READY, 98, loading, detail,
                        "", label);
        server_stop(&app->server);
        char serr[1024] = {0};
        bool started = false;
        if (hf) {
            started = server_start_hf(&app->server,
                                      hf_env_ready ? rt->hf_python : rt->python, script,
                                      app->settings.hf_model_path, port,
                                      app->settings.hf_device, log, serr, sizeof(serr));
        } else {
            started = server_start(&app->server, rt->python, script, rt->minimind,
                                   save_dir, weight, port, log, serr, sizeof(serr));
        }
        if (!started) {
            runtime_publish(app, true, true, true, false, RT_STAGE_ERROR, 98,
                            hf ? "Hugging Face reader could not start"
                               : "MiniMind server could not start",
                            "", serr, NULL);
            runtime_wait_retry(app);
            continue;
        }
        int timeout = hf ? 900000 : 180000;
        if (!server_wait_ready(&app->server, timeout, &rt->cancel)) {
            runtime_publish(app, true, true, true, false, RT_STAGE_ERROR, 98,
                            hf ? "Hugging Face reader did not become ready"
                               : "MiniMind server did not become ready",
                            app->server.error, app->server.error, NULL);
            server_stop(&app->server);
            runtime_wait_retry(app);
            continue;
        }
        char url[128];
        snprintf(url, sizeof(url), "http://127.0.0.1:%d", port);
        SDL_LockMutex(app->mu);
        app->prefer_minimind = true;
        app->vlm_switched = false;
        cs_strlcpy(app->minimind_url, url, sizeof(app->minimind_url));
        cs_strlcpy(app->local_reader_label, label, sizeof(app->local_reader_label));
        cs_strlcpy(app->local_reader_model, model_id, sizeof(app->local_reader_model));
        cs_strlcpy(app->reader.backend, label, sizeof(app->reader.backend));
        cs_strlcpy(app->reader.detail, detail, sizeof(app->reader.detail));
        app->reader.kind = VLM_BACKEND_LLAMA;
        app->reader.is_vlm = true;
        cs_strlcpy(app->reader.status, "Local reader ready", sizeof(app->reader.status));
        SDL_UnlockMutex(app->mu);
        runtime_publish(app, false, true, false, true, RT_STAGE_READY, 100,
                        hf ? "Hugging Face reader ready" : "MiniMind reader ready", "", "",
                        label);
        app_log(app, "%s reader ready: %s (%s)", hf ? "HF" : "MiniMind", url, label);

        int failures = 0;
        while (!SDL_GetAtomicInt(&app->quit)) {
            SDL_Delay(3000);
            bool reload = false;
            SDL_LockMutex(app->mu);
            reload = app->reload_runtime;
            app->reload_runtime = false;
            SDL_UnlockMutex(app->mu);
            if (reload) {
                server_stop(&app->server);
                SDL_LockMutex(app->mu);
                app->prefer_minimind = false;
                app->vlm_switched = false;
                SDL_UnlockMutex(app->mu);
                break;
            }
            if (server_healthy(&app->server)) {
                failures = 0;
                continue;
            }
            failures++;
            if (failures < 2) continue;
            SDL_LockMutex(app->mu);
            app->prefer_minimind = false;
            SDL_UnlockMutex(app->mu);
            runtime_publish(app, true, true, true, false, RT_STAGE_ERROR, 100,
                            "Local reader server stopped", app->server.error,
                            "The local reader server stopped. Retrying…", label);
            server_stop(&app->server);
            break;
        }
        if (SDL_GetAtomicInt(&app->quit)) break;
        SDL_Delay(1500);
    }
    server_stop(&app->server);
    SDL_LockMutex(app->mu);
    app->prefer_minimind = false;
    SDL_UnlockMutex(app->mu);
    return 0;
}

void app_retry_runtime(App *app)
{
    SDL_LockMutex(app->mu);
    app->retry_runtime = true;
    SDL_UnlockMutex(app->mu);
}

static void demo_publish_cell(App *app)
{
    CellSample s = make_demo_cell(cs_epoch_ms(), app->cell_count, app->wifi_salt);
    SDL_LockMutex(app->mu);
    cell_push_locked(app, &s);
    app->adb.state = ADB_STATE_CONNECTED;
    cs_strlcpy(app->adb.serial, "DEMO", sizeof(app->adb.serial));
    cs_strlcpy(app->adb.model, "Pixel 8 (demo)", sizeof(app->adb.model));
    cs_strlcpy(app->adb.detail, "Synthetic telephony source. No phone is connected.",
               sizeof(app->adb.detail));
    app->modal_checking = false;
    app->ever_connected = true;
    app->last_poll_ok_ms = cs_now_ms();
    cs_strlcpy(app->device_label, "Pixel 8 · demo", sizeof(app->device_label));
    SDL_UnlockMutex(app->mu);
}

static int SDLCALL cellscope_adb_thread(void *data)
{
    App *app = data;
    char *dumpsys = malloc(ADB_DUMPSYS_MAX);
    if (!dumpsys) return 1;
    int64_t last_poll = 0;
    int64_t last_wifi_scan = 0;
    AdbState last_state = ADB_STATE_UNKNOWN;
    while (!SDL_GetAtomicInt(&app->quit)) {
        int64_t now = cs_now_ms();
        bool demo = false, want = false, paused = false;
        AdbStatus st;
        char path[CS_PATH_MAX], serial[96];
        SDL_LockMutex(app->mu);
        paused = app->paused;
        demo = app->demo || app->demo_phone;
        st = app->adb;
        cs_strlcpy(path, app->adb_path, sizeof(path));
        cs_strlcpy(serial, st.serial, sizeof(serial));
        want = app->want_check;
        app->want_check = false;
        if (want) app->modal_checking = true;
        SDL_UnlockMutex(app->mu);

        /* Process one-shot connection checks even while paused so the startup
         * modal remains usable. Do not poll or store samples while paused. */
        if (want) {
            AdbStatus ns;
            adb_status_init(&ns);
            cs_strlcpy(ns.adb_path, path, sizeof(ns.adb_path));
            printf("[adb] checking %s\n", path[0] ? path : "(adb not found)");
            bool ok = adb_check(path[0] ? path : NULL, &ns);
            SDL_LockMutex(app->mu);
            app->adb = ns;
            app->last_check_ms = cs_now_ms();
            app->modal_checking = false;
            if (ok) {
                app->ever_connected = true;
                app->poll_failures = 0;
                cs_strlcpy(app->device_label,
                           ns.model[0] ? ns.model : ns.serial,
                           sizeof(app->device_label));
            }
            SDL_UnlockMutex(app->mu);
            if (ok) {
                last_poll = 0;
            } else if (ns.state != last_state) {
                app_log(app, "adb state %d: %s", (int)ns.state, ns.detail);
            }
            last_state = ns.state;
            SDL_Delay(80);
            continue;
        }

        if (demo) {
            if (now - app->last_demo_cell_ms >= 1000) {
                demo_publish_cell(app);
                app->last_demo_cell_ms = now;
            }
            SDL_Delay(80);
            continue;
        }

        if (paused) {
            SDL_Delay(120);
            continue;
        }

        SDL_LockMutex(app->mu);
        st = app->adb;
        cs_strlcpy(serial, st.serial, sizeof(serial));
        SDL_UnlockMutex(app->mu);
        if (st.state != ADB_STATE_CONNECTED || !serial[0]) {
            SDL_Delay(120);
            continue;
        }
        if (now - last_poll < 1000) {
            SDL_Delay(80);
            continue;
        }
        last_poll = now;
        char err[512] = {0};
        bool ok = adb_poll_dumpsys(path, serial, dumpsys, ADB_DUMPSYS_MAX, err, sizeof(err));
        if (ok) {
            TelephonyRaw raw;
            if (telephony_parse(dumpsys, &raw)) {
                CellSample s;
                telephony_to_sample(&raw, &s);
                char wifi_out[32768], wifi_err[256];
                int wifi_n = 0;
                WifiAp aps[32];
                if (now - last_wifi_scan > 10000) {
                    adb_wifi_start_scan(path, serial);
                    last_wifi_scan = now;
                }
                if (adb_wifi_scan(path, serial, wifi_out, sizeof(wifi_out), wifi_err,
                                  sizeof(wifi_err))) {
                    wifi_n = wifi_parse_scan(wifi_out, app->wifi_salt, aps, 32);
                }
                wifi_format(aps, wifi_n, s.wifi, sizeof(s.wifi));
                s.ts_ms = cs_epoch_ms();
                cs_strlcpy(s.source, st.model[0] ? st.model : serial, sizeof(s.source));
                SDL_LockMutex(app->mu);
                if (!app->paused) {
                    cell_push_locked(app, &s);
                    app->last_poll_ok_ms = cs_now_ms();
                    app->poll_failures = 0;
                }
                SDL_UnlockMutex(app->mu);
            } else {
                if (now - app->last_dump_save_ms > 10000) {
                    char raw_path[CS_PATH_MAX];
                    cs_path_join(raw_path, sizeof(raw_path), app->capture_dir,
                                 "last_dumpsys.txt");
                    cs_write_file(raw_path, dumpsys, strlen(dumpsys));
                    app->last_dump_save_ms = now;
                }
                SDL_LockMutex(app->mu);
                cs_strlcpy(app->adb.detail,
                           "Connected, but telephony.registry exposes no cell identity "
                           "(OEM restriction or airplane mode).",
                           sizeof(app->adb.detail));
                SDL_UnlockMutex(app->mu);
            }
        } else {
            SDL_LockMutex(app->mu);
            app->poll_failures++;
            if (app->poll_failures >= 2) {
                app->adb.state = ADB_STATE_ERROR;
                snprintf(app->adb.detail, sizeof(app->adb.detail),
                         "Lost access to the phone: %s", err[0] ? err : "dumpsys failed");
                cs_strlcpy(app->adb.hint,
                           "Replug the phone or run `adb devices` to check authorization. "
                           "CellScope will retry automatically.",
                           sizeof(app->adb.hint));
                app->last_check_ms = 0;
            }
            SDL_UnlockMutex(app->mu);
        }
        SDL_Delay(80);
    }
    free(dumpsys);
    return 0;
}

static int SDLCALL cellscope_vlm_thread(void *data)
{
    App *app = data;
    uint8_t *rgb = NULL;
    uint8_t *read_rgb = NULL;
    int rw = 0, rh = 0;
    int read_w = 0, read_h = 0;
    int64_t last_scan_ms = 0;
    while (!SDL_GetAtomicInt(&app->quit)) {
        int64_t now = cs_now_ms();
        bool paused = false;
        SDL_LockMutex(app->mu);
        paused = app->paused;
        if (paused) {
            app->reader.busy = false;
            cs_strlcpy(app->reader.status, "Paused — press play to resume",
                       sizeof(app->reader.status));
        }
        SDL_UnlockMutex(app->mu);
        if (paused) {
            SDL_Delay(120);
            continue;
        }
        bool prefer = false;
        char prefer_url[128] = {0};
        char prefer_model[128] = {0};
        char prefer_label[128] = {0};
        bool switched = false;
        SDL_LockMutex(app->mu);
        prefer = app->prefer_minimind && !app->vlm_explicit;
        cs_strlcpy(prefer_url, app->minimind_url, sizeof(prefer_url));
        cs_strlcpy(prefer_model, app->local_reader_model, sizeof(prefer_model));
        cs_strlcpy(prefer_label, app->local_reader_label, sizeof(prefer_label));
        switched = app->vlm_switched;
        SDL_UnlockMutex(app->mu);

        if (prefer && !switched) {
            vlm_config_init(&app->vlm, "llama", prefer_url,
                            prefer_model[0] ? prefer_model : "minimind-meter");
            vlm_config_probe(&app->vlm);
            if (app->vlm.active == VLM_BACKEND_LLAMA) {
                cs_strlcpy(app->vlm.label, prefer_label[0] ? prefer_label : "MiniMind · local",
                           sizeof(app->vlm.label));
                snprintf(app->vlm.detail, sizeof(app->vlm.detail),
                         "%s served by CellScope's private Python runtime.",
                         prefer_label[0] ? prefer_label : "MiniMind");
            }
            app->vlm_switched = true;
            SDL_LockMutex(app->mu);
            cs_strlcpy(app->reader.backend, app->vlm.label, sizeof(app->reader.backend));
            cs_strlcpy(app->reader.detail, app->vlm.detail, sizeof(app->reader.detail));
            app->reader.kind = app->vlm.active;
            app->reader.is_vlm = app->vlm.is_vlm;
            cs_strlcpy(app->reader.status, "Connecting to MiniMind…", sizeof(app->reader.status));
            SDL_UnlockMutex(app->mu);
            app_log(app, "switched to local reader server: %s", app->vlm.detail);
        } else if (!prefer && switched) {
            vlm_config_init(&app->vlm, app->vlm_mode, app->vlm_url, app->vlm_model);
            vlm_config_probe(&app->vlm);
            app->vlm_switched = false;
            SDL_LockMutex(app->mu);
            cs_strlcpy(app->reader.backend, app->vlm.label, sizeof(app->reader.backend));
            cs_strlcpy(app->reader.detail, app->vlm.detail, sizeof(app->reader.detail));
            app->reader.kind = app->vlm.active;
            app->reader.is_vlm = app->vlm.is_vlm;
            SDL_UnlockMutex(app->mu);
            app_log(app, "switched away from MiniMind server: %s", app->vlm.label);
        }

        if (!app->vlm.probed || (!app->vlm.available &&
                                 now - app->vlm.last_probe_ms > 15000)) {
            vlm_config_probe(&app->vlm);
            SDL_LockMutex(app->mu);
            cs_strlcpy(app->reader.backend, app->vlm.label, sizeof(app->reader.backend));
            cs_strlcpy(app->reader.detail, app->vlm.detail, sizeof(app->reader.detail));
            app->reader.kind = app->vlm.active;
            app->reader.is_vlm = app->vlm.is_vlm;
            if (!app->vlm.available) {
                cs_strlcpy(app->reader.status, "Reader unavailable", sizeof(app->reader.status));
                cs_strlcpy(app->reader.error, app->vlm.detail, sizeof(app->reader.error));
            } else {
                cs_strlcpy(app->reader.status, "Watching for a display…",
                           sizeof(app->reader.status));
                app->reader.error[0] = '\0';
            }
            SDL_UnlockMutex(app->mu);
            app_log(app, "reader backend: %s", app->vlm.label);
        }
        if (!app->vlm.available) {
            SDL_Delay(300);
            continue;
        }

        bool mock = app->vlm.active == VLM_BACKEND_MOCK;
        if (now - last_scan_ms < 1000) {
            SDL_Delay(20);
            continue;
        }
        last_scan_ms = now;

        bool have = false;
        bool candidate = false;
        bool consensus_complete = false;
        int64_t frame_ts = 0;
        float frame_score = 0;
        SDL_LockMutex(app->mu);
        consensus_complete = app->visibility.committed;
        if (mock) {
            have = true;
            candidate = true;
        } else {
            int64_t now_epoch = cs_epoch_ms();
            int best = -1;
            float best_score = 0;
            float current_best = 0;
            for (int i = 0; i < BURST_MAX; i++) {
                BurstFrame *f = &app->burst[i];
                if (!f->rgb) continue;
                if (now_epoch - f->ts_ms > 6000) continue;
                if (f->score > current_best) current_best = f->score;
                if (f->used) continue;
                float age = (float)(now_epoch - f->ts_ms);
                float effective = f->score - 0.25f * f->motion +
                                  0.03f * (1.0f - age / 6000.0f);
                if (effective > best_score) {
                    best_score = effective;
                    best = i;
                }
            }
            app->reader.best_score = current_best;

            /* Always retain a copy of the newest frame for the once-per-second
             * presence check. */
            if (app->latest_rgb && app->latest_w > 0 && app->latest_h > 0) {
                size_t bytes = (size_t)app->latest_w * app->latest_h * 3;
                uint8_t *copy = realloc(rgb, bytes);
                if (copy) {
                    rgb = copy;
                    memcpy(rgb, app->latest_rgb, bytes);
                    rw = app->latest_w;
                    rh = app->latest_h;
                    have = true;
                }
            }

            /* Separately copy the best DSP-passing candidate for frequency
             * extraction. */
            if (best >= 0 && best_score >= 0.12f) {
                BurstFrame *f = &app->burst[best];
                size_t bytes = (size_t)f->w * f->h * 3;
                uint8_t *copy = realloc(read_rgb, bytes);
                if (copy) {
                    read_rgb = copy;
                    memcpy(read_rgb, f->rgb, bytes);
                    read_w = f->w;
                    read_h = f->h;
                    frame_ts = f->ts_ms;
                    frame_score = f->score;
                    f->used = true;
                    candidate = true;
                    have = true;
                    app->reader.best_score = f->score;
                    app->reader.frame_w = f->w;
                    app->reader.frame_h = f->h;
                }
            }
            if (!rgb && candidate) {
                size_t bytes = (size_t)read_w * read_h * 3;
                uint8_t *copy = realloc(rgb, bytes);
                if (copy) {
                    rgb = copy;
                    memcpy(rgb, read_rgb, bytes);
                    rw = read_w;
                    rh = read_h;
                }
            }
            app->reader.burst_count = app->burst_count;
        }
        if (have) {
            app->reader.busy = true;
            app->reader.last_attempt_ms = now;
            app->reader.attempts++;
            if (mock)
                cs_strlcpy(app->reader.status, "Reading synthetic frame…",
                           sizeof(app->reader.status));
            else if (app->reader.display_until_ms == 0) {
                if (candidate)
                    snprintf(app->reader.status, sizeof(app->reader.status),
                             "Candidate frame · q=%d%% · 1 Hz scan",
                             (int)(frame_score * 100.0f));
                else
                    cs_strlcpy(app->reader.status, "Scanning for a meter screen (1 Hz)…",
                               sizeof(app->reader.status));
            }
        } else if (!mock) {
            if (app->burst_count == 0)
                cs_strlcpy(app->reader.status, "Point a meter screen at the guide",
                           sizeof(app->reader.status));
            else if (app->reader.best_score < 0.15f)
                cs_strlcpy(app->reader.status, "Move the meter closer — frame is blurry or dark",
                           sizeof(app->reader.status));
            else
                cs_strlcpy(app->reader.status, "Buffering frames — hold the screen steady",
                           sizeof(app->reader.status));
        }
        SDL_UnlockMutex(app->mu);
        if (!have) {
            SDL_Delay(20);
            continue;
        }

        if (!mock && candidate && !consensus_complete) {
            /* During the active attempt, a DSP candidate is already screen
             * evidence. Skip the separate presence call so frequency reads
             * arrive close enough together to reach consensus while the meter
             * is still in view. */
            SDL_LockMutex(app->mu);
            if (!app->reader.screen_visible) {
                app->reader.screen_visible = true;
                app->reader.visible = false;
                app->reader.display_until_ms = 0;
                app->reader.hz = 0;
                app->reader.confidence = 0;
                app->reader.raw[0] = '\0';
            }
            app->reader.busy = true;
            app->reader.error[0] = '\0';
            SDL_UnlockMutex(app->mu);
        } else if (!mock) {
            bool display_visible = false;
            char derr[512] = {0};
            if (!vlm_detect(&app->vlm, rgb, rw, rh, &display_visible, derr,
                            sizeof(derr))) {
                SDL_LockMutex(app->mu);
                app->reader.busy = false;
                app->reader.live_valid = false;
                cs_strlcpy(app->reader.error, derr, sizeof(app->reader.error));
                cs_strlcpy(app->reader.status, "Presence check failed — retrying",
                           sizeof(app->reader.status));
                SDL_UnlockMutex(app->mu);
                SDL_Delay(120);
                continue;
            }
            if (!display_visible) {
                SDL_LockMutex(app->mu);
                app->reader.busy = false;
                app->reader.screen_visible = false;
                app->reader.visible = false;
                app->reader.live_valid = false;
                app->reader.live_hz = 0;
                app->reader.live_raw[0] = '\0';
                visibility_commit_locked(app);
                cs_strlcpy(app->reader.status, "No device screen visible — watching",
                           sizeof(app->reader.status));
                SDL_UnlockMutex(app->mu);
                continue;
            }
            SDL_LockMutex(app->mu);
            if (!app->reader.screen_visible) {
                app->reader.screen_visible = true;
                app->reader.visible = false;
                app->reader.display_until_ms = 0;
                app->reader.hz = 0;
                app->reader.confidence = 0;
                app->reader.raw[0] = '\0';
            }
            app->reader.busy = true;
            if (consensus_complete) {
                app->reader.live_valid = false;
                app->reader.live_hz = 0;
                app->reader.live_raw[0] = '\0';
                cs_strlcpy(app->reader.status,
                           "Consensus recorded — move the screen away to start a new reading",
                           sizeof(app->reader.status));
            } else if (!candidate) {
                app->reader.live_valid = false;
                app->reader.live_hz = 0;
                app->reader.live_raw[0] = '\0';
                cs_strlcpy(app->reader.status,
                           "Screen visible — waiting for a readable candidate",
                           sizeof(app->reader.status));
            }
            SDL_UnlockMutex(app->mu);
            if (!candidate || consensus_complete) {
                SDL_Delay(20);
                continue;
            }
        }

        MeterReading r;
        char err[512] = {0};
        int64_t frame_ms = 0;
        bool ok = vlm_read(&app->vlm, candidate ? read_rgb : rgb,
                           candidate ? read_w : rw, candidate ? read_h : rh,
                           app->default_unit, &r, &frame_ms, err, sizeof(err));
        bool paused_now = false;
        SDL_LockMutex(app->mu);
        paused_now = app->paused;
        if (paused_now) app->reader.busy = false;
        SDL_UnlockMutex(app->mu);
        if (paused_now) continue;
        if (!ok) {
            SDL_LockMutex(app->mu);
            app->reader.busy = false;
            app->reader.live_valid = false;
            app->reader.live_raw[0] = '\0';
            cs_strlcpy(app->reader.error, err, sizeof(app->reader.error));
            cs_strlcpy(app->reader.status, "No reading — adjust the meter", sizeof(app->reader.status));
            SDL_UnlockMutex(app->mu);
            SDL_Delay(120);
            continue;
        }

        if (mock) {
            if (!r.visible || r.hz <= 0) {
                meter_tracker_update(&app->tracker, &r, now);
                SDL_LockMutex(app->mu);
                app->reader.busy = false;
                app->reader.visible = false;
                app->reader.screen_visible = false;
                app->reader.live_valid = false;
                app->reader.error[0] = '\0';
                cs_strlcpy(app->reader.status, "Watching for a display…",
                           sizeof(app->reader.status));
                SDL_UnlockMutex(app->mu);
                continue;
            }
            if (r.confidence > 0 && r.confidence < 0.45) {
                SDL_LockMutex(app->mu);
                app->reader.busy = false;
                cs_strlcpy(app->reader.status, "Low confidence — hold the meter steady",
                           sizeof(app->reader.status));
                app->reader.confidence = r.confidence;
                SDL_UnlockMutex(app->mu);
                continue;
            }
            MeterTrackAction action = meter_tracker_update(&app->tracker, &r, now);
            int stable = meter_tracker_stable(&app->tracker);
            SDL_LockMutex(app->mu);
            app->reader.busy = false;
            app->reader.screen_visible = true;
            app->reader.live_valid = true;
            app->reader.live_hz = r.hz;
            app->reader.live_confidence = r.confidence;
            app->reader.live_ms = frame_ts ? frame_ts : (frame_ms ? frame_ms : cs_epoch_ms());
            cs_strlcpy(app->reader.live_raw, r.raw, sizeof(app->reader.live_raw));
            app->reader.stable = stable;
            app->reader.last_ok_ms = cs_epoch_ms();
            cs_strlcpy(app->reader.backend, app->vlm.label, sizeof(app->reader.backend));
            app->reader.error[0] = '\0';
            if (action == METER_TRACK_RECORD) {
                MeterSample ms;
                memset(&ms, 0, sizeof(ms));
                ms.ts_ms = frame_ts ? frame_ts : (frame_ms ? frame_ms : cs_epoch_ms());
                cs_format_time_ms(ms.ts_ms, ms.time_text, sizeof(ms.time_text));
                ms.hz = (uint64_t)(r.hz + 0.5);
                ms.confidence = r.confidence;
                cs_strlcpy(ms.raw, r.raw, sizeof(ms.raw));
                cs_strlcpy(ms.backend, app->vlm.label, sizeof(ms.backend));
                ms.demo = true;
                meter_push_locked(app, &ms);
                app->reader.accepted++;
                app->reader.visible = true;
                app->reader.display_until_ms = cs_now_ms() + 3000;
                app->reader.hz = r.hz;
                app->reader.confidence = r.confidence;
                cs_strlcpy(app->reader.raw, r.raw, sizeof(app->reader.raw));
                char eng[64];
                cs_format_eng(ms.hz, eng, sizeof(eng));
                snprintf(app->reader.status, sizeof(app->reader.status), "Recorded %s", eng);
            } else if (stable >= 2) {
                cs_strlcpy(app->reader.status, "Stable — waiting for a change",
                           sizeof(app->reader.status));
            } else {
                cs_strlcpy(app->reader.status, "Display detected — hold steady",
                           sizeof(app->reader.status));
            }
            SDL_UnlockMutex(app->mu);
            continue;
        }

        int64_t best_frame_ts = frame_ts ? frame_ts : (frame_ms ? frame_ms : cs_epoch_ms());
        SDL_LockMutex(app->mu);
        app->reader.busy = false;
        app->reader.last_ok_ms = cs_epoch_ms();
        cs_strlcpy(app->reader.backend, app->vlm.label, sizeof(app->reader.backend));
        app->reader.error[0] = '\0';
        if (candidate)
            visibility_update_locked(app, &r, read_rgb, read_w, read_h,
                                     best_frame_ts, frame_score);
        if (candidate && r.hz > 0) {
            app->reader.live_valid = true;
            app->reader.live_hz = r.hz;
            app->reader.live_confidence = r.confidence;
            app->reader.live_ms = best_frame_ts;
            cs_strlcpy(app->reader.live_raw, r.raw, sizeof(app->reader.live_raw));
            char mhz[64];
            snprintf(mhz, sizeof(mhz), "%.4f MHz", r.hz / 1e6);
            snprintf(app->reader.status, sizeof(app->reader.status),
                     "Screen visible · live %s · %d candidates", mhz,
                     app->visibility.frame_count);
        } else {
            app->reader.live_valid = false;
            app->reader.live_raw[0] = '\0';
            cs_strlcpy(app->reader.status,
                       "Candidate screen visible — reading digits",
                       sizeof(app->reader.status));
        }
        SDL_UnlockMutex(app->mu);
        SDL_Delay(80);
    }
    free(rgb);
    free(read_rgb);
    return 0;
}

void app_request_check(App *app)
{
    SDL_LockMutex(app->mu);
    app->want_check = true;
    app->modal_checking = true;
    app->modal_open = true;
    app->background_retry = true;
    SDL_UnlockMutex(app->mu);
    app_notify(app, false, "Checking for an Android device…");
}

void app_toggle_pause(App *app)
{
    SDL_LockMutex(app->mu);
    bool resuming = app->paused;
    SDL_UnlockMutex(app->mu);
    if (resuming) {
        if (app->demo && app_cell_count(app) == 0) app_seed_demo(app);
        if (!app->demo && !app->image_mode && !app->no_camera && !app->camera_ok)
            app_retry_camera(app, false);
    }

    SDL_LockMutex(app->mu);
    bool pause_now = !app->paused;
    bool session_ok = true;
    if (pause_now) {
        app->paused = true;
        app->reader.screen_visible = false;
        app->reader.live_valid = false;
        app->reader.live_raw[0] = '\0';
        visibility_commit_locked(app);
        session_write_locked(app);
        app->reader.busy = false;
        cs_strlcpy(app->reader.status, "Paused — press play to resume",
                   sizeof(app->reader.status));
        for (int i = 0; i < BURST_MAX; i++) {
            free(app->burst[i].rgb);
            memset(&app->burst[i], 0, sizeof(app->burst[i]));
        }
        app->burst_count = 0;
        app->burst_next = 0;
        app->reader.burst_count = 0;
    } else {
        session_ok = session_begin_locked(app);
        app->paused = false;
        cs_strlcpy(app->reader.status, "Resuming camera and ADB…",
                   sizeof(app->reader.status));
    }
    char session[CS_PATH_MAX];
    cs_strlcpy(session, app->session_dir, sizeof(session));
    SDL_UnlockMutex(app->mu);
    if (pause_now) {
        /* Release the camera so macOS turns off its mandatory camera-in-use
         * privacy indicator. It is reopened on resume. */
        camera_close(&app->cam);
        app->camera_ok = false;
    }
    app_log(app, pause_now ? "paused" : "resumed");
    if (pause_now) {
        app_notify(app, false, "Paused webcam and ADB. RF writes stopped; dataset remains open.");
    } else if (!session_ok) {
        app_notify(app, true, "Resumed, but could not create a dataset folder.");
    } else {
        app_notify(app, false, "Capturing to %s", session);
    }
}

void app_notify(App *app, bool error, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    cs_strlcpy(app->toast, buf, sizeof(app->toast));
    app->toast_until_ms = cs_now_ms() + (error ? 7000 : 4500);
    app->toast_error = error;
    app_log(app, "%s%s", error ? "error: " : "info: ", buf);
}

void app_copy_diagnostics(App *app)
{
    char text[4096];
    AdbStatus st;
    ReaderView rv;
    RuntimeView rt;
    SDL_LockMutex(app->mu);
    st = app->adb;
    rv = app->reader;
    rt = app->runtime_view;
    SDL_UnlockMutex(app->mu);
    snprintf(text, sizeof(text),
             "CellScope diagnostics\nadb: %s\nstate: %d\ndetail: %s\nhint: %s\nserial: %s "
             "model: %s\nreader: %s\nreader status: %s\nreader error: %s\n"
             "runtime: %d%% %s\nruntime error: %s\nraw adb:\n%s\n",
             st.adb_path, (int)st.state, st.detail, st.hint, st.serial, st.model,
             rv.backend, rv.status, rv.error, rt.percent, rt.message, rt.error_text,
             st.raw);
    SDL_SetClipboardText(text);
    app_notify(app, false, "Diagnostics copied to the clipboard.");
}

static const char *file_basename(const char *path)
{
    if (!path) return "";
    const char *a = strrchr(path, '/');
    const char *b = strrchr(path, '\\');
    const char *p = NULL;
    if (a && b) p = b > a ? b : a;
    else p = a ? a : b;
    return p ? p + 1 : path;
}

static bool has_json_suffix(const char *path)
{
    size_t n = path ? strlen(path) : 0;
    return n >= 5 && (cs_ends_with(path, ".json") || cs_ends_with(path, ".JSON"));
}

static void json_write_string(FILE *f, const char *s)
{
    char esc[8192];
    cs_json_escape(s, esc, sizeof(esc));
    fputc('"', f);
    fputs(esc, f);
    fputc('"', f);
}

static void json_meter_obj(FILE *f, const MeterSample *m)
{
    const char *shot = m->shot_name[0] ? m->shot_name : file_basename(m->path);
    fprintf(f, "{\"timestamp_unix_ms\": %lld, \"seq\": %llu, \"time\": ",
            (long long)m->ts_ms, (unsigned long long)m->seq);
    json_write_string(f, m->time_text);
    fprintf(f, ", \"frequency_hz\": %llu, \"frequency_ghz\": %.9f, "
               "\"confidence\": %.3f, \"backend\": ",
            (unsigned long long)m->hz, (double)m->hz / 1e9, m->confidence);
    json_write_string(f, m->backend);
    fputs(", \"raw\": ", f);
    json_write_string(f, m->raw);
    fputs(", \"screenshot\": ", f);
    json_write_string(f, shot);
    fputs(", \"screenshot_path\": ", f);
    json_write_string(f, m->path);
    fprintf(f, ", \"demo\": %s}", m->demo ? "true" : "false");
}

static void json_cell_obj(FILE *f, const CellSample *c)
{
    char time[24];
    cs_format_time(c->ts_ms, time, sizeof(time));
    fprintf(f, "{\"timestamp_unix_ms\": %lld, \"seq\": %llu, \"time\": ",
            (long long)c->ts_ms, (unsigned long long)c->seq);
    json_write_string(f, time);
    fputs(", \"plmn\": ", f); json_write_string(f, c->plmn);
    fputs(", \"mcc\": ", f); json_write_string(f, c->mcc);
    fputs(", \"mnc\": ", f); json_write_string(f, c->mnc);
    fputs(", \"operator\": ", f); json_write_string(f, c->operator_name);
    fputs(", \"country\": ", f); json_write_string(f, c->country);
    fputs(", \"tac\": ", f); json_write_string(f, c->tac);
    fputs(", \"rat\": ", f); json_write_string(f, c->rat);
    fputs(", \"enb\": ", f); json_write_string(f, c->enb);
    fputs(", \"cid\": ", f); json_write_string(f, c->cid);
    fputs(", \"pci\": ", f); json_write_string(f, c->pci);
    fputs(", \"ta\": ", f); json_write_string(f, c->ta);
    fputs(", \"arfcn\": ", f); json_write_string(f, c->arfcn);
    fputs(", \"band\": ", f); json_write_string(f, c->band);
    fputs(", \"bandwidth\": ", f); json_write_string(f, c->bw);
    fprintf(f, ", \"ca\": %s, \"dl_frequency\": ", c->ca ? "true" : "false");
    json_write_string(f, c->f_dl);
    fputs(", \"ul_frequency\": ", f); json_write_string(f, c->f_ul);
    fputs(", \"wifi\": ", f); json_write_string(f, c->wifi);
    fputs(", \"source\": ", f); json_write_string(f, c->source);
    fprintf(f, ", \"demo\": %s}", c->demo ? "true" : "false");
}

/* Rewrite the two session files as valid JSON arrays after every new sample.
 * Datasets are small (a few thousand rows at 1 Hz), so this keeps the files
 * crash-safe and always parseable without a trailing-comma dance. */
static void session_write_locked(App *app)
{
    if (!app->session_active || !app->session_dir[0] || app->session_write_failed)
        return;
    char path[CS_PATH_MAX];
    FILE *f = NULL;
    bool failed = false;
    cs_path_join(path, sizeof(path), app->session_dir, "phone.json");
    f = fopen(path, "wb");
    if (!f) {
        failed = true;
    } else {
        fputs("[\n", f);
        bool first = true;
        for (int i = 0; i < app->cell_count; i++) {
            const CellSample *c = &app->cells[(app->cell_head + i) % CELL_MAX];
            if (c->seq < app->session_cell_start) continue;
            if (!first) fputs(",\n", f);
            json_cell_obj(f, c);
            first = false;
        }
        fputs("\n]\n", f);
        if (ferror(f) || fclose(f) != 0) failed = true;
    }
    cs_path_join(path, sizeof(path), app->session_dir, "meter.json");
    f = fopen(path, "wb");
    if (!f) {
        failed = true;
    } else {
        fputs("[\n", f);
        bool first = true;
        for (int i = 0; i < app->meter_count; i++) {
            const MeterSample *m = &app->meters[(app->meter_head + i) % METER_MAX];
            if (m->seq < app->session_meter_start) continue;
            if (!first) fputs(",\n", f);
            json_meter_obj(f, m);
            first = false;
        }
        fputs("\n]\n", f);
        if (ferror(f) || fclose(f) != 0) failed = true;
    }
    if (failed) {
        app->session_write_failed = true;
        app_log(app, "session JSON write failed in %s", app->session_dir);
    }
}

static bool session_begin_locked(App *app)
{
    if (app->session_active) return true;
    if (!app->datasets_dir[0]) {
        cs_path_join(app->datasets_dir, sizeof(app->datasets_dir), app->capture_dir,
                     "datasets");
    }
    if (!cs_mkdirs(app->datasets_dir)) return false;
    char stamp[32], name[64];
    cs_format_date_stamp(cs_epoch_ms(), stamp, sizeof(stamp));
    cs_strlcpy(name, stamp, sizeof(name));
    for (int i = 2; i < 1000; i++) {
        char candidate[CS_PATH_MAX];
        cs_path_join(candidate, sizeof(candidate), app->datasets_dir, name);
        if (!cs_is_dir(candidate)) break;
        snprintf(name, sizeof(name), "%s_%d", stamp, i);
    }
    cs_path_join(app->session_dir, sizeof(app->session_dir), app->datasets_dir, name);
    if (!cs_mkdirs(app->session_dir)) return false;
    cs_strlcpy(app->session_name, name, sizeof(app->session_name));
    app->session_started_ms = cs_epoch_ms();
    app->session_cell_start = app->cell_seq + 1;
    app->session_meter_start = app->meter_seq + 1;
    app->session_write_failed = false;
    app->session_active = true;
    session_write_locked(app);
    return !app->session_write_failed;
}

static void session_end_locked(App *app)
{
    if (!app->session_active) return;
    session_write_locked(app);
    app->session_active = false;
}

static bool session_rotate_locked(App *app)
{
    session_end_locked(app);
    return session_begin_locked(app);
}

static void app_export_json_to(App *app, const char *path)
{
    char final_path[CS_PATH_MAX];
    cs_strlcpy(final_path, path, sizeof(final_path));
    if (!has_json_suffix(final_path) && strlen(final_path) + 5 < sizeof(final_path))
        cs_strlcat(final_path, ".json", sizeof(final_path));
    path = final_path;
    FILE *f = fopen(path, "wb");
    if (!f) {
        app_notify(app, true, "Could not write %s", path);
        return;
    }
    int64_t exported = cs_epoch_ms();
    int readings = 0, cells = 0;
    SDL_LockMutex(app->mu);
    fprintf(f,
            "{\n  \"app\": \"CellScope\",\n  \"exported_at_unix_ms\": %lld,\n"
            "  \"readings\": [\n",
            (long long)exported);
    for (int i = 0; i < app->meter_count; i++) {
        const MeterSample *m = &app->meters[(app->meter_head + i) % METER_MAX];
        if (i) fputs(",\n", f);
        fputs("    ", f);
        json_meter_obj(f, m);
        readings++;
    }
    fputs("\n  ],\n  \"cell_samples\": [\n", f);
    for (int i = 0; i < app->cell_count; i++) {
        const CellSample *c = &app->cells[(app->cell_head + i) % CELL_MAX];
        if (i) fputs(",\n", f);
        fputs("    ", f);
        json_cell_obj(f, c);
        cells++;
    }
    fputs("\n  ]\n}\n", f);
    SDL_UnlockMutex(app->mu);
    bool ok = !ferror(f);
    if (fclose(f) != 0) ok = false;
    if (ok) {
        app_log(app, "exported JSON: %s", path);
        app_notify(app, false, "Exported %d readings and %d cell samples to %s", readings,
                   cells, path);
    } else {
        app_notify(app, true, "Could not finish writing %s", path);
    }
}

static bool sha512_hex(const char *path, char *hex, size_t cap, char *err, size_t err_cap)
{
    char tool[CS_PATH_MAX];
    if (proc_find_in_path("shasum", tool, sizeof(tool))) {
    } else if (!proc_find_in_path("sha512sum", tool, sizeof(tool)) &&
               !proc_find_in_path("certutil", tool, sizeof(tool))) {
        snprintf(err, err_cap, "No SHA-512 tool found (shasum, sha512sum, or certutil)");
        return false;
    }
    const char *argv[6];
    int n = 0;
    argv[n++] = tool;
    char tool_name[CS_PATH_MAX];
    cs_strlcpy(tool_name, file_basename(tool), sizeof(tool_name));
    bool certutil = strcmp(tool_name, "certutil") == 0;
    if (strcmp(tool_name, "shasum") == 0) {
        argv[n++] = "-a";
        argv[n++] = "512";
    } else if (certutil) {
        argv[n++] = "-hashfile";
    }
    argv[n++] = path;
    if (certutil) argv[n++] = "SHA512";
    argv[n] = NULL;

    char out[8192] = {0};
    int code = -1;
    bool timed_out = false;
    char perr[512] = {0};
    if (!proc_run(argv, out, sizeof(out), 120000, &code, &timed_out, perr, sizeof(perr))) {
        snprintf(err, err_cap, "SHA-512 command failed: %s", perr);
        return false;
    }
    if (code != 0 || timed_out) {
        snprintf(err, err_cap, "SHA-512 command exited %d: %.200s", code, out);
        return false;
    }
    size_t run = 0;
    for (const char *p = out; *p; p++) {
        if (isxdigit((unsigned char)*p)) {
            run++;
        } else {
            run = 0;
        }
        if (run == 128) {
            const char *start = p - 127;
            if (cap < 129) return false;
            for (int i = 0; i < 128; i++) hex[i] = (char)tolower((unsigned char)start[i]);
            hex[128] = '\0';
            return true;
        }
    }
    snprintf(err, err_cap, "Could not parse SHA-512 output: %.200s", out);
    return false;
}

static bool write_sig_file(const char *file, const char *hex, char *err, size_t err_cap)
{
    char sig[CS_PATH_MAX];
    snprintf(sig, sizeof(sig), "%s.sig", file);
    FILE *f = fopen(sig, "wb");
    if (!f) {
        snprintf(err, err_cap, "Could not write %s", sig);
        return false;
    }
    fprintf(f, "%s  %s\n", hex, file_basename(file));
    bool ok = fclose(f) == 0;
    if (!ok) snprintf(err, err_cap, "Could not finish %s", sig);
    return ok;
}

static bool hash_and_sig(const char *path, int *count, char *err, size_t err_cap)
{
    char hex[129];
    if (!sha512_hex(path, hex, sizeof(hex), err, err_cap)) return false;
    if (!write_sig_file(path, hex, err, err_cap)) return false;
    (*count)++;
    return true;
}

static bool hash_dataset_files(const char *dir, int *count, char *err, size_t err_cap)
{
    *count = 0;
#ifdef _WIN32
    char pattern[CS_PATH_MAX];
    snprintf(pattern, sizeof(pattern), "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        snprintf(err, err_cap, "Could not enumerate %s", dir);
        return false;
    }
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (cs_ends_with(fd.cFileName, ".sig")) continue;
        char path[CS_PATH_MAX];
        cs_path_join(path, sizeof(path), dir, fd.cFileName);
        if (!hash_and_sig(path, count, err, err_cap)) {
            FindClose(h);
            return false;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return true;
#else
    DIR *d = opendir(dir);
    if (!d) {
        snprintf(err, err_cap, "Could not open %s", dir);
        return false;
    }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        if (cs_ends_with(de->d_name, ".sig")) continue;
        char path[CS_PATH_MAX];
        cs_path_join(path, sizeof(path), dir, de->d_name);
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        if (!hash_and_sig(path, count, err, err_cap)) {
            closedir(d);
            return false;
        }
    }
    closedir(d);
    return true;
#endif
}

static bool run_tar(const char *archive, const char *parent, const char *name, char *err,
                    size_t err_cap)
{
    char tar[CS_PATH_MAX];
    if (!proc_find_in_path("tar", tar, sizeof(tar))) {
        snprintf(err, err_cap, "tar was not found in PATH");
        return false;
    }
    const char *argv[] = {tar, "-czf", archive, "-C", parent, name, NULL};
    char out[4096] = {0};
    int code = -1;
    bool timed_out = false;
    char perr[512] = {0};
    if (!proc_run(argv, out, sizeof(out), 300000, &code, &timed_out, perr, sizeof(perr))) {
        snprintf(err, err_cap, "tar failed: %s", perr);
        return false;
    }
    if (code != 0 || timed_out) {
        snprintf(err, err_cap, "tar exited %d: %.300s", code, out);
        return false;
    }
    return true;
}

static bool write_dataset_manifest(App *app, const char *dir, const char *name,
                                   char *err, size_t err_cap)
{
    char path[CS_PATH_MAX];
    cs_path_join(path, sizeof(path), dir, "manifest.json");
    FILE *f = fopen(path, "wb");
    if (!f) {
        snprintf(err, err_cap, "Could not write %s", path);
        return false;
    }
#if defined(_WIN32)
    const char *platform = "windows";
#elif defined(__APPLE__)
    const char *platform = "macos";
#else
    const char *platform = "linux";
#endif
    SDL_LockMutex(app->mu);
    int samples = app->cell_count;
    int readings = app->meter_count;
    SDL_UnlockMutex(app->mu);
    char escaped[CS_PATH_MAX * 2];
    cs_json_escape(name, escaped, sizeof(escaped));
    fprintf(f,
            "{\n"
            "  \"schema\": \"cellscope-dataset/1\",\n"
            "  \"dataset\": \"%s\",\n"
            "  \"created_at_unix_ms\": %lld,\n"
            "  \"platform\": \"%s\",\n"
            "  \"git_commit\": \"%s\",\n"
            "  \"cell_samples\": %d,\n"
            "  \"readings\": %d\n"
            "}\n",
            escaped, (long long)cs_epoch_ms(), platform, CELLSCOPE_GIT_COMMIT,
            samples, readings);
    bool ok = fclose(f) == 0;
    if (!ok) snprintf(err, err_cap, "Could not finish %s", path);
    return ok;
}

void app_export_dataset(App *app)
{
    char dir[CS_PATH_MAX] = {0};
    char name[128] = {0};
    app->last_export_archive[0] = '\0';
    app->last_export_sig[0] = '\0';
    SDL_LockMutex(app->mu);
    if (app->session_active) session_write_locked(app);
    cs_strlcpy(dir, app->session_dir, sizeof(dir));
    cs_strlcpy(name, app->session_name, sizeof(name));
    bool write_failed = app->session_write_failed;
    SDL_UnlockMutex(app->mu);
    if (!dir[0] || !cs_is_dir(dir)) {
        app_notify(app, true, "No current dataset to export. Start a capture first.");
        return;
    }
    SDL_LockMutex(app->mu);
    int sample_count = app->cell_count;
    int reading_count = app->meter_count;
    SDL_UnlockMutex(app->mu);
    if (sample_count == 0 && reading_count == 0) {
        app_notify(app, true,
                   "Dataset is empty: no cell samples or readings were captured. "
                   "Nothing was compressed or exported.");
        return;
    }
    if (write_failed) {
        app_notify(app, true, "The current dataset JSON could not be finalized; export aborted.");
        return;
    }
    if (!name[0]) cs_strlcpy(name, file_basename(dir), sizeof(name));

    char base[CS_PATH_MAX] = {0};
    if (app->capture_dir[0])
        cs_strlcpy(base, app->capture_dir, sizeof(base));
    else {
        char dir[CS_PATH_MAX];
        if (cs_app_data_dir(dir, sizeof(dir)))
            cs_strlcpy(base, dir, sizeof(base));
        else if (!getcwd(base, sizeof(base)))
            cs_strlcpy(base, ".", sizeof(base));
    }
    char exports[CS_PATH_MAX];
    cs_path_join(exports, sizeof(exports), base, "exports");
    if (!cs_mkdirs(exports)) {
        app_notify(app, true, "Could not create %s", exports);
        return;
    }

    char err[512] = {0};
    if (!write_dataset_manifest(app, dir, name, err, sizeof(err))) {
        app_notify(app, true, "Could not write the dataset manifest: %s", err);
        return;
    }
    int hashed = 0;
    if (!hash_dataset_files(dir, &hashed, err, sizeof(err))) {
        app_notify(app, true, "Could not checksum the dataset: %s", err);
        return;
    }

    char parent[CS_PATH_MAX];
    cs_strlcpy(parent, dir, sizeof(parent));
    char *slash = strrchr(parent, '/');
    if (slash && slash != parent) *slash = '\0';
    else cs_strlcpy(parent, ".", sizeof(parent));

    char archive[CS_PATH_MAX];
    snprintf(archive, sizeof(archive), "%s/dataset-%s.tar.gz", exports, name);
    if (!run_tar(archive, parent, file_basename(dir), err, sizeof(err))) {
        app_notify(app, true, "Could not archive the dataset: %s", err);
        return;
    }
    char hex[129];
    if (!sha512_hex(archive, hex, sizeof(hex), err, sizeof(err)) ||
        !write_sig_file(archive, hex, err, sizeof(err))) {
        app_notify(app, true, "Could not sign the export: %s", err);
        return;
    }
    cs_strlcpy(app->last_export_archive, archive, sizeof(app->last_export_archive));
    snprintf(app->last_export_sig, sizeof(app->last_export_sig), "%s.sig", archive);

    if (!app->silent_export && !getenv("CELLSCOPE_EXPORT_NO_OPEN")) {
        char url[CS_PATH_MAX + 16];
        snprintf(url, sizeof(url), "file://%s", exports);
        SDL_OpenURL(url);
    }
    app_log(app, "exported dataset %s (%d file signatures): %s", name, hashed, archive);
    if (!app->silent_export)
        app_notify(app, false, "Exported %d signed files and dataset-%s.tar.gz to %s", hashed,
                   name, exports);
}

void app_save_window_png(App *app, const char *path)
{
    if (!gpu_renderer_screenshot(app->r, path)) {
        app_log(app, "screenshot failed: %s", SDL_GetError());
    } else {
        app_log(app, "screenshot saved: %s", path);
    }
}

static void settings_field_append(App *app, const char *text)
{
    if (!text || !*text) return;
    char *dst = app->settings_edit[app->settings_focus];
    size_t n = strlen(dst);
    size_t add = strlen(text);
    if (n + add >= CS_PATH_MAX) add = CS_PATH_MAX - 1 - n;
    if (add) memcpy(dst + n, text, add);
    dst[n + add] = '\0';
}

static void settings_field_backspace(App *app)
{
    char *dst = app->settings_edit[app->settings_focus];
    size_t n = strlen(dst);
    if (!n) return;
    n--;
    while (n > 0 && ((unsigned char)dst[n] & 0xC0) == 0x80) n--;
    dst[n] = '\0';
}

void app_handle_event(App *app, const SDL_Event *e)
{
    if (!e) return;
    if (app->settings_modal_open) {
        if (e->type == SDL_EVENT_TEXT_INPUT) {
            settings_field_append(app, e->text.text);
            return;
        }
        if (e->type == SDL_EVENT_KEY_DOWN) {
            switch (e->key.key) {
                case SDLK_ESCAPE:
                    app_close_settings_modal(app);
                    return;
                case SDLK_RETURN:
                    app_save_settings_modal(app);
                    return;
                case SDLK_TAB:
                    if (e->key.mod & SDL_KMOD_SHIFT)
                        app->settings_focus = (app->settings_focus + 6) % 7;
                    else
                        app->settings_focus = (app->settings_focus + 1) % 7;
                    return;
                case SDLK_BACKSPACE:
                    settings_field_backspace(app);
                    return;
                default:
                    return;
            }
        }
    } else if (app->upload_modal_open) {
        if (e->type == SDL_EVENT_KEY_DOWN) {
            if (e->key.key == SDLK_ESCAPE || e->key.key == SDLK_RETURN)
                app_close_upload_modal(app);
            return;
        }
    } else if (app->model_modal_open) {
        if (e->type == SDL_EVENT_KEY_DOWN) {
            if (e->key.key == SDLK_ESCAPE) app_close_model_modal(app);
            return;
        }
    }
    /* Ignore stray mouse events as the window is mapped. */
    if ((e->type == SDL_EVENT_MOUSE_BUTTON_DOWN || e->type == SDL_EVENT_MOUSE_BUTTON_UP) &&
        cs_now_ms() - app->started_ms < 350) {
        return;
    }
    ui_handle_event(&app->ui, e);
    if (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat) {
        if (e->key.key == SDLK_1 && (e->key.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI))) {
            app_set_tab(app, APP_TAB_RF);
            return;
        }
        if (e->key.key == SDLK_2 && (e->key.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI))) {
            app_set_tab(app, APP_TAB_SYMPTOM);
            return;
        }
        switch (e->key.key) {
            case SDLK_ESCAPE:
                if (app->remove_meter_open) {
                    app_cancel_remove_meter(app);
                } else if (app->modal_open) {
                    SDL_LockMutex(app->mu);
                    app->modal_open = false;
                    SDL_UnlockMutex(app->mu);
                    app_notify(app, false, "Continuing without a phone. Use Reconnect anytime.");
                }
                break;
            case SDLK_RETURN:
                if (app->modal_open) app_request_check(app);
                break;
            case SDLK_R:
                app_request_check(app);
                break;
            case SDLK_C:
                if (!app->demo && !app->image_mode && app->cam.count > 1) {
                    if (camera_switch(&app->cam)) {
                        app_notify(app, false, "Switched to camera: %s", app->cam.name);
                    } else {
                        app_notify(app, true, "Could not switch camera: %s",
                                   app->cam.error);
                    }
                }
                break;
            case SDLK_E:
                app_export_dataset(app);
                break;
            case SDLK_SPACE:
                if (!app->modal_open && !app->remove_meter_open) app_toggle_pause(app);
                break;
            default:
                break;
        }
    }
}
