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
- **`PostEffects` crashed on a resize narrower than 600px** — a heap overflow
  in `submitText`, on the **gl** backend too, so it predates the port. Fixed;
  see below.
- **`ShaderTexture` never animated, on either backend.** `ShaderQuad::draw`
  re-invalidates itself, but the texture and tints are written from the
  application's `onDraw`, which nothing re-invalidated — so the shader re-ran
  every frame on a frozen signal. Fixed in the example.

### The text overflow the resize test found

`numTextPieces` sizes the vertex buffer from the glyphs that overlap an
invalid rect, and the write loop advances `vertex_index` for exactly those —
but it handed `setVertexGradientPositions` the block's *whole* glyph count. Any
partially clipped text block therefore wrote past the allocation.

`PostEffects` reaches it because its own `resized()` puts the 300px backdrop
frame at `x = (width - 600) / 4`, so a client narrower than 600px pushes it off
the left edge and clips the labels behind it. Narrow, not portrait: 900x300
leaves `y` negative and does not crash, and the boundary is sharp at 600.

Nothing asserted, because `VISAGE_ASSERT(vertex_index == total_length *
kVerticesPerQuad)` at the end of `submitText` checks the counter, which was
always right — the overwrite came from a separate length argument. ASan named
it in one run.

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

## Phase 4 — compositing and the visualizer — **DONE**

- [x] `AudioVisualizer` runs on **both** backends from one tree, compositing
      under the UI, and its compute FFT matches the CPU one to `0.00000` on
      Vulkan and on GL
- [x] 229/229 ctest on both backends, and the other examples unaffected by the
      present-path change

### The application borrows visage's command buffer

A swapchain texture is acquired against one command buffer and presented when
*that* buffer is submitted, so there is no way to hand an application a second
target: it has to draw onto the same one. `bgfx::acquireWindowTarget()` does
the acquire and `presentFrameBuffer()` reuses whatever it handed out, so the
frame is

    drawWindow()          UI into the layer framebuffers, flushed
    onDrawBackground()    acquireWindowTarget() -> app records its passes
    present()             composite pass, LOADOP_LOAD, submits

and the app's pass lands after the UI's own draws with no coordination beyond
submission order. Reaching it from application code is
`ApplicationEditor::windowRenderTarget()`, which returns
`visage::WindowRenderTarget` — the command buffer and texture on SDL_GPU, and
on OpenGL just the drawable size, because there the application binds the
default framebuffer itself. That is what lets one example drive both.

`Renderer` grew `createGpuShader` / `createComputePipeline` (plus
`lastShaderError`) so an application can build its own pipelines from
shadertool blobs without the shim leaking out of `visage_graphics`.

### The MSL entrypoint was wrong for every shader

`createShader` passed `"main"`, but SPIRV-Cross renames the entry point to
`main0` on the way to MSL — so **every** shader would have failed to create on
macOS, not just the new compute ones. Found while adding compute, fixed for all
stages. macOS is still untested hardware; this was a build-time certainty, not
a measurement.

That is also the "macOS gains compute" item: the status line no longer reasons
about GL versions at all on this backend. It reports what the device says
(`SDL_GPUTextureSupportsFormat` for RG32F storage writes) and, on failure, the
driver's own message.

### shadertool: verbatim shaders and blob v3

`.vert` / `.frag` / `.comp` are now compiled with no preamble, no injected
locations and no uniform block — they are already Vulkan GLSL and declare their
own sets and bindings. An application driving SDL_GPU owns its descriptor
layout and would only be fighting the injection; `.glsl` is unchanged.

Blob v3 replaces the `is_fragment` flag with a stage word and carries the
compute counts and workgroup size, all from `SDL_ShaderCross_ReflectComputeSPIRV`
rather than hardcoded the way xlib has to. Every blob was regenerated; the
graphics ones differ only by the header, and regeneration was verified
byte-identical under v2 first so the diff means what it says.

### One change to xlib's kernels

They use a symmetric Hann window (`/(N-1)`); visage's CPU path is periodic
(`/N`). Left alone, the self-check reports a disagreement that looks like a
miscompiled kernel and is not. Otherwise both kernels are xlib's verbatim.

The kernels want **512 invocations per workgroup**, well above the 128 Vulkan
guarantees — the first thing to suspect if v3dv refuses them in Phase 5. That
path degrades to the CPU FFT with the driver's message in the status line.

### Numbers on the dev machine (RTX 3080 Ti, Release)

| | CPU analysis | GPU analysis | R2C 512 | complex 1024 |
|---|---|---|---|---|
| SDL_GPU / Vulkan | 101.9 µs | 14.2 µs | 8.1 µs | 6.4 µs |
| OpenGL 4.3 | 167.7 µs | 19.3 µs | — | — |

