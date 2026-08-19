# Raspberry Pi 4 bring-up checklist

Both backends now run on a Pi 4. Results are recorded inline below; the backend
comparison and the verdict live in `SDLGPU_PORT.md`'s Phase 5.

Two things worth knowing before starting, both learned the hard way:

- **A release build says nothing when it fails.** `VISAGE_LOG` is compiled out
  under `NDEBUG`, so a failed `SDL_Init`, a failed window claim or a shader error
  is silent and the symptom arrives later as a black panel or a null dereference.
  Set **`VISAGE_RENDER_INFO=1`** to get the driver, the API version and the
  visualizer's own figures on stderr regardless of build type.
- **Everything can be driven over ssh.** No desktop is needed: the GL suites run
  under `SDL_VIDEODRIVER=offscreen` and the SDL_GPU ones need no window at all,
  so stages 1-3, 7 and 8 need nothing attached to the board.

**Two independent things are unverified, and they fail for unrelated reasons:**

- **A — does the GLES renderer work on Mesa/v3d?** (shader compilation, pixel
  correctness, float render targets, front-face convention)
- **B — does KMSDRM windowing work?** (display takeover, input, no desktop)

Verify **A under the Pi's desktop first**, then do B separately. If you go
straight to bare KMSDRM and something is wrong, you cannot tell which of the two
broke, and A is where the interesting failures live.

Stages 1–4 are A. Stage 5 is B. Stages 6–8 are product concerns.

---

## Stage 0 — packages, before you configure anything

> **This is the one step with a silent failure mode.** SDL3 is built from source
> by CPM and it *omits backends whose dev packages are missing at configure
> time* without erroring. If you configure first and install after, you get a
> build with no KMSDRM and no clue why. If you have already configured, **delete
> the build directory** — a reconfigure will not re-detect.

```bash
sudo apt update
sudo apt install -y \
  build-essential cmake ninja-build git pkg-config \
  libdrm-dev libgbm-dev libudev-dev libinput-dev libxkbcommon-dev \
  libasound2-dev libpulse-dev \
  libx11-dev libxext-dev libxrandr-dev libxi-dev libxcursor-dev libxfixes-dev
```

- `libdrm-dev libgbm-dev libudev-dev libinput-dev` → stage 5 (KMSDRM)
- `libx11-dev` and friends → stages 1–4 (running under the desktop)
- `libasound2-dev libpulse-dev` → stage 7 audio capture
- EGL/GLES headers are **not** needed — SDL3 vendors its own Khronos headers and
  dlopens `libEGL.so.1` / `libGLESv2.so.2` at runtime.

- [ ] Packages installed **before** any `cmake` run
- [ ] KMS driver active: `grep vc4-kms-v3d /boot/firmware/config.txt`
      (older images: `/boot/config.txt`). The **full** KMS driver, not `fkms`.
- [ ] User in the right groups: `sudo usermod -aG video,render $USER`, then log
      out and back in
- [ ] Network reachable to **both** `github.com` (SDL3 release asset) and
      `gitlab.freedesktop.org` (freetype is cloned at configure time)
- [ ] **SDL_GPU builds only:** `mesa-vulkan-drivers` for v3dv, plus
      `vulkan-tools` to check it with `vulkaninfo --summary` (expect
      `V3D 4.2.14.0`, `V3DV Mesa`). This is a *runtime* dependency - SDL vendors
      the Vulkan headers - so a missing driver does not show up at configure
      time. Absent from the list above on the board this was written on.

Two packaging traps this list creates:

