# SDL_GPU port plan

Porting the renderer from raw GL/GLES to SDL_GPU, keeping the `bgfx/bgfx.h`
shim as the seam. The 19 files under `visage_graphics/` that include it need
**zero edits** — this is a second implementation of a 311-line header, not a
rewrite of visage.

---

## What the codebase survey found

Five things make this much cheaper than a Vulkan port normally is. All were
verified against the tree, not assumed.

**1. The Y-origin flip is already parameterised.** `Caps::originBottomLeft`
feeds `u_origin_flip`, consumed in `layer.cpp:192`, `path.cpp:679,806`,
`shape_batcher.cpp:118` and four shaders. bgfx supported D3D, so visage was
written for both conventions from the start. Setting `caps.originBottomLeft =
false` is the entire change. **Do not touch the shader math.**

**2. There are only 13 uniforms in the whole renderer** — 11 `vec4` plus
`s_texture` and `s_gradient`. That is **176 bytes**, far below SDL_GPU's
uniform-push limit. There is no need for per-shader uniform packing: define one
std140 block containing all 11 vec4s, push the whole thing every draw, let
unused members go unread.

**3. All samplers are fragment-only.** No `vs_*.glsl` declares a `sampler2D`.
Every vertex shader is `num_samplers = 0`.

**4. Only ~10 distinct pipeline states.** `blendModeValue()`
(`shape_batcher.cpp:38-67`) has 8 compile-time cases; plus the post-effect mask
state and the default. With ~20 programs the pipeline cache key is
`(program, blend state, target format)` and realistically holds a few dozen
entries. Populate it lazily.

**5. Only 6 vertex layouts** — `UvVertex`, `PathVertex`, `ShapeVertex`,
`ComplexShapeVertex`, `TextureVertex`, `PostEffectVertex`.

Also free: `BGRA8` currently needs a GL texture swizzle, but SDL_GPU has
`B8G8R8A8_UNORM` natively (colour emoji path gets simpler); `readPixelsTopDown`
disappears because SDL_GPU is already top-down; conservative raster is already
unimplemented in the GL backend, so it is not a regression.

---

## The one genuine restructure: `submit()` stops drawing

This is the only part that is not mechanical, and it is the thing to get right
first.

Today `submit()` executes immediately — binds the FBO, uploads transient
vertex/index data, draws. SDL_GPU forbids that shape: **all buffer uploads must
happen in a copy pass before any render pass that reads them.** (Dear ImGui hits
the same wall — hence its mandatory `ImGui_ImplSDLGPU3_PrepareDrawData()`
before the render pass.)

`submit()` becomes record-only:

- Append vertex/index bytes to a growable per-frame CPU arena; keep the offsets.
- Push a `DrawCommand { program, state, target, viewport, uniform block
  snapshot, texture bindings, vertex offset/size, index offset/count, index32 }`.
- Snapshot the full 176-byte uniform block per command. Uniforms currently
  persist across submits and post-effects rely on that — snapshotting preserves
  the semantics exactly, and at 176 bytes the cost is irrelevant.

`frame()` then replays:

- One copy pass uploading the whole arena (`cycle = true` on the first upload
  each frame).
- Walk the commands; open a new render pass whenever the target framebuffer
  changes; bind the cached pipeline, push uniforms to vertex slot 0 **and**
  fragment slot 0, bind fragment samplers, draw.

**Render passes must use `SDL_GPU_LOADOP_LOAD`, never `CLEAR`.** Window
framebuffers are persistent offscreen textures and dirty-region redraw depends
on untouched pixels surviving. A `CLEAR` here produces flickering that looks
like a compositing bug and is not.

`blit()` becomes a recorded command that forces a pass break (copy passes and
render passes cannot nest). `readTexture()` / `requestScreenShot()` need a
submit plus fence wait — let them flush early; they are test and screenshot
paths, not hot.

---

## Decisions taken

Reversible, but the plan below assumes them.

**Keep the GL backend alive**, selected by a CMake option. It is the reference
for the pixel tests — when an SDL_GPU test fails you want to diff against a
known-good backend rather than guess. It also keeps the Pi fallback if v3dv
disappoints. Cost is near zero since the header is already the seam. Revisit
after Phase 5.

**Ship SPIR-V + MSL only, skip DXIL.** Windows then runs the *same Vulkan
backend as the Pi*, so the dev machine exercises the product's code path
instead of a D3D12 path nothing ships on. macOS needs MSL regardless.