The last two columns are fenced GPU time per transform, the only per-kernel
measurement SDL_GPU allows. **The packed real-to-complex kernel is the slower
of the two here**, which is the opposite of the argument for writing it: half
the transform, but only half the threads do butterflies for a channel and the
recovery step diverges. Whether that holds on v3d is a Phase 5 question —
it is exactly the kind of thing that inverts on a small GPU. (It does invert;
see Phase 5. These figures are from the 512-thread, 1024-point kernels that
predate the Pi, and are kept as the dev-machine reference.)

The two backends' CPU figures are not comparable to each other (different
builds, different texture layouts); each column is only meaningful against the
one beside it.

---

## Phase 5 — Raspberry Pi 4 — **DONE**

**Verdict: ship the gl backend.** For UI-shaped workloads SDL_GPU costs about
twice the CPU on v3d, measured in two independent codebases, and every other
consideration points the same way. Detail below.

### What had to be fixed before it ran at all

Four defects, none visible in a release build because `VISAGE_LOG` compiles out
under `NDEBUG`:

- **`SDL_CreateGPUDevice()` was silently selecting llvmpipe.** V3D reports
  `depthClamp = false` and SDL_GPU treats depthClamp as required, so it passed
  over the GPU and fell back to software rasterization - which also cannot
  present to a KMSDRM display. The device is now created through properties that
  opt out of the feature and require hardware acceleration. Nothing in the shim
  clamps depth. **Any measurement taken before this fix was llvmpipe**, including
  a first pass at stage 3 that produced a spurious fourth test failure.
- **A kmsdrm window needs `SDL_WINDOW_VULKAN`**, or SDL hands the display plane
  to GBM/EGL and the claim fails with "Vulkan can't find any displays".
- **A kmsdrm window must be the panel's exact mode.** `vkCreateDisplayModeKHR` is
  unsupported on VideoCore, so SDL can only build a surface on a mode that
  already exists, and no example requested one. The window now takes the
  display's mode and `updateClientSize()` derives the logical size from it,
  already swapping the axes on a quarter turn.
- **The compute kernels asked for 512 invocations per workgroup.** V3D allows
  256 - `maxComputeWorkGroupSize = [256,256,256]`, 16 subgroups of 16 QPU
  threads, architectural and not a driver setting. Phase 4 suspected this and
  guessed Vulkan's floor of 128; the real ceiling is 256, still half of what the
  kernels wanted. Both now use strided loops at 256.

The gl backend needed one fix of its own: every example **segfaulted on exit**,
because Mesa unloads the driver behind the GL dispatch table when SDL destroys
the last window, while the atlases and layer framebuffers hang off a Canvas that
outlives it. `setContextLost()` gates the backend's `destroy()` overloads.

### Comparing the two backends

Three UI workloads, all vsync-locked at 60Hz on a 480x800 panel, so **CPU load
is the metric** - wall time is pinned by the panel either way. Measured from
`utime+stime` deltas in `/proc/<pid>/stat` over 8 s of steady state:

| workload | gl | SDL_GPU | ratio |
|---|---|---|---|
| imgui demo, identical draw code both sides | 7.7% | 14.2% | 1.84x |
| `ExampleShowcase` | 16.1% | 32.3% | 2.00x |
| `ExampleAudioVisualizer`, GPU kernels | 19.3% | 28.8% | 1.49x |

The imgui row is the cleanest evidence in the exercise: the same draw code on
both of its backends, so the difference is renderer overhead alone and nothing to
do with how this shim is written. Both examples were rebuilt at 480x800 first -
as shipped they request different sizes, which would have measured window area
instead. The visualizer's smaller ratio is dilution: a large share of its cost is
the application's own full-screen shader and the FFT, identical on both sides.

**`VisageBenchmark` disagrees, and it is the benchmark that is wrong for this
question.** Its `shapes` scene has SDL_GPU 3x *cheaper* on CPU (7.8 ms vs 23.4),
because it throws thousands of individual primitives and gl's per-draw state
churn dominates. Real UIs batch into few draw calls and spend their time on fill,
text and glyphs, where SDL_GPU's fixed per-frame cost is pure loss - visible in
the same sweep as `gradients` 1.15 ms vs 2.64 and `text` 2.53 vs 4.52. The
synthetic scene measures the one regime a panel UI does not live in.

Everything else on the ledger also favours gl: runtime GLSL (`LiveShaderEditing`
builds), no `MESA: error: destroy dumb object` at teardown, and presentation
through GBM that does not care about the panel's exact mode.

### The compute FFT is a wash on a Pi 4, and its own timers mislead

The visualizer's microsecond readouts cannot be read as CPU cost. They come from
a wall clock, so they include time blocked on the GPU, and `measureBackends()`
measures something different again - 200 iterations back to back with nothing
drawn between them, so the queued GPU work drains after the timer stops. Only the
selected backend's figure is updated per frame, so the other is frozen at
whatever the last batch measured, which makes the two look comparable when they
are not.

Measured as process CPU time instead, three repeats per configuration:

