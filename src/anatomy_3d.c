#include "anatomy_3d.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"
#include "stb_image.h"

#include "app.h"
#include "anatomy_shaders.h"
#include "util.h"

typedef struct { float p[3], n[3], u[2]; } Vertex;
typedef struct { float mvp[16]; float light[4]; } Uniforms;

struct Anatomy3D {
    App *app;
    GpuRenderer *r;
    bool loaded;
    bool failed;
    GpuTexture *color;
    SDL_GPUTexture *depth;
    SDL_GPUGraphicsPipeline *pipeline;
    SDL_GPUShader *vs;
    SDL_GPUShader *fs;
    SDL_GPUBuffer *vbo;
    SDL_GPUBuffer *ibo;
    SDL_GPUSampler *sampler;
    GpuTexture *texture;
    Uint32 index_count[2];
    Uint32 index_offset[2];
    int target_w, target_h;
    float yaw, pitch, distance;
    bool dragging;
    float last_x, last_y;
    int selected; /* -1 both, 0 female, 1 male */
};

static void mat_identity(float m[16])
{
    memset(m, 0, sizeof(float) * 16);
    m[0] = m[5] = m[10] = m[15] = 1;
}

static void mat_mul(float out[16], const float a[16], const float b[16])
{
    float r[16];
    for (int c = 0; c < 4; c++) {
        for (int row = 0; row < 4; row++) {
            r[c * 4 + row] = a[0 * 4 + row] * b[c * 4 + 0] +
                             a[1 * 4 + row] * b[c * 4 + 1] +
                             a[2 * 4 + row] * b[c * 4 + 2] +
                             a[3 * 4 + row] * b[c * 4 + 3];
        }
    }
    memcpy(out, r, sizeof(r));
}

static void mat_perspective(float m[16], float fovy, float aspect, float z_near, float z_far)
{
    memset(m, 0, sizeof(float) * 16);
    float f = 1.0f / tanf(fovy * 0.5f);
    m[0] = f / aspect;
    m[5] = f;
    m[10] = (z_far + z_near) / (z_near - z_far);
    m[11] = -1.0f;
    m[14] = (2.0f * z_far * z_near) / (z_near - z_far);
}