**Generalise the three GL-specific header extensions**: `initGlBackend()` →
`initBackend()`, `Renderer::initialize(getProcAddress)` needs an
`SDL_GPUDevice*` variant, and `RendererType` should report
`SDL_GetGPUDeviceDriver()`.

---

## Phase 0 — de-risk before writing any renderer code

Build Dear ImGui's `example_sdl3_sdlgpu3` on the Pi under your headless fix.

- [ ] `SDL_CreateGPUDevice()` succeeds on v3dv
- [ ] Record what `SDL_GetGPUShaderFormats()` reports
- [ ] It renders and stays up

If the device fails to create where raw Vulkan succeeds, **stop** — SDL_GPU is
hiding a knob your headless fix needs, and that changes the whole decision.

---

## Phase 1 — shader toolchain — **DONE**

`tools/shadertool/` (behind `-DVISAGE_BUILD_SHADERTOOL=ON`, target `shaders`).
Lifted from xlib: glslang GLSL→SPIR-V, SDL_shadercross SPIR-V→MSL.

**The `.glsl` sources were not modified.** Everything Vulkan needs — explicit
locations, descriptor sets, the std140 block — is injected by the tool, so one
shader source still feeds both backends. That is what makes "keep the GL
backend" cheap rather than a maintenance tax.

Output is one extension-less blob per shader in `shaders/compiled/`, so the
embedded symbol names (`vs_color`, `fs_color`) that `ShaderCache` and
`ProgramCache` key on are unchanged. Container: `"VSGS"` magic, version, stage,
`num_samplers`, `num_uniform_buffers`, the uniform block's member names in
order, then SPIR-V (4-byte aligned) and MSL. Blobs are committed (~500 KB); only
someone editing a `.glsl` needs glslang.

Counts come from `SDL_ShaderCross_ReflectGraphicsSPIRV`, not from parsing —
glslang drops a uniform block nothing reads, so assuming would be wrong.

- [x] 48/48 compile (39 library + 9 example), SPIR-V **and** MSL
- [x] Varying locations globally consistent — every `v_*` name maps to exactly
      one location across all 48, so any vs/fs pair links, including a
      live-edited stage against a library stage
