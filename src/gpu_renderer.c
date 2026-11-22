#include "gpu_renderer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu_shaders.h"

typedef struct { float x, y, u, v, r, g, b, a; } Vertex;

typedef struct {
    GpuTexture *tex;
    int first, count;
    int clip;
} DrawCmd;

struct GpuTexture {
    SDL_GPUTexture *tex;
    int w, h;
    bool dirty;
    unsigned char *pending;
    int pending_pitch;
};

#define MAX_VERTS 65536
#define MAX_DRAWS 2048
#define ATLAS_SIZE 4096
#define CLIP_STACK 32
#define MAX_CLIPS 512

struct GpuRenderer {
    SDL_GPUDevice *gpu;
    SDL_Window *win;
    SDL_GPUGraphicsPipeline *pipeline;
    SDL_GPUSampler *sampler;
    SDL_GPUBuffer *vbo;
    SDL_GPUTransferBuffer *tbo;
    SDL_GPUCommandBuffer *cmd;
    SDL_GPUTexture *swap;
    Uint32 swap_w, swap_h;
    int logical_w, logical_h;
    SDL_GPUTextureFormat swap_format;

    Vertex *verts;
    int vert_count;
    DrawCmd *draws;
    int draw_count;

    GpuRect clips[MAX_CLIPS];
    int clip_stack[CLIP_STACK];
    int clip_depth;
    int clip_count;

    GpuTexture *white;
    GpuTexture *atlas_tex;
    unsigned char *atlas;
    int atlas_x, atlas_y, atlas_row_h;
    bool atlas_dirty;

    GpuTexture **textures;
    int texture_count;
    SDL_GPUTransferBuffer *upload_tbs[256];
    int upload_count;
    char screenshot_path[1024];
};

static bool make_shader(SDL_GPUDevice *gpu, SDL_GPUShaderStage stage, SDL_GPUShader **out)
{
    SDL_GPUShaderCreateInfo i = {0};
#if defined(__APPLE__)
    const bool vert = stage == SDL_GPU_SHADERSTAGE_VERTEX;
    i.code = (const Uint8 *)(vert ? UI_VERT_MSL : UI_FRAG_MSL);
    i.code_size = SDL_strlen(vert ? UI_VERT_MSL : UI_FRAG_MSL);
    i.entrypoint = "main0";
    i.format = SDL_GPU_SHADERFORMAT_MSL;
#else
    const bool vert = stage == SDL_GPU_SHADERSTAGE_VERTEX;
    i.code = vert ? UI_VERT_SPV : UI_FRAG_SPV;
    i.code_size = vert ? UI_VERT_SPV_LEN : UI_FRAG_SPV_LEN;
    i.entrypoint = "main";
    i.format = SDL_GPU_SHADERFORMAT_SPIRV;
#endif
    i.stage = stage;
    if (stage == SDL_GPU_SHADERSTAGE_FRAGMENT) i.num_samplers = 1;
    *out = SDL_CreateGPUShader(gpu, &i);
    return *out != NULL;
}

static GpuTexture *texture_new(GpuRenderer *r, int w, int h)
{
    if (w <= 0 || h <= 0) return NULL;
    GpuTexture *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->w = w;
    t->h = h;
    SDL_GPUTextureCreateInfo ti = {0};
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ti.width = (Uint32)w;
    ti.height = (Uint32)h;
    ti.layer_count_or_depth = 1;
    ti.num_levels = 1;
    t->tex = SDL_CreateGPUTexture(r->gpu, &ti);
    if (!t->tex) {
        free(t);
        return NULL;
    }
    GpuTexture **grown = realloc(r->textures, sizeof(*grown) * (r->texture_count + 1));
    if (!grown) {
        SDL_ReleaseGPUTexture(r->gpu, t->tex);
        free(t);
        return NULL;
    }
    r->textures = grown;
    r->textures[r->texture_count++] = t;
    return t;
}

GpuTexture *gpu_texture_create(GpuRenderer *r, int w, int h)
{
    return texture_new(r, w, h);
}

