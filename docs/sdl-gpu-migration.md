# SDL_GPU UI migration

The app now presents through `SDL_GPU`; no `SDL_Renderer` or `SDL_Texture`
remains. The anatomy viewport will use the same GPU backend.

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
SDL_GPU UI.

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

## UI port

The complete UI is ported to `src/gpu_renderer.c`:

- one 4096x4096 RGBA atlas for text and shape tiles;
- dynamic batched vertex uploads;
- clip-aware scissor ranges;
- 1x1 white texture for solid fills;
- camera/image GPU textures;
- GPU swapchain screenshot readback, normalized to 1440x900;
- GLSL -> SPIR-V (Metal/Vulkan) and SPIR-V -> MSL (Metal) shader artifacts.

No SDL_Renderer calls remain in `src/`.


## 3D anatomy viewport

The symptom viewport renders the GLB through a dedicated offscreen color+depth
pass and composites the result through the normal GPU UI pipeline.

- cgltf GLB loading with node transform, normal and UV decoding.
- One VBO/IBO for the two male/female primitives, offset side by side.
- D32 depth target and depth-tested pipeline.
- 4K diffuse texture with generated mipmaps.
- Orbit drag and wheel zoom inside the body viewport.
- Female/Male selection by clicking the left/right half; labels reflect selection.
- Lazy retry when the B2 anatomy download completes after startup.
