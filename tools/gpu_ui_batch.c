#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

#if defined(__APPLE__)
static const char *MSL_VERT =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VIn { float2 pos [[attribute(0)]]; float4 color [[attribute(1)]]; };\n"
    "struct VOut { float4 pos [[position]]; float4 color; };\n"
    "vertex VOut main0(VIn in [[stage_in]]) { VOut o; o.pos=float4(in.pos,0,1); o.color=in.color; return o; }\n";
static const char *MSL_FRAG =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VOut { float4 pos [[position]]; float4 color; };\n"
    "fragment float4 main0(VOut in [[stage_in]]) { return in.color; }\n";
#endif

typedef struct { float x, y, r, g, b, a; } V;
#define MAX_VERTS 4096

typedef struct {
    SDL_GPUDevice *gpu;
    SDL_Window *win;
    SDL_GPUGraphicsPipeline *pipeline;
    SDL_GPUBuffer *vbo;
    SDL_GPUTransferBuffer *tbo;
    V verts[MAX_VERTS];
    int count;
} Batch;

static SDL_GPUShader *shader(SDL_GPUDevice *gpu, SDL_GPUShaderStage stage, const char *code)
{
#if defined(__APPLE__)
    SDL_GPUShaderCreateInfo i = {0};
    i.code = (const Uint8 *)code;
    i.code_size = SDL_strlen(code);
    i.entrypoint = "main0";
    i.format = SDL_GPU_SHADERFORMAT_MSL;
    i.stage = stage;
    return SDL_CreateGPUShader(gpu, &i);
#else
    (void)gpu; (void)stage; (void)code;
    return NULL;
#endif
}

static bool batch_init(Batch *b, SDL_GPUDevice *gpu, SDL_Window *win)
{
    memset(b, 0, sizeof(*b));
    b->gpu = gpu;
    b->win = win;
    SDL_GPUShader *vs = shader(gpu, SDL_GPU_SHADERSTAGE_VERTEX, MSL_VERT);
    SDL_GPUShader *fs = shader(gpu, SDL_GPU_SHADERSTAGE_FRAGMENT, MSL_FRAG);
    if (!vs || !fs) return false;
    SDL_GPUVertexBufferDescription vbd = {0};
    vbd.slot = 0;
    vbd.pitch = sizeof(V);
    vbd.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    SDL_GPUVertexAttribute attrs[2] = {
        {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 0},
        {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 2 * sizeof(float)},
    };
    SDL_GPUGraphicsPipelineCreateInfo ci = {0};
    ci.vertex_shader = vs;
    ci.fragment_shader = fs;
    ci.vertex_input_state.vertex_buffer_descriptions = &vbd;
    ci.vertex_input_state.num_vertex_buffers = 1;
    ci.vertex_input_state.vertex_attributes = attrs;
    ci.vertex_input_state.num_vertex_attributes = 2;
    ci.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    ci.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    ci.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    SDL_GPUColorTargetDescription target = {0};
    target.format = SDL_GetGPUSwapchainTextureFormat(gpu, win);
    ci.target_info.color_target_descriptions = &target;
    ci.target_info.num_color_targets = 1;
    b->pipeline = SDL_CreateGPUGraphicsPipeline(gpu, &ci);
    SDL_ReleaseGPUShader(gpu, vs);
    SDL_ReleaseGPUShader(gpu, fs);
    if (!b->pipeline) return false;
    SDL_GPUBufferCreateInfo bci = {0};
    bci.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    bci.size = sizeof(V) * MAX_VERTS;
    b->vbo = SDL_CreateGPUBuffer(gpu, &bci);
    SDL_GPUTransferBufferCreateInfo tci = {0};
    tci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tci.size = sizeof(V) * MAX_VERTS;
    b->tbo = SDL_CreateGPUTransferBuffer(gpu, &tci);
    return b->vbo && b->tbo;
}