void gpu_texture_update(GpuTexture *t, const void *rgba, int pitch)
{
    if (!t || !rgba) return;
    free(t->pending);
    t->pending = malloc((size_t)t->w * t->h * 4);
    if (!t->pending) return;
    for (int y = 0; y < t->h; y++)
        memcpy(t->pending + (size_t)y * t->w * 4, (const unsigned char *)rgba + (size_t)y * pitch,
               (size_t)t->w * 4);
    t->pending_pitch = t->w * 4;
    t->dirty = true;
}

void gpu_texture_destroy(GpuRenderer *r, GpuTexture *t)
{
    if (!r || !t) return;
    for (int i = 0; i < r->texture_count; i++) {
        if (r->textures[i] == t) {
            r->textures[i] = r->textures[--r->texture_count];
            break;
        }
    }
    if (t->tex) SDL_ReleaseGPUTexture(r->gpu, t->tex);
    free(t->pending);
    free(t);
}

int gpu_texture_width(const GpuTexture *t) { return t ? t->w : 0; }
int gpu_texture_height(const GpuTexture *t) { return t ? t->h : 0; }

GpuRenderer *gpu_renderer_create(SDL_Window *win, char *err, size_t err_cap)
{
    GpuRenderer *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->win = win;
#if defined(__APPLE__)
    SDL_GPUShaderFormat formats = SDL_GPU_SHADERFORMAT_MSL;
#elif defined(_WIN32)
    SDL_GPUShaderFormat formats = SDL_GPU_SHADERFORMAT_SPIRV;
#else
    SDL_GPUShaderFormat formats = SDL_GPU_SHADERFORMAT_SPIRV;
#endif
    r->gpu = SDL_CreateGPUDevice(formats, true, NULL);
    if (!r->gpu) {
        snprintf(err, err_cap, "GPU device: %s", SDL_GetError());
        free(r);
        return NULL;
    }
    if (!SDL_ClaimWindowForGPUDevice(r->gpu, win)) {
        snprintf(err, err_cap, "GPU window: %s", SDL_GetError());
        SDL_DestroyGPUDevice(r->gpu);
        free(r);
        return NULL;
    }
    r->swap_format = SDL_GetGPUSwapchainTextureFormat(r->gpu, win);
    SDL_GPUShader *vs = NULL, *fs = NULL;
    if (!make_shader(r->gpu, SDL_GPU_SHADERSTAGE_VERTEX, &vs) ||
        !make_shader(r->gpu, SDL_GPU_SHADERSTAGE_FRAGMENT, &fs)) {
        snprintf(err, err_cap, "GPU shader: %s", SDL_GetError());
        return NULL;
    }
    SDL_GPUVertexBufferDescription vbd = {0};
    vbd.slot = 0;
    vbd.pitch = sizeof(Vertex);
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
    target.format = SDL_GetGPUSwapchainTextureFormat(r->gpu, win);
    target.blend_state.enable_blend = true;
    target.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    target.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    target.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
    target.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    target.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    target.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    ci.target_info.color_target_descriptions = &target;
    ci.target_info.num_color_targets = 1;
    r->pipeline = SDL_CreateGPUGraphicsPipeline(r->gpu, &ci);
    SDL_ReleaseGPUShader(r->gpu, vs);
    SDL_ReleaseGPUShader(r->gpu, fs);
    if (!r->pipeline) {
        snprintf(err, err_cap, "GPU pipeline: %s", SDL_GetError());
        return NULL;
    }
    SDL_GPUSamplerCreateInfo si = {0};
    si.min_filter = SDL_GPU_FILTER_LINEAR;
    si.mag_filter = SDL_GPU_FILTER_LINEAR;
    si.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    si.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    r->sampler = SDL_CreateGPUSampler(r->gpu, &si);
    SDL_GPUBufferCreateInfo bci = {0};
    bci.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    bci.size = sizeof(Vertex) * MAX_VERTS;
    r->vbo = SDL_CreateGPUBuffer(r->gpu, &bci);
    SDL_GPUTransferBufferCreateInfo tci = {0};
    tci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tci.size = sizeof(Vertex) * MAX_VERTS;
    r->tbo = SDL_CreateGPUTransferBuffer(r->gpu, &tci);
    r->verts = calloc(MAX_VERTS, sizeof(Vertex));
    r->draws = calloc(MAX_DRAWS, sizeof(DrawCmd));
    r->atlas = calloc((size_t)ATLAS_SIZE * ATLAS_SIZE, 4);
    if (!r->sampler || !r->vbo || !r->tbo || !r->verts || !r->draws || !r->atlas) {
        snprintf(err, err_cap, "GPU allocation failed");
        return NULL;
    }
    r->atlas_x = 2;
    r->atlas_y = 2;
    r->atlas_row_h = 0;
    r->atlas[0] = r->atlas[1] = r->atlas[2] = r->atlas[3] = 255;
    r->white = texture_new(r, 1, 1);
    if (!r->white) return NULL;
    unsigned char white[4] = {255, 255, 255, 255};
    gpu_texture_update(r->white, white, 4);
    r->atlas_tex = texture_new(r, ATLAS_SIZE, ATLAS_SIZE);
    if (!r->atlas_tex) return NULL;
    r->atlas_dirty = true;
    return r;
}

