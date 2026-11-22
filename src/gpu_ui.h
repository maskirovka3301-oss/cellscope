#ifndef CELLSCOPE_GPU_UI_H
#define CELLSCOPE_GPU_UI_H

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stddef.h>

/* GPU UI backend. mac-first MSL implementation; other backends are added
 * through SDL_shadercross before this branch is merged. */
typedef struct {
    float x, y, w, h;
    Uint8 r, g, b, a;
} GpuUiRect;

typedef struct { float x, y, r, g, b, a; } GpuUiVertex;

typedef struct GpuUi {
    SDL_GPUDevice *gpu;
    SDL_Window *win;
    SDL_GPUGraphicsPipeline *pipeline;
    SDL_GPUBuffer *vbo;
    SDL_GPUTransferBuffer *tbo;
    SDL_GPUCommandBuffer *cmd;
    SDL_GPUTexture *swap;
    GpuUiVertex *verts;
    int count;
    int win_w, win_h;
} GpuUi;

bool gpu_ui_init(GpuUi *ui, SDL_GPUDevice *gpu, SDL_Window *win, char *err, size_t err_cap);
void gpu_ui_shutdown(GpuUi *ui);
bool gpu_ui_begin(GpuUi *ui, int win_w, int win_h, GpuUiRect clear);
void gpu_ui_fill_rect(GpuUi *ui, GpuUiRect r);
bool gpu_ui_end(GpuUi *ui);

#endif