- `libpulse-dev` pulls in **libpipewire without its config files or daemon**, so
  SDL's audio init used to log `pw.conf | can't load config client.conf` four
  times before falling through to ALSA. **Already handled**: the build now sets
  `VISAGE_LINUX_ALSA_ONLY=ON` by default, which builds SDL3 with ALSA as its only
  audio backend, so there is no pipewire to probe and nothing to set at runtime.
  `-DVISAGE_LINUX_ALSA_ONLY=OFF` in a fresh build directory restores the others.
- Installing the dev packages does **not** give you a capture device.
  `arecord -l` listed none on this board, so the visualizer runs on its test
  tone. That is not the bare-TTY fallback the stage 7 note describes; it is
  simply no capture hardware, and no amount of TTY switching changes it.

---

## Stage 1 — configure and build

`VISAGE_OPENGL_ES` defaults to **ON** on Linux, so a plain configure already
gives you the GLES build. Release, because Debug on a Pi 4 is painful.

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

- [x] Configure output's **`Video drivers:` line contains `kmsdrm`** *and*
      `x11` (or `wayland`). **If `kmsdrm` is absent, stop** — go back to stage 0,
      `rm -rf build`, and configure again.
- [x] `Audio drivers:` line contains `pulseaudio` or `alsa`
- [x] `-- VISAGE: Downloading SDL3 3.4.12` succeeded
- [x] Build completes with no errors

build warning:
/home/xiashj/visage/visage_widgets/text_editor.cpp:208:66: note: parameter passing for argument of type ‘std::pair<float, float>’ when C++17 is enabled changed to match C++14 in GCC 10.1

  208 |   std::pair<float, float> TextEditor::indexToPosition(int index) const {

---

## Stage 2 — first pixels, and which GLES version you actually get

Under the desktop, simplest example first:

```bash
./build/examples/ExampleBasic 2>&1 | tee /tmp/visage-gl.txt
```

Release builds need `VISAGE_RENDER_INFO=1` for the four lines below to appear at
all; `2>&1 | tee` alone gets you an empty file.

- [x] A window appears: dark blue background, cyan circle in the middle
- [x] Record these four log lines (they go to stderr):

```
GL_VENDOR: Broadcom
GL_RENDERER: V3D 4.2.14.0
GL_VERSION: OpenGL ES 3.1 Mesa 26.2.0-1~bpo13+0~rpt3
GL_SHADING_LANGUAGE_VERSION: OpenGL ES GLSL ES 3.10
```

**GLES 3.1, so no fallback happened and compute is available.** The SDL_GPU
build reports `Renderer: vulkan` with `Vulkan Device: V3D 4.2.14.0`; if it ever
says `llvmpipe` instead, the device selection regressed - see Phase 5.

Original expectation, for reference:

```
GL_VENDOR:  ...            expect Broadcom
GL_RENDERER: ...           expect V3D 4.2
GL_VERSION: ...            expect OpenGL ES 3.1
GL_SHADING_LANGUAGE_VERSION: ...
```

**Write down the GLES version — it decides stage 7.** The port now asks for a
GLES 3.1 context and falls back to 3.0, so:

- `OpenGL ES 3.1` → compute shaders available, GPU FFT can run
- `OpenGL ES 3.0` → fallback happened; the visualizer runs CPU-only *by design*,
  not as a bug

---

## Stage 3 — pixel correctness (this is the real port verification)

```bash
ctest --test-dir build --output-on-failure
```

- [ ] **229/229 pass.** This is the same count that passes on Windows desktop GL
      and on a real GLES context, so any failure here is a genuine Mesa/v3d
      divergence — the single most valuable thing this whole exercise can find.

>> 3 failures
          8 - Canvas visual validation (Failed)
          9 - Canvas advanced shape validation (Failed)
         44 - Degeneracies (Failed)

**Resolved: 229/229 on both backends.** The same three failed on the gl and
SDL_GPU backends, on exactly the same assertions, which is what placed the cause
below both APIs. They were never missing geometry - the samples read 250-254
where 255 was expected and 5-6 where 0 was, off by 1 to 5 LSBs:

```
canvas_tests.cpp:233: REQUIRE( bottom_edge.hexRed() == 0xff )   ->  254 == 255
path_tests.cpp:138:   REQUIRE( sample(10,10).hexRed() <= 1 )    ->    5 <= 1
```

V3D reports `subPixelPrecisionBits = 6` against the 8 of the desktop parts those
values were recorded on, so vertex positions snap to a grid four times coarser
and edge coverage rounds differently. Those three test cases now compare through
`channel()` in `visage_graphics/tests/pixel_tolerance.h`, a margin of 8. A fourth
failure - test 10, `Canvas state and position validation` - appears **only on
llvmpipe** and is not a v3d divergence.

No desktop needed: the gl suites run under `SDL_VIDEODRIVER=offscreen` (real v3d,
verified against `LIBGL_ALWAYS_SOFTWARE=1`, which is 8x slower) and the SDL_GPU
suites need no window.

To narrow a failure:

```bash
ctest --test-dir build -N                      # list test names
ctest --test-dir build -R "<name>" --output-on-failure
find build -name "VisageGraphicsTests*" -o -name "VisageIntegrationTests*"
```

Notes:
- `VisageUtilsTests` has **one pre-existing flaky** child-process timing test.
  If that is the only failure, ignore it.
- The GL-dependent suites bootstrap a hidden window + context via a Catch2
  listener. That works under the desktop. **Do not run ctest on bare KMSDRM** —
  there is no hidden window there.

---

## Stage 4 — examples sweep

Run each under the desktop. You are looking for *correct rendering*, not just
absence of crashes. The ones that probe specific risks:

| Example | What it would catch |
|---|---|
| `Paths` | Path fills depend on `glFrontFace(GL_CW)` plus `gl_FrontFacing`. **Mangled or inverted fills = front-face convention differs on v3d.** Highest-risk example. |
| `Bloom`, `PostEffects` | Float render targets. GLES needs `EXT_color_buffer_float`; without it the backend falls back to R8 and blurs go blocky or black. |
| `BlendModes` | The premultiplied-alpha blend paths (same machinery the new overlay compositing uses). |
| `Showcase` | Text, **colour emoji** (freetype + Twemoji on Linux), widgets, drag and drop. |
| `MultiWindow` | Two windows sharing one GL context. |
| `LiveShaderEditing` | Runtime GLSL compilation and the driver's error log. |
| `Gradients`, `Layout`, `MouseEvents`, `BringYourOwnWindow` | Baseline. |

- [x] Basic
- [x] BlendModes
- [x] Bloom
- [x] BringYourOwnWindow
- [x] Gradients
- [x] Layout
- [x] LiveShaderEditing
- [x] MouseEvents
- [x] MultiWindow
- [x] Paths
- [x] PostEffects
- [x] Showcase

(`ClapPlugin` is deliberately not built.)

---

## Stage 5 — KMSDRM, the product configuration

Only start this once stages 1–4 are clean, so any new failure is attributable to
windowing. You need a real TTY, not a terminal inside the desktop:

```bash
sudo systemctl isolate multi-user.target     # or: sudo systemctl stop lightdm
SDL_VIDEODRIVER=kmsdrm ./build/examples/ExampleBasic
```

- [x] Renders full-screen on the panel
- [x] Keyboard and mouse/touch reach the app
- [x] A few more examples run the same way (`Showcase`, `Paths`)
- [x] Exits cleanly and hands the console back

If it fails, force the driver explicitly (as above) rather than relying on
auto-detection, and turn on SDL's logging:

```bash
SDL_VIDEODRIVER=kmsdrm SDL_LOGGING=*=verbose ./build/examples/ExampleBasic
```

---

## Stage 6 — screen rotation

The panel is mounted rotated. **Nothing in the tree calls the rotation API yet**,
so you have to add one line to an example to exercise it — put this before
`show()`:

```cpp
visage::setScreenRotation(visage::ScreenRotation::Rotate90);
```

- [x] Rendering is rotated correctly (the present pass does it via per-corner UVs)
- [x] **Input lands where you press** — rotation transforms input coordinates too,
      and this is the half that is easy to get wrong
- [x] Test whichever of `Rotate90` / `Rotate180` / `Rotate270` matches the panel;
      test all four if the orientation is not settled

minor issue: the mouse cursor is not rotated properly

Not fixable in visage: SDL draws the cursor on a **hardware DRM cursor plane**
(`DRM_PLANE_TYPE_CURSOR`, its own GBM buffer), which the display controller
composites after everything visage produces, and SDL's kmsdrm cursor code has no
rotation handling at all. Position is still correct - the cursor sits on the
element it would click, because input goes through the same transform - so only
the glyph direction and the motion axes are wrong. Options: accept it (the
product is a touch panel with no cursor), hide the system cursor with
`visage::setCursorVisible(false)` and draw one as UI content, or use a
rotationally symmetric dot so orientation stops mattering (a hardware cursor is
padded into a 64x64 buffer here, so any size up to that works).

To make a connected mouse inert while keeping keyboard input:

```cpp
app.setIgnoresMouseEvents(true, false);  // false = do not pass to children
visage::setCursorVisible(false);
app.setAcceptsKeystrokes(true);
app.requestKeyboardFocus();              // nothing clicks to set focus for you
```

`Frame::frameAtPoint` then returns `nullptr` everywhere, so no hover, no clicks,
and nothing re-shows the cursor.
---

## Stage 7 — the GPU FFT visualizer

```bash
./build/examples/ExampleAudioVisualizer
```

**Read the status line under the title in the panel.** It is a self-check that
runs both FFT backends over one synthetic window and reports their largest
disagreement, so you get a number rather than an image that merely looks
plausible:

| Status line | Meaning |
|---|---|
| `compute FFT ready, matches CPU to 0.00000` | v3d's compute path agrees with the CPU FFT. Best case. |
| `compute FFT ready, matches CPU to 0.0xxxx` | **Report the number.** v3d miscompiles the kernel, or its shared-memory barriers behave differently from NVIDIA's. |
| `no compute shaders: GLES 3.0 (needs GLES 3.1)` | Context fell back; visualizer is CPU-only. Cross-check against stage 2. |
| `compute FFT ready (kernel check unavailable)` | R32F readback unsupported, so the check could not run. Kernel is probably fine but unproven. |
| `no compute FFT: ...` (SDL_GPU build) | The device's own reason. The kernels want 512 invocations per workgroup, which Vulkan only guarantees to 128, so v3dv refusing them is the first thing to suspect. |

- [x] Status line recorded
- [x] **Click "Measure both (200x)" and record the CPU and GPU microsecond
      figures.** This is the number that decides whether GPU FFT is worth
      keeping at all. Reference on the dev machine (RTX 3080 Ti): **167 µs CPU
      vs 19 µs GPU**. Expect the Pi's CPU figure to be far larger; the GPU figure
      is the one that matters.
- [x] Compositing works: the spectrum is visible *through* the translucent panel,
      and the area outside the panel shows the visualizer, not black
- [x] Frame rate holds at the panel's refresh rate
- [x] Toggle CPU ↔ GPU compute — the picture should look the same either way
      (the two backends fill the same texture)
- [x] SDL_GPU build only: toggle **R2C 512 ↔ complex 1024** and record both
      per-kernel figures. On the dev machine the packed kernel is the slower
      one; whether that survives on v3d is the open question.

**Do not read the panel's microsecond figures as CPU cost.** They come from
`microseconds()`, a wall clock, so they include time spent *blocked on the GPU*,
and `measureBackends()` measures something different again: it runs its 200
iterations back to back with nothing drawn between them, so the queued GPU work
drains after the timer stops and never appears in the average. The two numbers
shown side by side are also not comparable - `render()` only updates the figure
for the backend currently selected, so the other one is frozen at whatever the
last batch measured.

What that produces on the gl backend, and it looks alarming:

| analysis | live wall-clock figure | process CPU | fps |
|---|---|---|---|
| GPU compute | ~200 µs | 23.4 / 24.0 / 23.5 % | 60.3 |
| CPU | **~7100 µs** | 24.0 / 24.0 / 23.8 % | 60.3 |

The 7 ms is a **stall, not work**: `uploadRows()` calls `glTexSubImage2D` on the
very texture the full-screen spectrum shader is sampling, so the driver blocks
until the GPU releases it. Process CPU time is flat across the two, and so is
the frame rate. SDL_GPU shows ~150-290 µs on both paths because its upload goes
through a staging copy instead, and its process CPU is likewise flat -
31.8-32.3 % either way.

**So the compute FFT is a wash on a Pi 4 for this workload** - neither the win
the port plan hoped for nor a regression. Three repeats per configuration, after
a single unrepeated pair suggested otherwise. Agreement is exact (0.00000) on
both backends, so the 256-invocation kernels are correct either way.

Use `VISAGE_VIZ_BACKEND=cpu|gpu` to pick the analysis at startup, since it is
otherwise only reachable through a panel button.

Fenced per-kernel GPU time, SDL_GPU only, and these *are* GPU-side measurements:

```
kernel[0] R2C 512        92.6 us      <- faster on v3d
kernel[1] complex 1024  117.5 us
```

**The packed kernel is the faster of the two here, 1.27x** - the inverse of the
dev machine, where `SDLGPU_PORT.md` records it as slower and flags the inversion
as an open question. On v3d, halving the transform beats keeping 512 threads
busy. The gl backend reports `-1` because GLES 3.1 does not require timer
queries, so it cannot fence GPU time at all.

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

Audio note: on a bare TTY the recording device may not open, in which case the
example falls back to **"Test tone"** automatically. That is expected and the
visualizer still works; it does not indicate a problem.
minor issue: audio visualizer picture is not rotated properly

Fixed. The application draws through `windowRenderTarget()` straight into the
window surface, and only the composite layer passes through the rotating present
pass - so the UI turned and the spectrum did not. Both visualizer shaders now map
their own coordinates into logical space with `logicalUv()`, derived from the
present pass's corner table rather than guessed.

---

## Stage 8 — product-shape checks

- [x] Sustained frame rate over 10+ minutes (Pi 4 thermal throttling) —
      80,000 frames of the `shapes` scene held **8.133 ms/frame** against 8.286
      over 120 frames, so no decay. Temperature 53.5 → ~62 °C and flat,
      `get_throttled=0x0` throughout, ARM pinned at 1500 MHz and V3D at 500 MHz.
- [x] No memory growth over time — RSS flat at **95,420 kB** for ~17 minutes
      within one scene. Apparent jumps to 187/256/271 MB are scene *transitions*
      allocating their own resources (the path atlas, the blur targets), not a
      leak. Note `--scene NAME` is singular: `--scenes` and a bare positional
      are both ignored, and a run that silently does all five scenes takes hours
      because `paths` alone is ~120 ms/frame.
- [x] **Run it alongside your actual audio DSP load.** "CPU saved" only means
      something when the CPU is contended — this is the measurement that
      justifies or kills the compute FFT for the product. Stand-in: busy loops
      on 0/4/8 threads with the idle share of `/proc/stat` recorded per run, so
      saturation is verified rather than assumed (77% → 1% → 0% idle). **CPU per
      frame is flat across all loads** — gl `shapes` 23.4 → 23.7 → 24.4 ms —
      while wall time grows 26 → 37 → 59 ms. The renderer is scheduling-limited,
      not throughput-limited, so more GUI threads cannot help; reserve a core for
      audio instead. Beware: 3 busy threads on a 4-core box leaves the
      single-threaded renderer a core of its own and measures nothing.
- [ ] Startup time acceptable (the visualizer runs a 200-iteration benchmark at
      launch; if that hitch is too long on a Pi, drop the call to
      `measureBackends()` in `initializeVisualizer()`)

---

## Failure playbook

| Symptom | Likely cause | Where to look |
|---|---|---|
| **Every app fails with `No available video device`** | a previous run still holds DRM master. One leftover process makes every later app fail `SDL_Init`, and the symptom is a null dereference much later, not an error | `fuser -v /dev/dri/card1`. Note `pkill -x` silently matches **nothing** for names over 15 characters, so `ExamplePostEffects` survives a cleanup that appears to work |
| Nothing on stderr at all, ever | release build: `VISAGE_LOG` is compiled out under `NDEBUG` | run with `VISAGE_RENDER_INFO=1` |
| App runs, one core pinned, no window, no error | SDL_GPU window claim failing and being retried per frame | fixed; the claim is attempted once and reports both the window and display-mode sizes |
| `pw.conf can't load config client.conf` ×4 | libpipewire installed without configs or daemon; SDL probes it then falls back to ALSA | harmless; `SDL_AUDIO_DRIVER=alsa` |
| `MESA: error: destroy dumb object N: Invalid argument` | SDL's kmsdrm Vulkan swapchain and Mesa both release the same scanout buffer at exit. One per swapchain image, SDL_GPU only, after the last frame | upstream; not fixable here. imgui's own SDL_GPU example does it too |
| Segfault on exit, gl backend | GPU resources outliving the context: Mesa unloads the driver with the last window | fixed by `setContextLost()`; see Phase 5 |
| `kmsdrm` missing from configure output | dev packages installed after configuring | stage 0, then `rm -rf build` |
| Path fills mangled / inverted | front-face convention differs on v3d | `glFrontFace(GL_CW)` in `initGlBackend`, `fs_path_fill.glsl` |
| Blurs blocky or black | `EXT_color_buffer_float` absent → R16F falls back to R8 | `bgfx_gl.cpp` `r16f_renderable` probe |
| Shader compile errors at startup | v3d's GLSL ES compiler is stricter | error text comes straight from the driver log |
| Everything black outside the UI panel | transparent-background compositing not taking effect | `Canvas::setTransparentBackground`, present blend in `bgfx_gl.cpp` |
| Visualizer draws but UI is invisible (or vice versa) | draw order / framebuffer binding | `onDrawBackground()` fires after `drawWindow()`, before `present()` |
| Compute kernel disagrees with CPU | barriers or `bitfieldReverse` on v3d | `kComputeSource` in `examples/AudioVisualizer/visualizer_gl.cpp`, or `examples/shaders/fft_*.comp` on SDL_GPU |
| GLES 3.0 instead of 3.1 | v3d/Mesa version, or `fkms` instead of full KMS | stage 0 `config.txt`, `apt list --installed | grep mesa` |
| Input in the wrong place after rotation | input transform vs render transform disagree | `setScreenRotation` consumers in the SDL3 adapter |

---

## What to send back

1. The configure output's `Video drivers:` and `Audio drivers:` lines
2. The four `GL_*` log lines from stage 2
3. `ctest` summary, plus the names of any failing tests
4. Which examples looked wrong, and a photo of each
5. The visualizer status line and the two microsecond figures
6. Whether rotation worked for both rendering and input