static void vec3_sub(float o[3], const float a[3], const float b[3])
{
    o[0] = a[0] - b[0]; o[1] = a[1] - b[1]; o[2] = a[2] - b[2];
}
static void vec3_cross(float o[3], const float a[3], const float b[3])
{
    o[0] = a[1] * b[2] - a[2] * b[1];
    o[1] = a[2] * b[0] - a[0] * b[2];
    o[2] = a[0] * b[1] - a[1] * b[0];
}
static float vec3_dot(const float a[3], const float b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
static void vec3_norm(float a[3])
{
    float l = sqrtf(vec3_dot(a, a));
    if (l > 0.00001f) { a[0] /= l; a[1] /= l; a[2] /= l; }
}

static void mat_look_at(float m[16], const float eye[3], const float center[3],
                        const float up[3])
{
    float f[3], s[3], u[3];
    vec3_sub(f, center, eye);
    vec3_norm(f);
    vec3_cross(s, f, up);
    vec3_norm(s);
    vec3_cross(u, s, f);
    m[0] = s[0]; m[4] = s[1]; m[8] = s[2]; m[12] = -vec3_dot(s, eye);
    m[1] = u[0]; m[5] = u[1]; m[9] = u[2]; m[13] = -vec3_dot(u, eye);
    m[2] = -f[0]; m[6] = -f[1]; m[10] = -f[2]; m[14] = vec3_dot(f, eye);
    m[3] = 0; m[7] = 0; m[11] = 0; m[15] = 1;
}

static bool make_shader(GpuRenderer *r, SDL_GPUShaderStage stage, SDL_GPUShader **out)
{
    SDL_GPUShaderCreateInfo i = {0};
#if defined(__APPLE__)
    bool vert = stage == SDL_GPU_SHADERSTAGE_VERTEX;
    i.code = (const Uint8 *)(vert ? ANATOMY_VERT_MSL : ANATOMY_FRAG_MSL);
    i.code_size = SDL_strlen(vert ? ANATOMY_VERT_MSL : ANATOMY_FRAG_MSL);
    i.entrypoint = "main0";
    i.format = SDL_GPU_SHADERFORMAT_MSL;
#else
    bool vert = stage == SDL_GPU_SHADERSTAGE_VERTEX;
    i.code = vert ? ANATOMY_VERT_SPV : ANATOMY_FRAG_SPV;
    i.code_size = vert ? ANATOMY_VERT_SPV_LEN : ANATOMY_FRAG_SPV_LEN;
    i.entrypoint = "main";
    i.format = SDL_GPU_SHADERFORMAT_SPIRV;
#endif
    i.stage = stage;
    i.num_uniform_buffers = 1;
    if (stage == SDL_GPU_SHADERSTAGE_FRAGMENT) i.num_samplers = 1;
    *out = SDL_CreateGPUShader(gpu_renderer_device(r), &i);
    return *out != NULL;
}

static bool upload_buffer(GpuRenderer *r, SDL_GPUBuffer *buf, const void *data, size_t size)
{
    SDL_GPUDevice *gpu = gpu_renderer_device(r);
    SDL_GPUTransferBufferCreateInfo tci = {0};
    tci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tci.size = (Uint32)size;
    SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(gpu, &tci);
    if (!tb) return false;
    void *m = SDL_MapGPUTransferBuffer(gpu, tb, false);
    memcpy(m, data, size);
    SDL_UnmapGPUTransferBuffer(gpu, tb);
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(gpu);
    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTransferBufferLocation src = {tb, 0};
    SDL_GPUBufferRegion dst = {buf, 0, (Uint32)size};
    SDL_UploadToGPUBuffer(cp, &src, &dst, false);
    SDL_EndGPUCopyPass(cp);
    bool ok = SDL_SubmitGPUCommandBuffer(cmd);
    SDL_ReleaseGPUTransferBuffer(gpu, tb);
    return ok;
}

static const char *find_model_path(App *app)
{
    static char path[CS_PATH_MAX];
    if (app->anatomy_model_path[0] && cs_file_exists(app->anatomy_model_path)) {
        cs_strlcpy(path, app->anatomy_model_path, sizeof(path));
        return path;
    }
    cs_path_join(path, sizeof(path), app->asset_dir,
                 "3d/ecorche-musclenames-male-female-anatomy/"
                 "Écorché Musclenames Male & Female Anatomy.glb");
    if (cs_file_exists(path)) return path;
    cs_path_join(path, sizeof(path), app->project_root,
                 "assets/3d/ecorche-musclenames-male-female-anatomy/"
                 "Écorché Musclenames Male & Female Anatomy.glb");
    return cs_file_exists(path) ? path : NULL;
}

static bool load_model(Anatomy3D *a)
{
    const char *path = find_model_path(a->app);
    if (!path) {
        a->failed = true;
        return false;
    }
    cgltf_options opt = {0};
    cgltf_data *data = NULL;
    if (cgltf_parse_file(&opt, path, &data) != cgltf_result_success) {
        a->failed = true;
        return false;
    }
    if (cgltf_load_buffers(&opt, data, path) != cgltf_result_success) {
        cgltf_free(data);
        a->failed = true;
        return false;
    }
    cgltf_node *node = NULL;
    cgltf_mesh *mesh = NULL;
    for (cgltf_size i = 0; i < data->nodes_count; i++) {
        if (data->nodes[i].mesh) {
            node = &data->nodes[i];
            mesh = node->mesh;
            break;
        }
    }
    if (!mesh || mesh->primitives_count < 2) {
        cgltf_free(data);
        a->failed = true;
        return false;
    }
    float world[16];
    if (node) cgltf_node_transform_world(node, world);
    else mat_identity(world);

    int prim_count = (int)(mesh->primitives_count < 2 ? mesh->primitives_count : 2);
    Vertex *verts = NULL;
    Uint32 *indices = NULL;
    Uint32 vert_count = 0, idx_count = 0;
    float bmin[3] = {1e30f, 1e30f, 1e30f}, bmax[3] = {-1e30f, -1e30f, -1e30f};
    int first_vert[2] = {0, 0}, count_vert[2] = {0, 0};

    for (int p = 0; p < prim_count; p++) {
        cgltf_primitive *pr = &mesh->primitives[p];
        cgltf_accessor *pa = NULL, *na = NULL, *ta = NULL;
        for (cgltf_size i = 0; i < pr->attributes_count; i++) {
            if (pr->attributes[i].type == cgltf_attribute_type_position) pa = pr->attributes[i].data;
            else if (pr->attributes[i].type == cgltf_attribute_type_normal) na = pr->attributes[i].data;
            else if (pr->attributes[i].type == cgltf_attribute_type_texcoord) ta = pr->attributes[i].data;
        }
        if (!pa || !ta) continue;
        first_vert[p] = (int)vert_count;
        count_vert[p] = (int)pa->count;
        verts = realloc(verts, sizeof(Vertex) * (vert_count + pa->count));
        for (cgltf_size i = 0; i < pa->count; i++) {
            float pos[3] = {0}, nor[3] = {0, 1, 0}, uv[2] = {0};
            cgltf_accessor_read_float(pa, i, pos, 3);
            if (na) cgltf_accessor_read_float(na, i, nor, 3);
            cgltf_accessor_read_float(ta, i, uv, 2);
            Vertex *v = &verts[vert_count++];
            v->p[0] = world[0] * pos[0] + world[4] * pos[1] + world[8] * pos[2] + world[12];
            v->p[1] = world[1] * pos[0] + world[5] * pos[1] + world[9] * pos[2] + world[13];
            v->p[2] = world[2] * pos[0] + world[6] * pos[1] + world[10] * pos[2] + world[14];
            v->n[0] = world[0] * nor[0] + world[4] * nor[1] + world[8] * nor[2];
            v->n[1] = world[1] * nor[0] + world[5] * nor[1] + world[9] * nor[2];
            v->n[2] = world[2] * nor[0] + world[6] * nor[1] + world[10] * nor[2];
            vec3_norm(v->n);
            v->u[0] = uv[0]; v->u[1] = uv[1];
            for (int k = 0; k < 3; k++) {
                if (v->p[k] < bmin[k]) bmin[k] = v->p[k];
                if (v->p[k] > bmax[k]) bmax[k] = v->p[k];
            }
        }
        if (pr->indices) {
            Uint32 *nidx = realloc(indices, sizeof(Uint32) * (idx_count + pr->indices->count));
            if (!nidx) break;
            indices = nidx;
            for (cgltf_size i = 0; i < pr->indices->count; i++) {
                indices[idx_count++] = (Uint32)first_vert[p] +
                    (Uint32)cgltf_accessor_read_index(pr->indices, i);
            }
            a->index_offset[p] = (Uint32)(idx_count - pr->indices->count);
            a->index_count[p] = (Uint32)pr->indices->count;
        }
    }
    float h = bmax[1] - bmin[1];
    if (h <= 0.0001f || vert_count == 0) {
        free(verts); free(indices); cgltf_free(data); a->failed = true; return false;
    }
    float scale = 1.65f / h;
    float cx = (bmin[0] + bmax[0]) * 0.5f;
    float cy = (bmin[1] + bmax[1]) * 0.5f;
    float cz = (bmin[2] + bmax[2]) * 0.5f;
    for (Uint32 i = 0; i < vert_count; i++) {
        int side = 0;
        for (int p = 0; p < prim_count; p++)
            if ((int)i >= first_vert[p] && (int)i < first_vert[p] + count_vert[p]) side = p;
        verts[i].p[0] = (verts[i].p[0] - cx) * scale + (side ? 0.38f : -0.38f);
        verts[i].p[1] = (verts[i].p[1] - cy) * scale;
        verts[i].p[2] = (verts[i].p[2] - cz) * scale;
    }

    SDL_GPUDevice *gpu = gpu_renderer_device(a->r);
    SDL_GPUBufferCreateInfo vbi = {0};
    vbi.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    vbi.size = (Uint32)(sizeof(Vertex) * vert_count);
    a->vbo = SDL_CreateGPUBuffer(gpu, &vbi);
    SDL_GPUBufferCreateInfo ibi = {0};
    ibi.usage = SDL_GPU_BUFFERUSAGE_INDEX;
    ibi.size = (Uint32)(sizeof(Uint32) * idx_count);
    a->ibo = SDL_CreateGPUBuffer(gpu, &ibi);
    if (!a->vbo || !a->ibo || !upload_buffer(a->r, a->vbo, verts, sizeof(Vertex) * vert_count) ||
        !upload_buffer(a->r, a->ibo, indices, sizeof(Uint32) * idx_count)) {
        free(verts); free(indices); cgltf_free(data); a->failed = true; return false;
    }

    /* First material texture, if present. */
    cgltf_primitive *p0 = &mesh->primitives[0];
    if (p0->material && p0->material->pbr_metallic_roughness.base_color_texture.texture) {
        cgltf_texture *ct = p0->material->pbr_metallic_roughness.base_color_texture.texture;
        cgltf_image *img = ct->image;
        if (img && img->buffer_view) {
            const unsigned char *bytes = (const unsigned char *)img->buffer_view->buffer->data +
                                         img->buffer_view->offset;
            int w = 0, h2 = 0, n = 0;
            unsigned char *pixels = stbi_load_from_memory(bytes, (int)img->buffer_view->size,
                                                          &w, &h2, &n, 4);
            if (pixels) {
                a->texture = gpu_texture_create_mipped(a->r, w, h2);
                if (a->texture) gpu_texture_update(a->texture, pixels, w * 4);
                stbi_image_free(pixels);
            }
        }
    }
    if (!a->texture) {
        a->texture = gpu_texture_create(a->r, 1, 1);
        unsigned char px[4] = {220, 200, 190, 255};
        if (a->texture) gpu_texture_update(a->texture, px, 4);
    }
    free(verts);
    free(indices);
    cgltf_free(data);
    a->loaded = true;
    return true;
}

static bool create_pipeline(Anatomy3D *a)
{
    SDL_GPUDevice *gpu = gpu_renderer_device(a->r);
    if (!make_shader(a->r, SDL_GPU_SHADERSTAGE_VERTEX, &a->vs) ||
        !make_shader(a->r, SDL_GPU_SHADERSTAGE_FRAGMENT, &a->fs)) return false;
    SDL_GPUVertexBufferDescription vbd = {0};
    vbd.slot = 0;
    vbd.pitch = sizeof(Vertex);
    vbd.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    SDL_GPUVertexAttribute attrs[3] = {
        {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 0},
        {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 3 * sizeof(float)},
        {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 6 * sizeof(float)},
    };
    SDL_GPUGraphicsPipelineCreateInfo ci = {0};
    ci.vertex_shader = a->vs;
    ci.fragment_shader = a->fs;
    ci.vertex_input_state.vertex_buffer_descriptions = &vbd;
    ci.vertex_input_state.num_vertex_buffers = 1;
    ci.vertex_input_state.vertex_attributes = attrs;
    ci.vertex_input_state.num_vertex_attributes = 3;
    ci.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    ci.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    ci.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    ci.depth_stencil_state.enable_depth_test = true;
    ci.depth_stencil_state.enable_depth_write = true;
    ci.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS;
    SDL_GPUColorTargetDescription ct = {0};
    ct.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    ci.target_info.color_target_descriptions = &ct;
    ci.target_info.num_color_targets = 1;
    ci.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    ci.target_info.has_depth_stencil_target = true;
    a->pipeline = SDL_CreateGPUGraphicsPipeline(gpu, &ci);
    SDL_GPUSamplerCreateInfo si = {0};
    si.min_filter = SDL_GPU_FILTER_LINEAR;
    si.mag_filter = SDL_GPU_FILTER_LINEAR;
    si.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    si.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    a->sampler = SDL_CreateGPUSampler(gpu, &si);
    return a->pipeline != NULL && a->sampler != NULL;
}

static bool ensure_target(Anatomy3D *a, int w, int h)
{
    if (a->color && a->target_w == w && a->target_h == h) return true;
    if (a->color) { gpu_texture_destroy(a->r, a->color); a->color = NULL; }
    if (a->depth) {
        SDL_ReleaseGPUTexture(gpu_renderer_device(a->r), a->depth);
        a->depth = NULL;
    }
    a->color = gpu_texture_create_target(a->r, w, h);
    if (!a->color) return false;
    SDL_GPUTextureCreateInfo ti = {0};
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    ti.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    ti.width = (Uint32)w;
    ti.height = (Uint32)h;
    ti.layer_count_or_depth = 1;
    ti.num_levels = 1;
    a->depth = SDL_CreateGPUTexture(gpu_renderer_device(a->r), &ti);
    a->target_w = w;
    a->target_h = h;
    return a->depth != NULL;
}

Anatomy3D *anatomy_3d_create(App *app)
{
    Anatomy3D *a = calloc(1, sizeof(*a));
    if (!a) return NULL;
    a->app = app;
    a->r = app->r;
    a->yaw = 0;
    a->pitch = 0;
    a->distance = 4.2f;
    a->selected = -1;
    if (!create_pipeline(a)) {
        a->failed = true;
        return a;
    }
    if (!load_model(a)) {
        a->failed = true;
    }
    return a;
}

void anatomy_3d_destroy(Anatomy3D *a)
{
    if (!a) return;
    SDL_GPUDevice *gpu = gpu_renderer_device(a->r);
    if (a->color) gpu_texture_destroy(a->r, a->color);
    if (a->depth) SDL_ReleaseGPUTexture(gpu, a->depth);
    if (a->texture) gpu_texture_destroy(a->r, a->texture);
    if (a->vbo) SDL_ReleaseGPUBuffer(gpu, a->vbo);
    if (a->ibo) SDL_ReleaseGPUBuffer(gpu, a->ibo);
    if (a->pipeline) SDL_ReleaseGPUGraphicsPipeline(gpu, a->pipeline);
    if (a->vs) SDL_ReleaseGPUShader(gpu, a->vs);
    if (a->fs) SDL_ReleaseGPUShader(gpu, a->fs);
    if (a->sampler) SDL_ReleaseGPUSampler(gpu, a->sampler);
    free(a);
}

bool anatomy_3d_available(const Anatomy3D *a) { return a && a->loaded && a->pipeline; }
GpuTexture *anatomy_3d_texture(const Anatomy3D *a) { return a ? a->color : NULL; }
int anatomy_3d_selection(const Anatomy3D *a) { return a ? a->selected : -1; }

void anatomy_3d_frame(App *app)
{
    Anatomy3D *a = app->anatomy;
    if (!a) return;
    /* The B2 download finishes after startup in release builds. Retry loading
     * once the path appears, without blocking the UI thread at launch. */
    if (!a->loaded && app->anatomy_model_path[0] && find_model_path(app)) {
        if (load_model(a)) a->failed = false;
    }
    if (!anatomy_3d_available(a) || app->active_tab != APP_TAB_SYMPTOM) return;
    Ui *ui = &app->ui;
    float left_w = (float)ui->win_w * 0.5f;
    float body_y = 96.0f;
    float body_h = (float)ui->view_h - body_y - 18.0f;
    float body_w = body_h * 0.48f;
    if (body_w > left_w - 100.0f) body_w = left_w - 100.0f;
    float body_x = (left_w - body_w) * 0.5f;
    int tw = (int)body_w, th = (int)body_h;
    if (tw < 16 || th < 16) return;
    float mx = ui->mx, my = ui->my - ui->origin_y;
    bool inside = mx >= body_x && mx <= body_x + body_w && my >= body_y &&
                  my <= body_y + body_h;
    if (inside && ui->mouse_pressed) {
        int side = mx < body_x + body_w * 0.5f ? 0 : 1;
        a->selected = a->selected == side ? -1 : side;
    }
    if (inside) {
        if (ui->mouse_down) {
            if (!a->dragging) {
                a->dragging = true;
                a->last_x = mx;
                a->last_y = my;
            } else {
                float dx = mx - a->last_x, dy = my - a->last_y;
                a->yaw += dx * 0.35f;
                a->pitch += dy * 0.30f;
                if (a->pitch > 60) a->pitch = 60;
                if (a->pitch < -60) a->pitch = -60;
                a->last_x = mx;
                a->last_y = my;
            }
        } else a->dragging = false;
        if (ui->wheel != 0) {
            a->distance -= ui->wheel * 0.16f;
            if (a->distance < 2.4f) a->distance = 2.4f;
            if (a->distance > 7.0f) a->distance = 7.0f;
            ui->wheel = 0;
        }
    } else a->dragging = false;
    if (!ensure_target(a, tw, th)) return;

    float rad_yaw = a->yaw * 3.14159265f / 180.0f;
    float rad_pitch = a->pitch * 3.14159265f / 180.0f;
    float eye[3] = {sinf(rad_yaw) * cosf(rad_pitch) * a->distance,
                    sinf(rad_pitch) * a->distance,
                    cosf(rad_yaw) * cosf(rad_pitch) * a->distance};
    float center[3] = {0, 0, 0}, up[3] = {0, 1, 0};
    float proj[16], view[16], mvp[16];
    mat_perspective(proj, 45.0f * 3.14159265f / 180.0f, (float)tw / (float)th, 0.1f, 20.0f);
    mat_look_at(view, eye, center, up);
    mat_mul(mvp, proj, view);
    Uniforms u;
    memcpy(u.mvp, mvp, sizeof(mvp));
    u.light[0] = 0.45f; u.light[1] = 0.8f; u.light[2] = 0.4f; u.light[3] = 0;

    SDL_GPUDevice *gpu = gpu_renderer_device(a->r);
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(gpu);
    if (!cmd) return;
    SDL_GPUColorTargetInfo ct = {0};
    ct.texture = gpu_texture_raw(a->color);
    ct.clear_color = (SDL_FColor){0.028f, 0.038f, 0.048f, 1.0f};
    ct.load_op = SDL_GPU_LOADOP_CLEAR;
    ct.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPUDepthStencilTargetInfo dt = {0};
    dt.texture = a->depth;
    dt.clear_depth = 1.0f;
    dt.load_op = SDL_GPU_LOADOP_CLEAR;
    dt.store_op = SDL_GPU_STOREOP_STORE;
    SDL_PushGPUVertexUniformData(cmd, 0, &u, sizeof(u));
    SDL_PushGPUFragmentUniformData(cmd, 0, &u, sizeof(u));
    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &ct, 1, &dt);
    SDL_BindGPUGraphicsPipeline(pass, a->pipeline);
    SDL_GPUBufferBinding vb = {a->vbo, 0};
    SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);
    SDL_GPUBufferBinding ib = {a->ibo, 0};
    SDL_BindGPUIndexBuffer(pass, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
    SDL_GPUTextureSamplerBinding ts = {gpu_texture_raw(a->texture), a->sampler};
    SDL_BindGPUFragmentSamplers(pass, 0, &ts, 1);
    for (int p = 0; p < 2; p++) {
        if (a->index_count[p] > 0)
            SDL_DrawGPUIndexedPrimitives(pass, a->index_count[p], 1, a->index_offset[p], 0, 0);
    }
    SDL_EndGPURenderPass(pass);
    SDL_SubmitGPUCommandBuffer(cmd);
}
