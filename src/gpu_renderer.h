#ifndef CELLSCOPE_GPU_RENDERER_H
#define CELLSCOPE_GPU_RENDERER_H

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct GpuRenderer GpuRenderer;
typedef struct GpuTexture GpuTexture;

typedef struct {
    float x, y, w, h;
} GpuRect;

typedef struct {
    Uint8 r, g, b, a;
} GpuColor;

#define GPU_COLOR(r, g, b, a) ((GpuColor){(Uint8)(r), (Uint8)(g), (Uint8)(b), (Uint8)(a)})

GpuRenderer *gpu_renderer_create(SDL_Window *win, char *err, size_t err_cap);
void gpu_renderer_destroy(GpuRenderer *r);
bool gpu_renderer_begin(GpuRenderer *r, int logical_w, int logical_h);
bool gpu_renderer_end(GpuRenderer *r);
void gpu_renderer_push_clip(GpuRenderer *r, GpuRect clip);
void gpu_renderer_pop_clip(GpuRenderer *r);
void gpu_renderer_fill(GpuRenderer *r, GpuRect dst, GpuColor c);
void gpu_renderer_line(GpuRenderer *r, float x0, float y0, float x1, float y1, float thickness,
                       GpuColor c);
void gpu_renderer_texture(GpuRenderer *r, GpuTexture *tex, GpuRect dst, float u0, float v0,
                          float u1, float v1, GpuColor c);
void gpu_renderer_texture_rect(GpuRenderer *r, GpuTexture *tex, GpuRect dst, GpuColor c);
bool gpu_renderer_screenshot(GpuRenderer *r, const char *path);

GpuTexture *gpu_texture_create(GpuRenderer *r, int w, int h);
void gpu_texture_update(GpuTexture *tex, const void *rgba, int pitch);
void gpu_texture_destroy(GpuRenderer *r, GpuTexture *tex);
int gpu_texture_width(const GpuTexture *tex);
int gpu_texture_height(const GpuTexture *tex);

/* One dynamic atlas for text/shape tiles. */
int gpu_atlas_size(GpuRenderer *r);
unsigned char *gpu_atlas_pixels(GpuRenderer *r);
bool gpu_atlas_alloc(GpuRenderer *r, int w, int h, int *x, int *y);
void gpu_atlas_dirty(GpuRenderer *r);
GpuTexture *gpu_atlas_texture(GpuRenderer *r);

#endif
