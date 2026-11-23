#include "symptom_catalog.h"

#include <stdio.h>
#include <string.h>

#include "util.h"

static SymptomDef g_defs[SYMPTOM_CATALOG_MAX];
static int g_count;
static bool g_loaded;

static const char *kFallback[][2] = {
    {"Pain", "severity,onset,duration,pattern,description,triggers,relief,notes"},
    {"Numbness", "severity,onset,duration,pattern,description,triggers"},
    {"Tingling / pins and needles", "severity,onset,duration,pattern,description,triggers"},
    {"Weakness", "severity,onset,duration,pattern,description,triggers"},
    {"Burning", "severity,onset,duration,pattern,description,triggers,relief"},
    {"Pressure", "severity,onset,duration,pattern,description,triggers,relief"},
    {"Swelling", "severity,onset,duration,description,triggers,relief"},
    {"Stiffness", "severity,onset,duration,pattern,description,triggers,relief"},
    {"Tremor", "severity,onset,duration,pattern,description,triggers"},
    {"Headache", "severity,onset,duration,pattern,description,triggers,relief,notes"},
    {"Dizziness / lightheadedness", "severity,onset,duration,pattern,description,triggers"},
    {"Vision change", "onset,duration,pattern,description,triggers,relief,notes"},
    {"Hearing change", "onset,duration,pattern,description,triggers,relief,notes"},
    {"Nausea", "severity,onset,duration,pattern,description,triggers,relief"},
    {"Fatigue", "severity,onset,duration,pattern,description,triggers,relief"},
    {"Anxiety", "severity,onset,duration,pattern,description,triggers,relief,notes"},
    {"Confusion", "severity,onset,duration,pattern,description,triggers,notes"},
};

static unsigned parse_controls(const char *text)
{
    unsigned flags = 0;
    char buf[256];
    cs_strlcpy(buf, text ? text : "", sizeof(buf));
    char *save = NULL;
    for (char *p = strtok_r(buf, ",", &save); p; p = strtok_r(NULL, ",", &save)) {
        while (*p == ' ' || *p == '\t') p++;
        char *end = p + strlen(p);
        while (end > p && (end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';
        if (!strcmp(p, "severity")) flags |= SYM_CTRL_SEVERITY;
        else if (!strcmp(p, "onset")) flags |= SYM_CTRL_ONSET;
        else if (!strcmp(p, "duration")) flags |= SYM_CTRL_DURATION;
        else if (!strcmp(p, "pattern")) flags |= SYM_CTRL_PATTERN;
        else if (!strcmp(p, "description")) flags |= SYM_CTRL_DESCRIPTION;
        else if (!strcmp(p, "triggers")) flags |= SYM_CTRL_TRIGGERS;
        else if (!strcmp(p, "relief")) flags |= SYM_CTRL_RELIEF;
        else if (!strcmp(p, "notes")) flags |= SYM_CTRL_NOTES;
    }
    return flags;
}

static void add_def(const char *name, unsigned controls)
{
    if (g_count >= SYMPTOM_CATALOG_MAX || !name || !*name) return;
    SymptomDef *d = &g_defs[g_count++];
    cs_strlcpy(d->name, name, sizeof(d->name));
    d->controls = controls;
}

static void load_fallback(void)
{
    g_count = 0;
    for (size_t i = 0; i < sizeof(kFallback) / sizeof(kFallback[0]); i++)
        add_def(kFallback[i][0], parse_controls(kFallback[i][1]));
}

bool symptom_catalog_load(const char *asset_dir, char *err, size_t err_cap)
{
    if (g_loaded) return true;
    g_loaded = true;
    char path[CS_PATH_MAX];
    cs_path_join(path, sizeof(path), asset_dir ? asset_dir : "", "symptom-catalog.txt");
    FILE *f = fopen(path, "rb");
    if (!f) {
        load_fallback();
        snprintf(err, err_cap, "Symptom catalog missing at %s; using built-in list", path);
        return false;
    }
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#' || *p == '\n' || *p == '\r') continue;
        char *nl = strpbrk(p, "\r\n");
        if (nl) *nl = '\0';
        char *bar = strchr(p, '|');
        if (!bar) continue;
        *bar = '\0';
        char *name = p;
        char *end = name + strlen(name);
        while (end > name && (end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';
        add_def(name, parse_controls(bar + 1));
    }
    fclose(f);
    if (g_count == 0) load_fallback();
    return true;
}

int symptom_catalog_count(void) { return g_count; }

const SymptomDef *symptom_catalog_at(int index)
{
    return index >= 0 && index < g_count ? &g_defs[index] : NULL;
}

const SymptomDef *symptom_catalog_find(const char *name)
{
    if (!name) return NULL;
    for (int i = 0; i < g_count; i++)
        if (strcmp(g_defs[i].name, name) == 0) return &g_defs[i];
    return NULL;
}