void gpu_renderer_destroy(GpuRenderer *r)
{
    if (!r) return;
    if (r->atlas_tex) gpu_texture_destroy(r, r->atlas_tex);
    if (r->white) gpu_texture_destroy(r, r->white);
    while (r->texture_count > 0) gpu_texture_destroy(r, r->textures[0]);
    if (r->gpu) {
        if (r->pipeline) SDL_ReleaseGPUGraphicsPipeline(r->gpu, r->pipeline);
        if (r->sampler) SDL_ReleaseGPUSampler(r->gpu, r->sampler);
        if (r->vbo) SDL_ReleaseGPUBuffer(r->gpu, r->vbo);
        if (r->tbo) SDL_ReleaseGPUTransferBuffer(r->gpu, r->tbo);
        SDL_ReleaseWindowFromGPUDevice(r->gpu, r->win);
        SDL_DestroyGPUDevice(r->gpu);
    }
    free(r->verts);
    free(r->draws);
    free(r->atlas);
    free(r->textures);
    free(r);
}

bool gpu_renderer_begin(GpuRenderer *r, int logical_w, int logical_h)
{
    r->logical_w = logical_w;
    r->logical_h = logical_h;
    r->vert_count = 0;
    r->draw_count = 0;
    r->clip_depth = 0;
    r->clip_count = 0;
    r->cmd = SDL_AcquireGPUCommandBuffer(r->gpu);
    if (!r->cmd) return false;
    if (!SDL_AcquireGPUSwapchainTexture(r->cmd, r->win, &r->swap, &r->swap_w, &r->swap_h))
        return false;
    return r->swap != NULL;
}

void gpu_renderer_push_clip(GpuRenderer *r, GpuRect c)
{
    if (r->clip_depth > 0) {
        GpuRect p = r->clips[r->clip_stack[r->clip_depth - 1]];
        float x0 = c.x > p.x ? c.x : p.x;
        float y0 = c.y > p.y ? c.y : p.y;
        float x1 = c.x + c.w < p.x + p.w ? c.x + c.w : p.x + p.w;
        float y1 = c.y + c.h < p.y + p.h ? c.y + c.h : p.y + p.h;
        c = (GpuRect){x0, y0, x1 > x0 ? x1 - x0 : 0, y1 > y0 ? y1 - y0 : 0};
    }
    if (r->clip_depth >= CLIP_STACK || r->clip_count >= MAX_CLIPS) return;
    int id = r->clip_count++;
    r->clips[id] = c;
    r->clip_stack[r->clip_depth++] = id;
}

void gpu_renderer_pop_clip(GpuRenderer *r)
{
    if (r->clip_depth > 0) r->clip_depth--;
}

