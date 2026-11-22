#ifndef CELLSCOPE_CAMERA_H
#define CELLSCOPE_CAMERA_H

#include <SDL3/SDL.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gpu_renderer.h"

typedef struct {
    GpuRenderer *r;
    SDL_Camera *cam;
    GpuTexture *tex;
    SDL_CameraID ids[8];
    char names[8][128];
    int count;
    int index;
    int width, height;
    bool active;
    bool permission_pending;
    bool permission_denied;
    char name[128];
    char error[256];
    int64_t last_frame_ms;
    uint64_t frames;

    uint8_t *rgb;
    int rgb_w, rgb_h;
    int64_t rgb_ms;
    int64_t last_capture_ms;
} Camera;

bool camera_init(Camera *cam, GpuRenderer *r, int index, char *err, size_t err_cap);
void camera_close(Camera *cam);
void camera_update(Camera *cam);
bool camera_switch(Camera *cam);

#endif