| backend | analysis | live wall-clock | process CPU | fps |
|---|---|---|---|---|
| gl | GPU compute | ~200 µs | 23.4 / 24.0 / 23.5 % | 60.3 |
| gl | CPU | ~7100 µs | 24.0 / 24.0 / 23.8 % | 60.3 |
| SDL_GPU | GPU compute | ~150-220 µs | 32.3 / 32.3 / 31.9 % | 60.2 |
| SDL_GPU | CPU | ~290 µs | 23.0 / 31.8 / 31.8 % | 60.2 |

**The analysis choice makes no measurable difference to CPU on either backend**,
and the frame rate is 60 either way. The alarming 7 ms on gl is a stall rather
than work: `uploadRows()` calls `glTexSubImage2D` on the texture the full-screen
spectrum shader is sampling, so the driver blocks until the GPU releases it.
SDL_GPU's upload goes through a staging copy and does not block, which is why the
same code path costs ~290 µs there.

Note the first CPU-analysis run in each series reads low (18.2 % and 23.0 %) and
the repeats do not. A single unrepeated pair of those outliers is what first
suggested a difference; there is none.

The kernels themselves are correct - agreement is exact on both backends - so the
256-invocation rewrite is sound. `VISAGE_VIZ_BACKEND=cpu|gpu` selects the
analysis at startup, which is otherwise only reachable through a panel button.

The per-kernel question this plan left open is answered, and inverted. These are
fenced GPU-side measurements and stand:

```
kernel[0] R2C 512        92.6 us      <- faster on v3d
kernel[1] complex 1024  117.5 us
```

The packed kernel is 1.27x *faster* here, the opposite of the RTX figures above.
On v3d, halving the transform beats keeping 512 threads busy. The gl backend
cannot fence GPU time at all - GLES 3.1 does not require timer queries.

### Why 2048 is the size, and why it is also the ceiling

The transform is 2048 points per channel. Doubling it from 1024 is what makes
the compute path worth anything at all, and it is simultaneously the last step
this GPU can take:

| N | complex kernel (16N bytes) | packed R2C kernel (8N bytes) |
|---|---|---|
| 1024 | 16 KB | 8 KB |
| 2048 | **32 KB - exactly V3D's limit** | 16 KB |
| 4096 | 64 KB - impossible | **32 KB - exactly the limit** |
| 8192 | impossible | impossible |

`maxComputeSharedMemorySize` is 32768 on V3D. The complex kernel at 2048 sits on
that number exactly and was accepted; there is no headroom past it. Going further
would need a multi-pass FFT split across dispatches, which is a different piece
of work.

Measured effect of the doubling, three repeats, process CPU time:

| backend | analysis | @1024 | @2048 | per doubling |
|---|---|---|---|---|
| gl | GPU compute | 23.5 % | 23.8 % | **+0.2** |
| gl | CPU | 23.8 % | 25.8 % | **+1.9** |
| SDL_GPU | GPU compute | 32.3 % | 32.8 % | +0.5 |
| SDL_GPU | CPU | 31.8 % | 32.9 % | +1.1 |

CPU work grows with N log N while dispatch overhead stays flat, exactly as the
offload argument predicts - so at 2048 the compute path is finally ahead, by
about 2 points of one core on gl. Fenced GPU time grew sub-linearly with it:
R2C 92.6 -> 140.4 µs and complex 117.5 -> 184.9 µs, 1.5x for 2.2x the work.

**But 2 points of a 24 % total is still noise against a DSP budget, and the size
where it would genuinely matter is the size where the shared memory runs out.**
That is the durable argument against the compute FFT on this hardware: it cannot
grow into being clearly worthwhile. Keep the kernels, default to CPU analysis,
and revisit only on a GPU with more shared memory. Agreement stays exact
(0.00000) on both backends at 2048, so the kernels are correct at this size.

Each series had one low outlier, so the 2-point gap is only a few times the
noise band - consistent across both backends and with the mechanism, but not a
figure to quote to three significant digits.

### Two findings that outrank the backend choice

- **`paths` costs ~120 ms/frame on both backends**, GPU-bound and almost exactly
  linear in pixel area (126 ms at 800x480, 33 at 400x240, 9.5 at 200x120;
  `blocked` scales 4.16x and 4.42x per 4x pixel cut). One pass, one draw of
  ~1176 additively-blended triangles - so it is fill and overdraw, not pass
  overhead. The benchmark scene is adversarial by construction, but any
  full-screen path fill will hurt on v3d regardless of backend.
- **The renderer is single-threaded and scheduling-limited, not
  throughput-limited.** CPU per frame is flat across 0/4/8 competing threads
  while wall time grows. More GUI threads cannot help; reserve a core for audio.

### Original plan for this phase

Re-run `PI4_BRINGUP.md` against the SDL_GPU build. Stages 3 and 4 carry over
unchanged; stage 2's GL version logging becomes `SDL_GetGPUDeviceDriver()` plus
the device's supported formats. Stage 5's `LiveShaderEditing` row no longer
applies — that example is gl-only now. Stage 7 gains a kernel selector, and its
status line comes from the device rather than from a GL version.

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
