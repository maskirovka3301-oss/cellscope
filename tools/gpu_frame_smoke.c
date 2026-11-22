#include <SDL3/SDL.h>
#include <stdio.h>
#include "gpu_ui.h"

int main(void)
{
    SDL_Init(SDL_INIT_VIDEO);
    SDL_Window *win = SDL_CreateWindow("CellScope GPU frame", 640, 400,
                                       SDL_WINDOW_HIGH_PIXEL_DENSITY);
    SDL_GPUDevice *gpu = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_MSL, true, NULL);
    if (!win || !gpu || !SDL_ClaimWindowForGPUDevice(gpu, win)) return 1;
    GpuUi ui;
    char err[256];
    if (!gpu_ui_init(&ui, gpu, win, err, sizeof(err))) {
        fprintf(stderr, "gpu_ui_init: %s\n", err);
        return 1;
    }
    if (!gpu_ui_begin(&ui, 640, 400, (GpuUiRect){0})) return 1;
    gpu_ui_fill_rect(&ui, (GpuUiRect){0, 0, 640, 32, 8, 12, 17, 255});
    gpu_ui_fill_rect(&ui, (GpuUiRect){20, 60, 280, 300, 16, 23, 31, 255});
    gpu_ui_fill_rect(&ui, (GpuUiRect){340, 60, 280, 300, 16, 23, 31, 255});
    gpu_ui_fill_rect(&ui, (GpuUiRect){40, 80, 90, 28, 69, 224, 160, 255});
    gpu_ui_push_clip(&ui, (GpuUiRect){20, 60, 120, 120});
    gpu_ui_fill_rect(&ui, (GpuUiRect){0, 0, 320, 300, 90, 60, 40, 255});
    gpu_ui_pop_clip(&ui);
    if (!gpu_ui_end(&ui)) return 1;
    SDL_Delay(800);
    gpu_ui_shutdown(&ui);
    SDL_ReleaseWindowFromGPUDevice(gpu, win);
    SDL_DestroyGPUDevice(gpu);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
