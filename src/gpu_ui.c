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

#define MAX_VERTS GPU_UI_MAX_VERTS

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
    if (stage == SDL_GPU_SHADERSTAGE_FRAGMENT) i.num_samplers = 1;
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
    ui->clip_ids = calloc(MAX_VERTS, sizeof(int));
    if (!ui->verts || !ui->clip_ids) {
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
    SDL_GPUVertexAttribute attrs[3] = {
        {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 0},
        {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 2 * sizeof(float)},
        {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 4 * sizeof(float)},
    };
    SDL_GPUGraphicsPipelineCreateInfo ci = {0};
    ci.vertex_shader = vs;
    ci.fragment_shader = fs;
    ci.vertex_input_state.vertex_buffer_descriptions = &vbd;
    ci.vertex_input_state.num_vertex_buffers = 1;
    ci.vertex_input_state.vertex_attributes = attrs;
    ci.vertex_input_state.num_vertex_attributes = 3;
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
    SDL_GPUTextureCreateInfo ti = {0};
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ti.width = 1;
    ti.height = 1;
    ti.layer_count_or_depth = 1;
    ti.num_levels = 1;
    ui->white = SDL_CreateGPUTexture(gpu, &ti);
    SDL_GPUSamplerCreateInfo si = {0};
    si.min_filter = SDL_GPU_FILTER_NEAREST;
    si.mag_filter = SDL_GPU_FILTER_NEAREST;
    si.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    si.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    ui->sampler = SDL_CreateGPUSampler(gpu, &si);
    if (!ui->white || !ui->sampler) {
        snprintf(err, err_cap, "Could not create GPU UI texture");
        return false;
    }
    SDL_GPUTransferBufferCreateInfo txi = {0};
    txi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    txi.size = 4;
    SDL_GPUTransferBuffer *txb = SDL_CreateGPUTransferBuffer(gpu, &txi);
    void *px = SDL_MapGPUTransferBuffer(gpu, txb, false);
    ((Uint8 *)px)[0] = 255; ((Uint8 *)px)[1] = 255;
    ((Uint8 *)px)[2] = 255; ((Uint8 *)px)[3] = 255;
    SDL_UnmapGPUTransferBuffer(gpu, txb);
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(gpu);
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureTransferInfo src = {txb, 0, 1, 1};
    SDL_GPUTextureRegion dst = {ui->white, 0, 0, 0, 1, 1, 1};
    SDL_UploadToGPUTexture(copy, &src, &dst, false);
    SDL_EndGPUCopyPass(copy);
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_ReleaseGPUTransferBuffer(gpu, txb);
    return true;
}

void gpu_ui_shutdown(GpuUi *ui)
{
    if (!ui) return;
    if (ui->gpu) {
        if (ui->pipeline) SDL_ReleaseGPUGraphicsPipeline(ui->gpu, ui->pipeline);
        if (ui->vbo) SDL_ReleaseGPUBuffer(ui->gpu, ui->vbo);
        if (ui->tbo) SDL_ReleaseGPUTransferBuffer(ui->gpu, ui->tbo);
        if (ui->white) SDL_ReleaseGPUTexture(ui->gpu, ui->white);
        if (ui->sampler) SDL_ReleaseGPUSampler(ui->gpu, ui->sampler);
    }
    free(ui->verts);
    free(ui->clip_ids);
    memset(ui, 0, sizeof(*ui));
}

bool gpu_ui_begin(GpuUi *ui, int win_w, int win_h, GpuUiRect clear)
{
    (void)clear;
    ui->win_w = win_w;
    ui->win_h = win_h;
    ui->count = 0;
    ui->clip_depth = 0;
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
        {l, t, 0, 0, cr, cg, cb, ca}, {rt, t, 1, 0, cr, cg, cb, ca}, {rt, b, 1, 1, cr, cg, cb, ca},
        {l, t, 0, 0, cr, cg, cb, ca}, {rt, b, 1, 1, cr, cg, cb, ca}, {l, b, 0, 1, cr, cg, cb, ca},
    };
    memcpy(&ui->verts[ui->count], q, sizeof(q));
    int id = ui->clip_depth > 0 ? ui->clip_depth - 1 : -1;
    for (int i = 0; i < 6; i++) ui->clip_ids[ui->count + i] = id;
    ui->count += 6;
}

void gpu_ui_push_clip(GpuUi *ui, GpuUiRect r)
{
    if (!ui || ui->clip_depth >= GPU_UI_CLIP_STACK) return;
    if (ui->clip_depth > 0) {
        GpuUiRect p = ui->clips[ui->clip_depth - 1];
        float x0 = r.x > p.x ? r.x : p.x;
        float y0 = r.y > p.y ? r.y : p.y;
        float x1 = r.x + r.w < p.x + p.w ? r.x + r.w : p.x + p.w;
        float y1 = r.y + r.h < p.y + p.h ? r.y + r.h : p.y + p.h;
        r = (GpuUiRect){x0, y0, x1 > x0 ? x1 - x0 : 0, y1 > y0 ? y1 - y0 : 0};
    }
    ui->clips[ui->clip_depth++] = r;
}

void gpu_ui_pop_clip(GpuUi *ui)
{
    if (ui && ui->clip_depth > 0) ui->clip_depth--;
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
    SDL_GPUTextureSamplerBinding ts = {ui->white, ui->sampler};
    SDL_BindGPUFragmentSamplers(pass, 0, &ts, 1);
    int start = 0;
    while (start < ui->count) {
        int id = ui->clip_ids[start];
        int end = start;
        while (end < ui->count && ui->clip_ids[end] == id) end++;
        SDL_Rect sc = {0, 0, ui->win_w, ui->win_h};
        if (id >= 0) {
            GpuUiRect c = ui->clips[id];
            if (c.w <= 0 || c.h <= 0) { start = end; continue; }
            sc.x = (int)c.x;
            sc.y = (int)c.y;
            sc.w = (int)(c.w + 0.5f);
            sc.h = (int)(c.h + 0.5f);
        }
        SDL_SetGPUScissor(pass, &sc);
        SDL_DrawGPUPrimitives(pass, end - start, 1, start, 0);
        start = end;
    }
    SDL_EndGPURenderPass(pass);
    if (!SDL_SubmitGPUCommandBuffer(ui->cmd)) return false;
    ui->cmd = NULL;
    ui->swap = NULL;
    return true;
}
