#include <SDL3/SDL.h>
#include <stdio.h>

int main(void)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window *win = SDL_CreateWindow("CellScope SDL_GPU smoke", 640, 400,
                                       SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE);
    if (!win) {
        fprintf(stderr, "window: %s\n", SDL_GetError());
        return 1;
    }
#if defined(__APPLE__)
    SDL_GPUShaderFormat formats = SDL_GPU_SHADERFORMAT_MSL;
#elif defined(_WIN32)
    SDL_GPUShaderFormat formats = SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_SPIRV;
#else
    SDL_GPUShaderFormat formats = SDL_GPU_SHADERFORMAT_SPIRV;
#endif
    SDL_GPUDevice *gpu = SDL_CreateGPUDevice(formats, true, NULL);
    if (!gpu) {
        fprintf(stderr, "SDL_CreateGPUDevice: %s\n", SDL_GetError());
        return 1;
    }
    if (!SDL_ClaimWindowForGPUDevice(gpu, win)) {
        fprintf(stderr, "SDL_ClaimWindowForGPUDevice: %s\n", SDL_GetError());
        return 1;
    }
    printf("GPU driver: %s\n", SDL_GetGPUDeviceDriver(gpu));
    printf("Shader formats: 0x%x\n", (unsigned)SDL_GetGPUShaderFormats(gpu));

    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(gpu);
    if (!cmd) {
        fprintf(stderr, "command buffer: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GPUTexture *swap = NULL;
    Uint32 sw = 0, sh = 0;
    if (!SDL_AcquireGPUSwapchainTexture(cmd, win, &swap, &sw, &sh)) {
        fprintf(stderr, "swapchain: %s\n", SDL_GetError());
        return 1;
    }
    if (swap) {
        SDL_GPUColorTargetInfo color = {0};
        color.texture = swap;
        color.clear_color = (SDL_FColor){0.03f, 0.04f, 0.05f, 1.0f};
        color.load_op = SDL_GPU_LOADOP_CLEAR;
        color.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &color, 1, NULL);
        SDL_EndGPURenderPass(pass);
    }
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_Delay(500);
    SDL_ReleaseWindowFromGPUDevice(gpu, win);
    SDL_DestroyGPUDevice(gpu);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return swap ? 0 : 1;
}
