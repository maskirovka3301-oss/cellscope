#include "anatomy_assets.h"

#include <curl/curl.h>
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

#ifdef _WIN32
#define strtok_r strtok_s
#define strcasecmp _stricmp
#endif

#define ANATOMY_MAX_FILES 8

typedef struct {
    char *data;
    size_t len;
} MemBuf;

typedef struct {
    char name[192];
    char sha512[160];
    long long size;
} AssetFile;

typedef struct {
    char version[64];
    AssetFile files[ANATOMY_MAX_FILES];
    int count;
} AnatomyManifest;

typedef struct {
    App *app;
    long long base;
    long long total;
} ProgressCtx;

static size_t mem_write(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    MemBuf *b = (MemBuf *)userdata;
    size_t n = size * nmemb;
    char *next = realloc(b->data, b->len + n + 1);
    if (!next) return 0;
    b->data = next;
    memcpy(b->data + b->len, ptr, n);
    b->len += n;
    b->data[b->len] = '\0';
    return n;
}

static size_t file_write(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    return fwrite(ptr, size, nmemb, (FILE *)userdata) * size;
}

static int progress_cb(void *userdata, curl_off_t dltotal, curl_off_t dlnow,
                       curl_off_t ultotal, curl_off_t ulnow)
{
    (void)ultotal;
    (void)ulnow;
    ProgressCtx *p = (ProgressCtx *)userdata;
    if (SDL_GetAtomicInt(&p->app->quit)) return 1;
    if (p->total <= 0) return 0;
    long long done = p->base + (dltotal > 0 && dlnow > dltotal ? dltotal : dlnow);
    int pct = (int)((done * 100) / p->total);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    SDL_LockMutex(p->app->mu);
    p->app->anatomy_percent = pct;
    SDL_UnlockMutex(p->app->mu);
    return 0;
}

static bool url_join(char *out, size_t cap, const char *base, const char *name)
{
    if (!base || !*base || !name || !*name) return false;
    size_t n = strlen(base);
    snprintf(out, cap, "%s%s%s", base, base[n - 1] == '/' ? "" : "/", name);
    return true;
}

static bool fetch_manifest(const char *base, AnatomyManifest *m, char *err, size_t cap)
{
    char url[1024];
    url_join(url, sizeof(url), base, "manifest.txt");
    MemBuf b = {0};
    CURL *c = curl_easy_init();
    if (!c) {
        snprintf(err, cap, "Could not initialize HTTP client");
        return false;
    }
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, mem_write);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, 15000L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, 30000L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "CellScope/anatomy");
    CURLcode rc = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(c);
    if (rc != CURLE_OK || code != 200 || !b.data) {
        snprintf(err, cap, "Could not fetch anatomy manifest (%ld)", code);
        free(b.data);
        return false;
    }

    memset(m, 0, sizeof(*m));
    char *save = NULL;
    for (char *line = strtok_r(b.data, "\r\n", &save); line;
         line = strtok_r(NULL, "\r\n", &save)) {
        if (strncmp(line, "version=", 8) == 0) {
            cs_strlcpy(m->version, line + 8, sizeof(m->version));
        } else if (strncmp(line, "file=", 5) == 0 && m->count < ANATOMY_MAX_FILES) {
            char name[192], hash[160];
            long long size = 0;
            if (sscanf(line + 5, "%191s sha512=%159s size=%lld", name, hash, &size) == 3) {
                if (strchr(name, '/') || strchr(name, '\\') || strstr(name, "..")) continue;
                AssetFile *f = &m->files[m->count++];
                cs_strlcpy(f->name, name, sizeof(f->name));
                cs_strlcpy(f->sha512, hash, sizeof(f->sha512));
                f->size = size;
            }
        }
    }
    free(b.data);
    if (!m->version[0]) cs_strlcpy(m->version, "1", sizeof(m->version));
    if (!m->count) {
        snprintf(err, cap, "Anatomy manifest has no files");
        return false;
    }
    return true;
}

static bool sha512_file(const char *path, char out[129])
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    crypto_hash_sha512_state st;
    crypto_hash_sha512_init(&st);
    unsigned char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) crypto_hash_sha512_update(&st, buf, n);
    bool ok = !ferror(f);
    fclose(f);
    if (!ok) return false;
    unsigned char digest[crypto_hash_sha512_BYTES];
    crypto_hash_sha512_final(&st, digest);
    sodium_bin2hex(out, 129, digest, sizeof(digest));
    return true;
}

static bool verify_file(const char *path, const AssetFile *f)
{
    char got[129];
    if (!sha512_file(path, got)) return false;
    return strcasecmp(got, f->sha512) == 0;
}