static void emit(GpuRenderer *r, GpuTexture *tex, int clip, float x, float y, float w, float h,
                 float u0, float v0, float u1, float v1, GpuColor c)
{
    if (!r || !tex || r->vert_count + 6 > MAX_VERTS || r->draw_count >= MAX_DRAWS) return;
    if (r->draw_count == 0 || r->draws[r->draw_count - 1].tex != tex ||
        r->draws[r->draw_count - 1].clip != clip) {
        DrawCmd *d = &r->draws[r->draw_count++];
        d->tex = tex;
        d->first = r->vert_count;
        d->count = 0;
        d->clip = clip;
    }
    float l = (x / (float)r->logical_w) * 2 - 1;
    float rt = ((x + w) / (float)r->logical_w) * 2 - 1;
    float t = 1 - (y / (float)r->logical_h) * 2;
    float b = 1 - ((y + h) / (float)r->logical_h) * 2;
    float cr = c.r / 255.0f, cg = c.g / 255.0f, cb = c.b / 255.0f, ca = c.a / 255.0f;
    Vertex *v = &r->verts[r->vert_count];
    Vertex q[6] = {
        {l, t, u0, v0, cr, cg, cb, ca}, {rt, t, u1, v0, cr, cg, cb, ca},
        {rt, b, u1, v1, cr, cg, cb, ca}, {l, t, u0, v0, cr, cg, cb, ca},
        {rt, b, u1, v1, cr, cg, cb, ca}, {l, b, u0, v1, cr, cg, cb, ca},
    };
    memcpy(v, q, sizeof(q));
    r->vert_count += 6;
    r->draws[r->draw_count - 1].count += 6;
}

void gpu_renderer_fill(GpuRenderer *r, GpuRect dst, GpuColor c)
{
    emit(r, r->white, r->clip_depth > 0 ? r->clip_stack[r->clip_depth - 1] : -1, dst.x, dst.y, dst.w, dst.h, 0, 0,
         1, 1, c);
}

void gpu_renderer_line(GpuRenderer *r, float x0, float y0, float x1, float y1, float thickness,
                       GpuColor c)
{
    float dx = x1 - x0, dy = y1 - y0;
    float len = SDL_sqrtf(dx * dx + dy * dy);
    if (len < 0.001f) return;
    float nx = -dy / len * thickness * 0.5f;
    float ny = dx / len * thickness * 0.5f;
    if (!r || r->vert_count + 6 > MAX_VERTS || r->draw_count >= MAX_DRAWS) return;
    if (r->draw_count == 0 || r->draws[r->draw_count - 1].tex != r->white) {
        DrawCmd *d = &r->draws[r->draw_count++];
        d->tex = r->white;
        d->first = r->vert_count;
        d->count = 0;
        d->clip = r->clip_depth > 0 ? r->clip_stack[r->clip_depth - 1] : -1;
    }
    float l0 = (x0 + nx) / r->logical_w * 2 - 1;
    float t0 = 1 - (y0 + ny) / r->logical_h * 2;
    float l1 = (x1 + nx) / r->logical_w * 2 - 1;
    float t1 = 1 - (y1 + ny) / r->logical_h * 2;
    float l2 = (x1 - nx) / r->logical_w * 2 - 1;
    float t2 = 1 - (y1 - ny) / r->logical_h * 2;
    float l3 = (x0 - nx) / r->logical_w * 2 - 1;
    float t3 = 1 - (y0 - ny) / r->logical_h * 2;
    float cr = c.r / 255.0f, cg = c.g / 255.0f, cb = c.b / 255.0f, ca = c.a / 255.0f;
    Vertex q[6] = {{l0, t0, 0, 0, cr, cg, cb, ca}, {l1, t1, 1, 0, cr, cg, cb, ca},
                   {l2, t2, 1, 1, cr, cg, cb, ca}, {l0, t0, 0, 0, cr, cg, cb, ca},
                   {l2, t2, 1, 1, cr, cg, cb, ca}, {l3, t3, 0, 1, cr, cg, cb, ca}};
    memcpy(&r->verts[r->vert_count], q, sizeof(q));
    r->vert_count += 6;
    r->draws[r->draw_count - 1].count += 6;
}

void gpu_renderer_texture(GpuRenderer *r, GpuTexture *tex, GpuRect dst, float u0, float v0,
                          float u1, float v1, GpuColor c)
{
    emit(r, tex, r->clip_depth > 0 ? r->clip_stack[r->clip_depth - 1] : -1, dst.x, dst.y, dst.w, dst.h, u0, v0,
         u1, v1, c);
}

void gpu_renderer_texture_rect(GpuRenderer *r, GpuTexture *tex, GpuRect dst, GpuColor c)
{
    emit(r, tex, r->clip_depth > 0 ? r->clip_stack[r->clip_depth - 1] : -1, dst.x, dst.y, dst.w, dst.h, 0, 0, 1,
         1, c);
}

