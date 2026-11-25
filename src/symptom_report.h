#ifndef CELLSCOPE_SYMPTOM_REPORT_H
#define CELLSCOPE_SYMPTOM_REPORT_H

#include <stdbool.h>
#include "app.h"

void symptom_report_mark_dirty(App *app);
void symptom_report_reset(App *app);
void symptom_report_reset_locked(App *app);
void symptom_report_flush(App *app);
bool symptom_report_write_locked(App *app);

#endif