static bool download_file(App *app, const char *base, const char *dir, const AssetFile *f,
                          long long base_progress, long long total, char *err, size_t cap)
{
    char url[1024], path[CS_PATH_MAX], part[CS_PATH_MAX + 8];
    url_join(url, sizeof(url), base, f->name);
    cs_path_join(path, sizeof(path), dir, f->name);
    snprintf(part, sizeof(part), "%s.part", path);
    FILE *out = fopen(part, "wb");
    if (!out) {
        snprintf(err, cap, "Could not write %s", part);
        return false;
    }
    ProgressCtx pc = {app, base_progress, total};
    CURL *c = curl_easy_init();
    if (!c) {
        fclose(out);
        snprintf(err, cap, "Could not initialize HTTP client");
        return false;
    }
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, file_write);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, out);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, progress_cb);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, &pc);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, 15000L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, 3600000L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "CellScope/anatomy");
    CURLcode rc = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(c);
    bool ok = fclose(out) == 0;
    if (rc != CURLE_OK || code != 200 || !ok) {
        remove(part);
        snprintf(err, cap, "Could not download %s (%ld)", f->name, code);
        return false;
    }
    if (!verify_file(part, f)) {
        remove(part);
        snprintf(err, cap, "Checksum failed for %s", f->name);
        return false;
    }
    remove(path);
    if (rename(part, path) != 0) {
        remove(part);
        snprintf(err, cap, "Could not install %s", f->name);
        return false;
    }
    return true;
}

static void set_status(App *app, bool downloading, bool ready, bool failed,
                       int percent, const char *text)
{
    SDL_LockMutex(app->mu);
    app->anatomy_downloading = downloading;
    app->anatomy_ready = ready;
    app->anatomy_failed = failed;
    app->anatomy_percent = percent;
    cs_strlcpy(app->anatomy_status, text ? text : "", sizeof(app->anatomy_status));
    SDL_UnlockMutex(app->mu);
}

static int anatomy_thread(void *arg)
{
    App *app = (App *)arg;
    char base[1024] = {0};
    const char *env = getenv("CELLSCOPE_ANATOMY_URL");
    if (env) cs_strlcpy(base, env, sizeof(base));
    char err[512] = {0};
    AnatomyManifest manifest;
    if (!fetch_manifest(base, &manifest, err, sizeof(err))) {
        set_status(app, false, false, true, 0, err);
        return 1;
    }

    char data_root[CS_PATH_MAX], dir[CS_PATH_MAX];
    if (!cs_app_data_dir(data_root, sizeof(data_root))) {
        set_status(app, false, false, true, 0, "Could not resolve the CellScope data folder");
        return 1;
    }
    char rel[256];
    snprintf(rel, sizeof(rel), "assets/anatomy/%s", manifest.version);
    cs_path_join(dir, sizeof(dir), data_root, rel);
    if (!cs_mkdirs(dir)) {
        set_status(app, false, false, true, 0, "Could not create the anatomy cache folder");
        return 1;
    }

    long long total = 0;
    for (int i = 0; i < manifest.count; i++) total += manifest.files[i].size;
    long long done = 0;
    for (int i = 0; i < manifest.count; i++) {
        AssetFile *f = &manifest.files[i];
        char path[CS_PATH_MAX];
        cs_path_join(path, sizeof(path), dir, f->name);
        SDL_LockMutex(app->mu);
        cs_strlcpy(app->anatomy_status, f->name, sizeof(app->anatomy_status));
        SDL_UnlockMutex(app->mu);
        if (!verify_file(path, f)) {
            if (!download_file(app, base, dir, f, done, total, err, sizeof(err))) {
                set_status(app, false, false, true, 0, err);
                return 1;
            }
        }
        done += f->size;
        SDL_LockMutex(app->mu);
        app->anatomy_percent = total > 0 ? (int)((done * 100) / total) : 0;
        SDL_UnlockMutex(app->mu);
    }
    char ready[256];
    snprintf(ready, sizeof(ready), "Anatomy model ready (%s)", manifest.version);
    set_status(app, false, true, false, 100, ready);
    return 0;
}

void anatomy_assets_start(App *app)
{
    SDL_LockMutex(app->mu);
    if (app->anatomy_ready || app->anatomy_downloading) {
        SDL_UnlockMutex(app->mu);
        return;
    }
    const char *env = getenv("CELLSCOPE_ANATOMY_URL");
    if (!env || !*env) {
        cs_strlcpy(app->anatomy_status, "Anatomy model source is not configured (2D fallback)",
                   sizeof(app->anatomy_status));
        app->anatomy_failed = false;
        SDL_UnlockMutex(app->mu);
        return;
    }
    if (app->anatomy_thread) {
        SDL_WaitThread(app->anatomy_thread, NULL);
        app->anatomy_thread = NULL;
    }
    app->anatomy_failed = false;
    app->anatomy_downloading = true;
    app->anatomy_percent = 0;
    cs_strlcpy(app->anatomy_status, "Fetching anatomy manifest", sizeof(app->anatomy_status));
    SDL_UnlockMutex(app->mu);
    app->anatomy_thread = SDL_CreateThread(anatomy_thread, "anatomy-assets", app);
    if (!app->anatomy_thread) set_status(app, false, false, true, 0, "Could not start anatomy download");
}

void anatomy_assets_shutdown(App *app)
{
    if (!app->anatomy_thread) return;
    SDL_WaitThread(app->anatomy_thread, NULL);
    app->anatomy_thread = NULL;
}
