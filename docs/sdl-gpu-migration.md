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