static bool upload_texture(GpuRenderer *r, GpuTexture *t, SDL_GPUCopyPass *copy)
{
    if (!t->dirty || !t->pending) return true;
    SDL_GPUTransferBufferCreateInfo tci = {0};
    tci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tci.size = (Uint32)t->w * t->h * 4;
    SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(r->gpu, &tci);
    if (!tb) return false;
    void *map = SDL_MapGPUTransferBuffer(r->gpu, tb, false);
    if (!map) return false;
    memcpy(map, t->pending, (size_t)t->w * t->h * 4);
    SDL_UnmapGPUTransferBuffer(r->gpu, tb);
    SDL_GPUTextureTransferInfo src = {tb, 0, (Uint32)t->w, (Uint32)t->h};
    SDL_GPUTextureRegion dst = {t->tex, 0, 0, 0, 0, 0, (Uint32)t->w, (Uint32)t->h, 1};
    SDL_UploadToGPUTexture(copy, &src, &dst, false);
    if (r->upload_count < 256) r->upload_tbs[r->upload_count++] = tb;
    free(t->pending);
    t->pending = NULL;
    t->dirty = false;
    return true;
}

bool gpu_renderer_end(GpuRenderer *r)
{
    if (!r->cmd || !r->swap) return false;
    if (r->atlas_dirty) {
        if (r->atlas_tex->pending) free(r->atlas_tex->pending);
        r->atlas_tex->pending = malloc((size_t)ATLAS_SIZE * ATLAS_SIZE * 4);
        memcpy(r->atlas_tex->pending, r->atlas, (size_t)ATLAS_SIZE * ATLAS_SIZE * 4);
        r->atlas_tex->pending_pitch = ATLAS_SIZE * 4;
        r->atlas_tex->dirty = true;
        r->atlas_dirty = false;
    }
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(r->cmd);
    for (int i = 0; i < r->texture_count; i++) upload_texture(r, r->textures[i], copy);
    if (r->atlas_tex->dirty) upload_texture(r, r->atlas_tex, copy);
    void *map = SDL_MapGPUTransferBuffer(r->gpu, r->tbo, false);
    memcpy(map, r->verts, sizeof(Vertex) * r->vert_count);
    SDL_UnmapGPUTransferBuffer(r->gpu, r->tbo);
    SDL_GPUTransferBufferLocation src = {r->tbo, 0};
    SDL_GPUBufferRegion dst = {r->vbo, 0, (Uint32)(sizeof(Vertex) * r->vert_count)};
    SDL_UploadToGPUBuffer(copy, &src, &dst, false);
    SDL_EndGPUCopyPass(copy);
    SDL_GPUColorTargetInfo ct = {0};
    ct.texture = r->swap;
    ct.clear_color = (SDL_FColor){0.03f, 0.04f, 0.05f, 1.0f};
    ct.load_op = SDL_GPU_LOADOP_CLEAR;
    ct.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(r->cmd, &ct, 1, NULL);
    SDL_BindGPUGraphicsPipeline(pass, r->pipeline);
    SDL_GPUBufferBinding binding = {r->vbo, 0};
    SDL_BindGPUVertexBuffers(pass, 0, &binding, 1);
    for (int i = 0; i < r->draw_count; i++) {
        DrawCmd *d = &r->draws[i];
        if (!d->tex || d->count == 0) continue;
        SDL_Rect sc = {0, 0, r->swap_w, r->swap_h};
        if (d->clip >= 0) {
            GpuRect c = r->clips[d->clip];
            sc.x = (int)(c.x * r->swap_w / r->logical_w);
            sc.y = (int)(c.y * r->swap_h / r->logical_h);
            sc.w = (int)(c.w * r->swap_w / r->logical_w);
            sc.h = (int)(c.h * r->swap_h / r->logical_h);
            if (sc.w <= 0 || sc.h <= 0) continue;
        }
        SDL_SetGPUScissor(pass, &sc);
        SDL_GPUTextureSamplerBinding ts = {d->tex->tex, r->sampler};
        SDL_BindGPUFragmentSamplers(pass, 0, &ts, 1);
        SDL_DrawGPUPrimitives(pass, d->count, 1, d->first, 0);
    }
    SDL_EndGPURenderPass(pass);
    bool shot = r->screenshot_path[0] != 0;
    SDL_GPUTransferBuffer *shot_tb = NULL;
    if (shot) {
        SDL_GPUTransferBufferCreateInfo tci = {0};
        tci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
        tci.size = (Uint32)r->swap_w * (Uint32)r->swap_h * 4;
        shot_tb = SDL_CreateGPUTransferBuffer(r->gpu, &tci);
        if (shot_tb) {
            SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(r->cmd);
            SDL_GPUTextureRegion src = {r->swap, 0, 0, 0, 0, 0,
                                        (Uint32)r->swap_w, (Uint32)r->swap_h, 1};
            SDL_GPUTextureTransferInfo dst = {shot_tb, 0, (Uint32)r->swap_w,
                                              (Uint32)r->swap_h};
            SDL_DownloadFromGPUTexture(cp, &src, &dst);
            SDL_EndGPUCopyPass(cp);
        }
    }
    SDL_GPUFence *fence = NULL;
    if (shot) fence = SDL_SubmitGPUCommandBufferAndAcquireFence(r->cmd);
    else if (!SDL_SubmitGPUCommandBuffer(r->cmd)) return false;
    if (shot) {
        if (fence) {
            SDL_WaitForGPUFences(r->gpu, true, &fence, 1);
            SDL_ReleaseGPUFence(r->gpu, fence);
        } else {
            SDL_WaitForGPUIdle(r->gpu);
        }
        if (shot_tb) {
            void *px = SDL_MapGPUTransferBuffer(r->gpu, shot_tb, false);
            if (px) {
                SDL_PixelFormat fmt = r->swap_format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM
                                          ? SDL_PIXELFORMAT_BGRA32
                                          : SDL_PIXELFORMAT_RGBA32;
                SDL_Surface *surf = SDL_CreateSurface((int)r->swap_w, (int)r->swap_h, fmt);
                if (surf) {
                    for (Uint32 y = 0; y < r->swap_h; y++)
                        memcpy((Uint8 *)surf->pixels + (size_t)y * surf->pitch,
                               (Uint8 *)px + (size_t)y * r->swap_w * 4,
                               (size_t)r->swap_w * 4);
                    SDL_Surface *out = SDL_CreateSurface(r->logical_w, r->logical_h, fmt);
                    if (out) {
                        SDL_BlitSurfaceScaled(surf, NULL, out, NULL, SDL_SCALEMODE_LINEAR);
                        SDL_SavePNG(out, r->screenshot_path);
                        SDL_DestroySurface(out);
                    } else {
                        SDL_SavePNG(surf, r->screenshot_path);
                    }
                    SDL_DestroySurface(surf);
                }
                SDL_UnmapGPUTransferBuffer(r->gpu, shot_tb);
            }
            SDL_ReleaseGPUTransferBuffer(r->gpu, shot_tb);
        }
        r->screenshot_path[0] = 0;
    }
    for (int i = 0; i < r->upload_count; i++)
        SDL_ReleaseGPUTransferBuffer(r->gpu, r->upload_tbs[i]);
    r->upload_count = 0;
    r->cmd = NULL;
    r->swap = NULL;
    return true;
}