static void rect(Batch *b, float x, float y, float w, float h, float r, float g, float bl,
                 float a)
{
    if (b->count + 6 > MAX_VERTS) return;
    float l = x * 2 - 1, rr = (x + w) * 2 - 1;
    float t = 1 - y * 2, bb = 1 - (y + h) * 2;
    V v[6] = {
        {l, t, r, g, bl, a}, {rr, t, r, g, bl, a}, {rr, bb, r, g, bl, a},
        {l, t, r, g, bl, a}, {rr, bb, r, g, bl, a}, {l, bb, r, g, bl, a},
    };
    memcpy(&b->verts[b->count], v, sizeof(v));
    b->count += 6;
}

static bool batch_present(Batch *b)
{
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(b->gpu);
    if (!cmd) return false;
    void *map = SDL_MapGPUTransferBuffer(b->gpu, b->tbo, false);
    memcpy(map, b->verts, sizeof(V) * b->count);
    SDL_UnmapGPUTransferBuffer(b->gpu, b->tbo);
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTransferBufferLocation src = {b->tbo, 0};
    SDL_GPUBufferRegion dst = {b->vbo, 0, sizeof(V) * b->count};
    SDL_UploadToGPUBuffer(copy, &src, &dst, false);
    SDL_EndGPUCopyPass(copy);
    SDL_GPUTexture *swap = NULL;
    if (!SDL_AcquireGPUSwapchainTexture(cmd, b->win, &swap, NULL, NULL)) return false;
    if (swap) {
        SDL_GPUColorTargetInfo ct = {0};
        ct.texture = swap;
        ct.clear_color = (SDL_FColor){0.03f, 0.04f, 0.05f, 1.0f};
        ct.load_op = SDL_GPU_LOADOP_CLEAR;
        ct.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &ct, 1, NULL);
        SDL_BindGPUGraphicsPipeline(pass, b->pipeline);
        SDL_GPUBufferBinding binding = {b->vbo, 0};
        SDL_BindGPUVertexBuffers(pass, 0, &binding, 1);
        SDL_DrawGPUPrimitives(pass, b->count, 1, 0, 0);
        SDL_EndGPURenderPass(pass);
    }
    SDL_SubmitGPUCommandBuffer(cmd);
    return swap != NULL;
}

int main(void)
{
    SDL_Init(SDL_INIT_VIDEO);
    SDL_Window *win = SDL_CreateWindow("CellScope GPU UI batch", 640, 400,
                                       SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE);
    SDL_GPUDevice *gpu = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_MSL, true, NULL);
    if (!win || !gpu || !SDL_ClaimWindowForGPUDevice(gpu, win)) {
        fprintf(stderr, "GPU init failed: %s\n", SDL_GetError());
        return 1;
    }
    Batch b;
    if (!batch_init(&b, gpu, win)) return 1;
    /* A small mock of the app frame: tab bar, left panel, right panel, button. */
    rect(&b, 0, 0, 1.0f, 0.08f, 0.03f, 0.05f, 0.07f, 1);
    rect(&b, 0.02f, 0.12f, 0.46f, 0.80f, 0.06f, 0.09f, 0.12f, 1);
    rect(&b, 0.52f, 0.12f, 0.46f, 0.80f, 0.05f, 0.07f, 0.09f, 1);
    rect(&b, 0.04f, 0.04f, 0.10f, 0.035f, 0.55f, 0.95f, 0.75f, 1);
    rect(&b, 0.04f, 0.16f, 0.09f, 0.035f, 0.14f, 0.22f, 0.28f, 1);
    if (!batch_present(&b)) return 1;
    SDL_Delay(1200);
    SDL_ReleaseGPUTransferBuffer(gpu, b.tbo);
    SDL_ReleaseGPUBuffer(gpu, b.vbo);
    SDL_ReleaseGPUGraphicsPipeline(gpu, b.pipeline);
    SDL_ReleaseWindowFromGPUDevice(gpu, win);
    SDL_DestroyGPUDevice(gpu);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
