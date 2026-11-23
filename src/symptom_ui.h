#ifndef CELLSCOPE_SYMPTOM_UI_H
#define CELLSCOPE_SYMPTOM_UI_H

#include "app.h"
#include "anatomy_3d.h"

void symptom_ui_draw(App *app);
bool symptom_ui_handle_event(App *app, const SDL_Event *e);
void symptom_add_muscle_card(App *app, const AnatomyPick *pick);
void symptom_ui_cancel_edit(App *app);

#endif
