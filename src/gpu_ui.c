#include "gpu_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__)
static const char *MSL_VERT =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "struct I{float2 p [[attribute(0)]];float4 c [[attribute(1)]];};\n"
    "struct O{float4 p [[position]];float4 c;};\n"
    "vertex O main0(I i [[stage_in]]){O o;o.p=float4(i.p,0,1);o.c=i.c;return o;}\n";
static const char *MSL_FRAG =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "struct O{float4 p [[position]];float4 c;};\n"
    "fragment float4 main0(O i [[stage_in]]){return i.c;}\n";
#endif

#define MAX_VERTS 8192

typedef GpuUiVertex V;

static SDL_GPUShader *mk_shader(SDL_GPUDevice *gpu, SDL_GPUShaderStage stage, const char *code)
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

bool gpu_ui_init(GpuUi *ui, SDL_GPUDevice *gpu, SDL_Window *win, char *err, size_t err_cap)
{
    memset(ui, 0, sizeof(*ui));
    ui->gpu = gpu;
    ui->win = win;
    ui->verts = calloc(MAX_VERTS, sizeof(V));
    if (!ui->verts) {
        snprintf(err, err_cap, "Out of memory");
        return false;
    }
    SDL_GPUShader *vs = mk_shader(gpu, SDL_GPU_SHADERSTAGE_VERTEX, MSL_VERT);
    SDL_GPUShader *fs = mk_shader(gpu, SDL_GPU_SHADERSTAGE_FRAGMENT, MSL_FRAG);
    if (!vs || !fs) {
        snprintf(err, err_cap, "Could not create GPU UI shader: %s", SDL_GetError());
        return false;
    }
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
    ui->pipeline = SDL_CreateGPUGraphicsPipeline(gpu, &ci);
    SDL_ReleaseGPUShader(gpu, vs);
    SDL_ReleaseGPUShader(gpu, fs);
    if (!ui->pipeline) {
        snprintf(err, err_cap, "Could not create GPU UI pipeline: %s", SDL_GetError());
        return false;
    }
    SDL_GPUBufferCreateInfo bci = {0};
    bci.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    bci.size = sizeof(V) * MAX_VERTS;
    ui->vbo = SDL_CreateGPUBuffer(gpu, &bci);
    SDL_GPUTransferBufferCreateInfo tci = {0};
    tci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tci.size = sizeof(V) * MAX_VERTS;
    ui->tbo = SDL_CreateGPUTransferBuffer(gpu, &tci);
    if (!ui->vbo || !ui->tbo) {
        snprintf(err, err_cap, "Could not create GPU UI buffer");
        return false;
    }
    return true;
}

void gpu_ui_shutdown(GpuUi *ui)
{
    if (!ui) return;
    if (ui->gpu) {
        if (ui->pipeline) SDL_ReleaseGPUGraphicsPipeline(ui->gpu, ui->pipeline);
        if (ui->vbo) SDL_ReleaseGPUBuffer(ui->gpu, ui->vbo);
        if (ui->tbo) SDL_ReleaseGPUTransferBuffer(ui->gpu, ui->tbo);
    }
    free(ui->verts);
    memset(ui, 0, sizeof(*ui));
}

bool gpu_ui_begin(GpuUi *ui, int win_w, int win_h, GpuUiRect clear)
{
    (void)clear;
    ui->win_w = win_w;
    ui->win_h = win_h;
    ui->count = 0;
    ui->cmd = SDL_AcquireGPUCommandBuffer(ui->gpu);
    if (!ui->cmd) return false;
    return SDL_AcquireGPUSwapchainTexture(ui->cmd, ui->win, &ui->swap, NULL, NULL);
}

void gpu_ui_fill_rect(GpuUi *ui, GpuUiRect r)
{
    if (!ui || ui->count + 6 > MAX_VERTS) return;
    float l = (r.x / (float)ui->win_w) * 2.0f - 1.0f;
    float rt = ((r.x + r.w) / (float)ui->win_w) * 2.0f - 1.0f;
    float t = 1.0f - (r.y / (float)ui->win_h) * 2.0f;
    float b = 1.0f - ((r.y + r.h) / (float)ui->win_h) * 2.0f;
    float cr = r.r / 255.0f, cg = r.g / 255.0f, cb = r.b / 255.0f, ca = r.a / 255.0f;
    V q[6] = {
        {l, t, cr, cg, cb, ca}, {rt, t, cr, cg, cb, ca}, {rt, b, cr, cg, cb, ca},
        {l, t, cr, cg, cb, ca}, {rt, b, cr, cg, cb, ca}, {l, b, cr, cg, cb, ca},
    };
    memcpy(&ui->verts[ui->count], q, sizeof(q));
    ui->count += 6;
}

bool gpu_ui_end(GpuUi *ui)
{
    if (!ui->cmd || !ui->swap || ui->count == 0) return false;
    void *map = SDL_MapGPUTransferBuffer(ui->gpu, ui->tbo, false);
    memcpy(map, ui->verts, sizeof(V) * ui->count);
    SDL_UnmapGPUTransferBuffer(ui->gpu, ui->tbo);
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(ui->cmd);
    SDL_GPUTransferBufferLocation src = {ui->tbo, 0};
    SDL_GPUBufferRegion dst = {ui->vbo, 0, (Uint32)(sizeof(V) * ui->count)};
    SDL_UploadToGPUBuffer(copy, &src, &dst, false);
    SDL_EndGPUCopyPass(copy);
    SDL_GPUColorTargetInfo ct = {0};
    ct.texture = ui->swap;
    ct.clear_color = (SDL_FColor){0.03f, 0.04f, 0.05f, 1.0f};
    ct.load_op = SDL_GPU_LOADOP_CLEAR;
    ct.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(ui->cmd, &ct, 1, NULL);
    SDL_BindGPUGraphicsPipeline(pass, ui->pipeline);
    SDL_GPUBufferBinding binding = {ui->vbo, 0};
    SDL_BindGPUVertexBuffers(pass, 0, &binding, 1);
    SDL_DrawGPUPrimitives(pass, ui->count, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
    if (!SDL_SubmitGPUCommandBuffer(ui->cmd)) return false;
    ui->cmd = NULL;
    ui->swap = NULL;
    return true;
}
