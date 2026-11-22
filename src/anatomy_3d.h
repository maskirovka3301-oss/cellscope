#ifndef CELLSCOPE_ANATOMY_3D_H
#define CELLSCOPE_ANATOMY_3D_H

#include <SDL3/SDL.h>
#include <stdbool.h>

#include "gpu_renderer.h"

typedef struct Anatomy3D Anatomy3D;
typedef struct App App;

Anatomy3D *anatomy_3d_create(App *app);
void anatomy_3d_destroy(Anatomy3D *a);
bool anatomy_3d_available(const Anatomy3D *a);
GpuTexture *anatomy_3d_texture(const Anatomy3D *a);
int anatomy_3d_selection(const Anatomy3D *a);
void anatomy_3d_frame(App *app);

#endif
