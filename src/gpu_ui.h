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

typedef struct { float x, y, u, v, r, g, b, a; } GpuUiVertex;
#define GPU_UI_CLIP_STACK 8
#define GPU_UI_MAX_VERTS 8192

typedef struct GpuUi {
    SDL_GPUDevice *gpu;
    SDL_Window *win;
    SDL_GPUGraphicsPipeline *pipeline;
    SDL_GPUBuffer *vbo;
    SDL_GPUTransferBuffer *tbo;
    SDL_GPUCommandBuffer *cmd;
    SDL_GPUTexture *swap;
    SDL_GPUTexture *white;
    SDL_GPUSampler *sampler;
    GpuUiVertex *verts;
    int *clip_ids;
    int count;
    int win_w, win_h;
    GpuUiRect clips[GPU_UI_CLIP_STACK];
    int clip_depth;
} GpuUi;

bool gpu_ui_init(GpuUi *ui, SDL_GPUDevice *gpu, SDL_Window *win, char *err, size_t err_cap);
void gpu_ui_shutdown(GpuUi *ui);
bool gpu_ui_begin(GpuUi *ui, int win_w, int win_h, GpuUiRect clear);
void gpu_ui_fill_rect(GpuUi *ui, GpuUiRect r);
void gpu_ui_push_clip(GpuUi *ui, GpuUiRect r);
void gpu_ui_pop_clip(GpuUi *ui);
bool gpu_ui_end(GpuUi *ui);

#endif
