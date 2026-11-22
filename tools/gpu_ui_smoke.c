#include <SDL3/SDL.h>
#include <stdio.h>

#if defined(__APPLE__)
static const char *MSL_VERT =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VIn { float2 pos [[attribute(0)]]; float4 color [[attribute(1)]]; };\n"
    "struct VOut { float4 pos [[position]]; float4 color; };\n"
    "vertex VOut main0(VIn in [[stage_in]]) {\n"
    "  VOut o; o.pos = float4(in.pos, 0.0, 1.0); o.color = in.color; return o;\n"
    "}\n";
static const char *MSL_FRAG =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VOut { float4 pos [[position]]; float4 color; };\n"
    "fragment float4 main0(VOut in [[stage_in]]) { return in.color; }\n";
#endif

typedef struct { float x, y, r, g, b, a; } Vertex;

static SDL_GPUShader *make_shader(SDL_GPUDevice *gpu, SDL_GPUShaderStage stage,
                                  const char *code)
{
#if defined(__APPLE__)
    SDL_GPUShaderCreateInfo info = {0};
    info.code = (const Uint8 *)code;
    info.code_size = SDL_strlen(code);
    info.entrypoint = "main0";
    info.format = SDL_GPU_SHADERFORMAT_MSL;
    info.stage = stage;
    return SDL_CreateGPUShader(gpu, &info);
#else
    (void)gpu;
    (void)stage;
    (void)code;
    return NULL;
#endif
}

int main(void)
{
    SDL_Init(SDL_INIT_VIDEO);
    SDL_Window *win = SDL_CreateWindow("CellScope GPU UI smoke", 640, 400,
                                       SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE);
#if defined(__APPLE__)
    SDL_GPUShaderFormat formats = SDL_GPU_SHADERFORMAT_MSL;
#elif defined(_WIN32)
    SDL_GPUShaderFormat formats = SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_SPIRV;
#else
    SDL_GPUShaderFormat formats = SDL_GPU_SHADERFORMAT_SPIRV;
#endif
    SDL_GPUDevice *gpu = SDL_CreateGPUDevice(formats, true, NULL);
    if (!win || !gpu || !SDL_ClaimWindowForGPUDevice(gpu, win)) {
        fprintf(stderr, "GPU init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GPUShader *vs = make_shader(gpu, SDL_GPU_SHADERSTAGE_VERTEX, MSL_VERT);
    SDL_GPUShader *fs = make_shader(gpu, SDL_GPU_SHADERSTAGE_FRAGMENT, MSL_FRAG);
    if (!vs || !fs) {
        fprintf(stderr, "shader creation failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_GPUVertexBufferDescription vb = {0};
    vb.slot = 0;
    vb.pitch = sizeof(Vertex);
    vb.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    SDL_GPUVertexAttribute attrs[2] = {
        {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 0},
        {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 2 * sizeof(float)},
    };
    SDL_GPUGraphicsPipelineCreateInfo ci = {0};
    ci.vertex_shader = vs;
    ci.fragment_shader = fs;
    ci.vertex_input_state.vertex_buffer_descriptions = &vb;
    ci.vertex_input_state.num_vertex_buffers = 1;
    ci.vertex_input_state.vertex_attributes = attrs;
    ci.vertex_input_state.num_vertex_attributes = 2;
    ci.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    ci.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    ci.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    SDL_GPUColorTargetDescription color = {0};
    color.format = SDL_GetGPUSwapchainTextureFormat(gpu, win);
    ci.target_info.color_target_descriptions = &color;
    ci.target_info.num_color_targets = 1;
    SDL_GPUGraphicsPipeline *pipeline = SDL_CreateGPUGraphicsPipeline(gpu, &ci);
    if (!pipeline) {
        fprintf(stderr, "pipeline failed: %s\n", SDL_GetError());
        return 1;
    }

    Vertex verts[6] = {
        {-0.8f, -0.8f, 0.0f, 0.90f, 0.50f, 1.0f},
        { 0.8f, -0.8f, 0.0f, 0.50f, 0.90f, 1.0f},
        { 0.8f,  0.8f, 0.0f, 0.90f, 0.90f, 0.20f},
        {-0.8f, -0.8f, 0.0f, 0.90f, 0.50f, 1.0f},
        { 0.8f,  0.8f, 0.0f, 0.90f, 0.90f, 0.20f},
        {-0.8f,  0.8f, 0.0f, 0.50f, 0.50f, 1.0f},
    };
    SDL_GPUBufferCreateInfo bci = {0};
    bci.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    bci.size = sizeof(verts);
    SDL_GPUBuffer *vbo = SDL_CreateGPUBuffer(gpu, &bci);
    SDL_GPUTransferBufferCreateInfo tci = {0};
    tci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tci.size = sizeof(verts);
    SDL_GPUTransferBuffer *tbo = SDL_CreateGPUTransferBuffer(gpu, &tci);
    void *map = SDL_MapGPUTransferBuffer(gpu, tbo, false);
    memcpy(map, verts, sizeof(verts));
    SDL_UnmapGPUTransferBuffer(gpu, tbo);

    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(gpu);
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTransferBufferLocation src = {tbo, 0};
    SDL_GPUBufferRegion dst = {vbo, 0, sizeof(verts)};
    SDL_UploadToGPUBuffer(copy, &src, &dst, false);
    SDL_EndGPUCopyPass(copy);

    SDL_GPUTexture *swap = NULL;
    Uint32 sw = 0, sh = 0;
    if (!SDL_AcquireGPUSwapchainTexture(cmd, win, &swap, &sw, &sh)) {
        fprintf(stderr, "swapchain: %s\n", SDL_GetError());
        return 1;
    }
    if (swap) {
        SDL_GPUColorTargetInfo ct = {0};
        ct.texture = swap;
        ct.clear_color = (SDL_FColor){0.03f, 0.04f, 0.05f, 1.0f};
        ct.load_op = SDL_GPU_LOADOP_CLEAR;
        ct.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &ct, 1, NULL);
        SDL_BindGPUGraphicsPipeline(pass, pipeline);
        SDL_GPUBufferBinding binding = {vbo, 0};
        SDL_BindGPUVertexBuffers(pass, 0, &binding, 1);
        SDL_DrawGPUPrimitives(pass, 6, 1, 0, 0);
        SDL_EndGPURenderPass(pass);
    }
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_Delay(1200);
    SDL_ReleaseGPUTransferBuffer(gpu, tbo);
    SDL_ReleaseGPUBuffer(gpu, vbo);
    SDL_ReleaseGPUGraphicsPipeline(gpu, pipeline);
    SDL_ReleaseGPUShader(gpu, vs);
    SDL_ReleaseGPUShader(gpu, fs);
    SDL_ReleaseWindowFromGPUDevice(gpu, win);
    SDL_DestroyGPUDevice(gpu);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return swap ? 0 : 1;
}