int gpu_atlas_size(GpuRenderer *r) { return r ? ATLAS_SIZE : 0; }
unsigned char *gpu_atlas_pixels(GpuRenderer *r) { return r ? r->atlas : NULL; }
GpuTexture *gpu_atlas_texture(GpuRenderer *r) { return r ? r->atlas_tex : NULL; }
void gpu_atlas_dirty(GpuRenderer *r) { if (r) r->atlas_dirty = true; }

bool gpu_atlas_alloc(GpuRenderer *r, int w, int h, int *x, int *y)
{
    if (!r || w <= 0 || h <= 0) return false;
    if (r->atlas_x + w + 2 > ATLAS_SIZE) {
        r->atlas_x = 2;
        r->atlas_y += r->atlas_row_h + 2;
        r->atlas_row_h = 0;
    }
    if (r->atlas_y + h + 2 > ATLAS_SIZE) return false;
    *x = r->atlas_x;
    *y = r->atlas_y;
    r->atlas_x += w + 2;
    if (h > r->atlas_row_h) r->atlas_row_h = h;
    return true;
}

bool gpu_renderer_screenshot(GpuRenderer *r, const char *path)
{
    if (!r || !path || !*path) return false;
    SDL_strlcpy(r->screenshot_path, path, sizeof(r->screenshot_path));
    return true;
}
