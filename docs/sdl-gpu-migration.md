# SDL_GPU UI migration

The app currently uses `SDL_Renderer` for all 2D UI drawing. The anatomy viewport
needs a real 3D pipeline, and the OpenGL/SDL_Renderer hybrid is not reliable.
The agreed architecture is a full migration to `SDL_GPU`.

## Backends

`SDL_GPU` selects the native backend:

- macOS: Metal
- Windows: Vulkan or D3D12
- Linux: Vulkan

No OpenGL context is required after the migration.

## Migration phases

1. `feature/sdl-gpu-foundation`
   - create the GPU device and claim the window
   - acquire and submit command buffers
   - swapchain resize
   - shader loading/toolchain
   - screenshot readback
2. `feature/sdl-gpu-ui`
   - shader pipeline for textured/colored quads
   - batch rectangles, lines, circles, rounded shapes, text and images
   - clipping/scissor and color/alpha modulation
   - preserve all current screens and modal states
3. `feature/sdl-gpu-3d`
   - depth-enabled model pipeline
   - 4K textures, lighting, male/female side-by-side
   - orbit/zoom camera
   - adaptive LOD
4. `feature/sdl-gpu-integration`
   - connect the 3D viewport to the Symptom Report tab and cards

## Shader toolchain

SDL_GPU consumes backend-native shader formats. We need one of:

- SDL_shadercross to compile shaders at build/runtime, or
- precompiled SPIR-V, DXIL and MSL shader bundles per platform.

The shader toolchain and its CI dependencies must be decided before the UI port.

## Non-goals

- Keeping SDL_Renderer as a fallback renderer; the migration must not run two
  presentation paths.
- Native child windows. SDL_GPU replaces both the UI and 3D rendering.

## Risks

- Visual parity of text and rounded UI surfaces.
- Shader compilation on three platforms.
- GPU texture memory for the 4K anatomy atlas.
- Screenshot/UI-regression support without `SDL_RenderReadPixels`.
- Migration size: this is a renderer rewrite, not a widget change.

## Foundation status

`tools/gpu_smoke.c` is the first foundation milestone. It:

- creates an SDL_GPU device;
- selects Metal on macOS, Vulkan on Linux, and Vulkan/DXIL on Windows;
- claims the window;
- acquires a command buffer and swapchain texture;
- clears the swapchain and presents.

On the development machine it reports:

```text
GPU driver: metal
Shader formats: 0x30
```

The smoke target is built as `gpu_smoke` and does not replace the existing
SDL_Renderer UI yet.

SDL_shadercross discovery is wired as an optional CMake package. The next
foundation step is to build/install SDL_shadercross and add the first real UI
quad pipeline; until then the app continues to use its current renderer.

## Graphics pipeline milestone

`tools/gpu_ui_smoke.c` adds the first real SDL_GPU graphics pipeline:

- MSL vertex/fragment shaders on macOS;
- vertex buffer + transfer-buffer upload;
- graphics pipeline creation;
- bind and draw six vertices;
- present a colored quad.

It is a standalone validation target (`gpu_ui_smoke`) and does not replace the
application UI yet. The next step is to build the general UI batcher and
texture/text primitives on top of this pipeline.

## UI batcher milestone

`tools/gpu_ui_batch.c` adds the first reusable batcher shape:

- dynamic vertex batch for colored rectangles;
- one transfer-buffer upload per frame;
- one pipeline bind and one draw call for the batch;
- mock frame with tab bar, left/right panels and button rectangles.

The next batcher extension is a texture atlas and font glyph quads. After that,
the real `ui.c` primitives can be mapped onto this backend.

## Atlas milestone

`tools/gpu_atlas_smoke.c` validates the atlas path:

- creates a GPU texture from CPU RGBA pixels;
- uploads it through a transfer buffer and copy pass;
- creates a sampler;
- binds the texture/sampler to a textured-quad pipeline;
- draws a UV-mapped quad.

This is the substrate for shape masks and FreeType glyphs. The next step is a
single persistent atlas with eviction, then mapping the real `ui.c` primitives
onto it.

## Glyph atlas milestone

`tools/gpu_text_smoke.c` adds a single R8 glyph atlas:

- rasterizes the UI font through FreeType into one 1024x1024 atlas;
- uploads the atlas through a GPU transfer buffer;
- draws glyph quads from the atlas through the textured pipeline;
- uses the atlas alpha with per-vertex color.

This is the text layer the real UI port will use. Remaining steps are the
persistent atlas/eviction policy and replacing `ui.c`'s per-string textures.

## UI port branch

`feature/sdl-gpu-ui` has started. It adds `src/gpu_ui.c/h`:

- GPU UI frame lifecycle;
- batched solid rectangle pass;
- `gpu_frame_smoke` validation target.

This branch is intentionally incomplete and is not merged. The application
still uses SDL_Renderer; text, clipping, textures and the real screens must be
ported before the renderer switch.

### GPU UI texture path

`feature/sdl-gpu-ui` now uses a texture-sampling pipeline and a 1x1 white base
texture for solid fills. Glyph and shape quads will bind the persistent atlas
through the same path.