- [x] Attribute locations match the bgfx `Attrib` enum exactly
- [x] GL build unaffected (clean Release build, blobs live in a subdirectory the
      `*.glsl` globs don't reach)

### Tables Phase 2 needs

Fixed in `main.cpp`; a shader using a name outside them fails the build loudly
rather than mislinking.

- **Attributes** — `a_position` 0, `a_color0` 1, `a_color1` 2, `a_color2` 3,
  `a_texcoord0..3` 4–7.
- **Varyings** — `v_coordinates` 0, `v_dimensions` 1, `v_gradient_pos` 2,
  `v_gradient_pos2` 3, `v_gradient_texture_pos` 4, `v_position` 5,
  `v_shader_values` 6, `v_shader_values1` 7, `v_texture_uv` 8.
- **Samplers** — binding is declaration order within each shader. This already
  matches the existing `setTexture` stage arguments (`s_gradient` 0 /
  `s_texture` 1 where both appear, `s_texture` 0 where it is alone), so
  **no C++ change is needed for sampler binding.**
- **Uniforms** — per-stage, per-shader block; `shader_utils`' two members come
  first, then the shader's own. The blob carries the member order, so the
  backend packs from the existing name-keyed map without a global layout.

### Third-party shaders (blob v2)

The tool also accepts fragment shaders written to **stock Shadertoy** — one
convention, not several — see `examples/shaders/thirdparty/README.md`. Defining
`mainImage` marks a file as an effect; no library shader does, so the check is a
single unambiguous substring and an author cannot trip it accidentally.

Shadertoy was chosen over xlib's `render(uv)` and glslViewer because its corpus
is the only one large enough that a third party needs no visage documentation.
Shadertoy's audio `iChannel0` layout (512×2, spectrum row + waveform row) is
already identical to xlib's `u_tex0`, so xlib's three shaders port by rewriting
their entry point — and `shadertoy_port_example.frag` stops needing a port at
all.

Only `iResolution`, `iTime` and `iChannel0` are provided; every other Shadertoy
input fails to compile with an undeclared-identifier error, because nothing
populates them and silence would be worse than a build failure.

Two consequences for Phase 2:

- **The backend must bind samplers by name, not by slot.** Slot 0 is the
  gradient atlas for a library shader quad but the audio texture for a
  third-party effect. Blob v2 carries the sampler names in binding order for
  exactly this reason.
- Declared samplers must be contiguous from 0, so the tool errors if glslang
  drops an unused one rather than letting bindings shift silently.

Still open, and blocking audio-reactive effects: nothing binds a caller-supplied
texture yet. `submitShader` (`shape_batcher.cpp:358`) binds only `s_gradient`,
and `Shader` has no `setUniformValue` the way `ShaderPostEffect` does. Both live
above the shim, so they can be built against the GL backend now and carry over.

---

## Phase 2 — the backend, headless first — **DONE (offscreen)**

- [x] 475/475 pixel assertions on Windows/Vulkan, and **229/229 ctest on both
      backends** from one tree

`visage_graphics/gpu/bgfx_sdlgpu.cpp`, selected with `-DVISAGE_SDL_GPU=ON`. The
shim header moved to `backend/include/bgfx/bgfx.h` so both backends implement
one file rather than a copy each, and `embedded.cmake` embeds
`shaders/compiled/*` instead of `shaders/*.glsl` — stems match, so every symbol
`ShaderCache` and `ProgramCache` key on is unchanged.

Three things the tests caught that this plan had not anticipated:

- **`setVertexBuffer(VertexBufferHandle)` carried no layout.** The GL backend
  keeps it on the buffer; this one dropped it. Pipelines are keyed on layout, so
  persistent-buffer draws reached pipeline creation with a dangling pointer.
- **Readback has to flush.** The plan said so and the first implementation did
  not, so `requestScreenShot` read a texture whose draws were still queued.
- **The headless screenshot path goes through `bgfx::blit`**, gated on
  `TEXTURE_BLIT` *and* `TEXTURE_READ_BACK`. Advertising only the latter skipped
  the capture silently and left a 0x0 screenshot, which surfaced as a segfault
  in `Screenshot::sample` rather than a failed assertion.

Both remaining stubs were Phase 3's: `presentFrameBuffer` now draws into a
claimed window's swapchain, and `reset` stays empty — swapchains are claimed
with vsync and SDL recreates them on resize, so no flag is left to act on.

### Original plan for this phase

- Record-only `submit()` and the copy-pass/render-pass frame structure above.
- Pipeline cache on `(program, blend state, target format)`.
- `SDL_GPUTextureSupportsFormat` replaces the `EXT_color_buffer_float` probe for
  the R16F path-atlas fallback.
- Port the Catch2 bootstrap in `tests/gl_test_context.cpp` to create a device
  with no window.

Target `VisageGraphicsTests` (475 pixel assertions) **before any windowing
exists**. Offscreen-only is the fastest, highest-signal loop you will get, and
it isolates renderer bugs from swapchain bugs.

- [ ] 475/475 pixel assertions pass on Windows/Vulkan

---

## Phase 3 — windowing and present — **DONE**

- [x] `VisageIntegrationTests` 694 assertions, and 229/229 ctest, on both
      backends from one tree
- [x] 12/12 examples run and match the GL backend on Windows — 7 are
      pixel-identical, the rest differ only by animation phase
- [x] `Paths`, `Bloom`, `PostEffects`, `BlendModes` given extra scrutiny
- [x] Window resize (swapchain recreation), graceful close (swapchain release)
      and two simultaneous swapchains on one device

`WindowSdl3` no longer creates a GL context under `-DVISAGE_SDL_GPU=ON`; it
claims the window against `Renderer::gpuDevice()` and `swapBuffers()` becomes a
no-op, because submitting the command buffer that acquired the swapchain
texture *is* the swap. `VISAGE_SDL_GPU` moved to a directory-wide definition —
the windowing layer needs it too, and only the graphics target had it.

Claiming is deferred to the first `makeContextCurrent()` rather than done in
`initialize()`: the renderer builds its device *from* the first window, so no
device exists while that window is starting. Releasing is explicit in
`close()` — SDL_GPU only watches `WINDOW_PIXEL_SIZE_CHANGED`, so a swapchain
would otherwise outlive its surface.

Presenting reuses `vs_full_screen_texture` + `fs_sample` unchanged: that pair
already takes `(x, y, u, v)` packed in one vec4 and does a plain texture read,
so there is no present-only shader. All four rotations sit in one static vertex
buffer, so a present is a draw with no upload.

### The rotation ring shift does carry over — but V does not

`SDL_SetGPUViewport` submits a **negative-height viewport**
(`SDL_gpu_vulkan.c:7489`, "Viewport flip for consistency with other backends"),
so SDL_GPU's clip space is y-up exactly like GL and `NDC (-1,-1)` is the
bottom-left of the destination on both. The screen corners are identical.

What differs is the *source*: SDL_GPU textures are top-down. Inverting the
ring's V makes ring *i* denote the same **image** corner as it does on GL,
and then the permutation and its direction are unchanged. Flipping V without
also re-deriving the direction would have silently run rotation
counter-clockwise.

### Three things the examples caught

- **Only the library shaders had been switched to blobs.** `examples/` still
  embedded raw `.glsl`, which the gpu backend rejects, so every example using
  a custom shader rendered black. `ShaderTexture` was entirely blank and
  `LiveShaderEditing` lost its preview pane.
- **`PostEffects` crashes on a resize to a portrait window** — a single
  `SetWindowPos` to 500x800 is enough, and it reproduces identically on the
  **gl** backend (`0xC0000005`, sometimes `0xC0000374` heap corruption). Not a
  port regression; `Bloom` survives the same resize, so it is not the shared
  downsample chain. Untriaged.
- **`ShaderTexture` never animated, on either backend.** `ShaderQuad::draw`
  re-invalidates itself, but the texture and tints are written from the
  application's `onDraw`, which nothing re-invalidated — so the shader re-ran
  every frame on a frozen signal. Fixed in the example.

### Decisions taken

**Live shader editing is a gl-backend feature.** `LiveShaderEditing` is
excluded from `VISAGE_SDL_GPU` builds rather than linking glslang at runtime:
it is a development tool, the gl backend is staying alive as the pixel-test
reference anyway, and the Pi does not carry a GLSL compiler for it. The
rendering path itself works — only `ShaderCache::swapShader` on GLSL text
cannot.

`AudioVisualizer` is excluded from `VISAGE_SDL_GPU` builds too; it drives GL
compute directly and is ported in phase 4.

---

## Phase 4 — compositing and the visualizer

- The `onDrawBackground` composite path, now with app and UI sharing one
  `SDL_GPUDevice` — the app renders into the swapchain texture in an earlier
  pass, visage composites over it with `LOADOP_LOAD`. The present pipeline
  already takes `LOADOP_LOAD` when `blend` is set; what is missing is the
  application's earlier pass.
- Port `examples/AudioVisualizer` to use xlib's `fft_r2c.comp` /
  `fft_complex.comp` directly instead of the GL translation. It is excluded
  from `VISAGE_SDL_GPU` builds until then — drop it from `SKIPPED_EXAMPLES` in
  `examples/CMakeLists.txt` to bring it back.
- **macOS gains compute.** The permanent CPU-only fallback assumption from the
  GL plan is void — Metal has compute. Update the self-check status line
  accordingly.

---

## Phase 5 — Raspberry Pi 4

Re-run `PI4_BRINGUP.md` against the SDL_GPU build. Stages 3, 4 and 7 carry over
unchanged; stage 2's GL version logging becomes `SDL_GetGPUDeviceDriver()` plus
the device's supported formats. Stage 5's `LiveShaderEditing` row no longer
applies — that example is gl-only now.

### Comparing the two backends

`VisageBenchmark` (`examples/Benchmark/`) renders headless, so it needs no
display and can be driven over SSH, and it measures the renderer rather than
the compositor or the panel's refresh rate. Build both trees and diff the
`--csv` output.

**SDL_GPU has no timestamp queries** — `SDL_QueryGPUFence` is the only
query-shaped call in the whole header — so true GPU time is not obtainable
through the shim, and nothing here pretends otherwise. Instead a batch of
frames is submitted and drained by one readback, whose fence wait puts GPU
execution inside wall time. `blocked` (wall minus *this thread's* CPU) is a
lower bound on GPU-bound time, not a measurement of it.

Two traps this design avoids, both of which make naive measurements useless:
vsync pins any windowed comparison to the panel refresh, and `GALLIUM_HUD`
instruments the gl path only, because v3d is Gallium and v3dv is not.

Watch `cpu`, not just `wall`: it sums every thread, so a driver with workers
can exceed wall time, and on a 4-core Pi that is the number with a ceiling.

---

## Risks, ranked

| Risk | Where it bites | Mitigation |
|---|---|---|
| SDL_GPU device creation fails on your headless Pi setup | Kills the port | Phase 0, before any code |
| Pass structure wrong → dirty-region flicker | Phase 3, looks like a compositing bug | `LOADOP_LOAD` everywhere; test with a partial-redraw example |
| v3dv miscompiles a shader the desktop accepts | Phase 5 | Pixel tests are the net — that is what the 1169 assertions are for |
| Uniform block std140 padding mismatch | Phase 2, silently wrong pixels | All 11 members are `vec4`, so padding is trivially correct — do not add a `float` member later |
| ~~shadercross runtime dependency on Pi~~ | Phase 3 | Resolved: live editing is gl-only, nothing new is linked |
