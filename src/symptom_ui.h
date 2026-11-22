#ifndef CELLSCOPE_SYMPTOM_UI_H
#define CELLSCOPE_SYMPTOM_UI_H

#include "app.h"
#include "anatomy_3d.h"

void symptom_ui_draw(App *app);
void symptom_add_muscle_card(App *app, const AnatomyPick *pick);

#endif
