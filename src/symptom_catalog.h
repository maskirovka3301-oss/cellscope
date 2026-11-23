#ifndef CELLSCOPE_SYMPTOM_CATALOG_H
#define CELLSCOPE_SYMPTOM_CATALOG_H

#include <stdbool.h>
#include <stddef.h>

#define SYMPTOM_CATALOG_MAX 64
#define SYMPTOM_NAME_MAX 64

enum {
    SYM_CTRL_SEVERITY = 1u << 0,
    SYM_CTRL_ONSET = 1u << 1,
    SYM_CTRL_DURATION = 1u << 2,
    SYM_CTRL_PATTERN = 1u << 3,
    SYM_CTRL_DESCRIPTION = 1u << 4,
    SYM_CTRL_TRIGGERS = 1u << 5,
    SYM_CTRL_RELIEF = 1u << 6,
    SYM_CTRL_NOTES = 1u << 7,
    SYM_CTRL_ALL = (1u << 8) - 1,
};

typedef struct {
    char name[SYMPTOM_NAME_MAX];
    unsigned controls;
} SymptomDef;

bool symptom_catalog_load(const char *asset_dir, char *err, size_t err_cap);
int symptom_catalog_count(void);
const SymptomDef *symptom_catalog_at(int index);
const SymptomDef *symptom_catalog_find(const char *name);

#endif
