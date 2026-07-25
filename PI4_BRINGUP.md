# Raspberry Pi 4 bring-up checklist

Nothing in the SDL3 + GLES port has run on a Pi yet. Neither has the GPU FFT
visualizer.

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

---

## Stage 1 — configure and build

`VISAGE_OPENGL_ES` defaults to **ON** on Linux, so a plain configure already
gives you the GLES build. Release, because Debug on a Pi 4 is painful.

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

- [ ] Configure output's **`Video drivers:` line contains `kmsdrm`** *and*
      `x11` (or `wayland`). **If `kmsdrm` is absent, stop** — go back to stage 0,
      `rm -rf build`, and configure again.
- [ ] `Audio drivers:` line contains `pulseaudio` or `alsa`
- [ ] `-- VISAGE: Downloading SDL3 3.4.12` succeeded
- [ ] Build completes with no errors

---

## Stage 2 — first pixels, and which GLES version you actually get

Under the desktop, simplest example first:

```bash
./build/examples/ExampleBasic 2>&1 | tee /tmp/visage-gl.txt
```

- [ ] A window appears: dark blue background, cyan circle in the middle
- [ ] Record these four log lines (they go to stderr):

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

- [ ] Basic
- [ ] BlendModes
- [ ] Bloom
- [ ] BringYourOwnWindow
- [ ] Gradients
- [ ] Layout
- [ ] LiveShaderEditing
- [ ] MouseEvents
- [ ] MultiWindow
- [ ] Paths
- [ ] PostEffects
- [ ] Showcase

(`ClapPlugin` is deliberately not built.)

---

## Stage 5 — KMSDRM, the product configuration

Only start this once stages 1–4 are clean, so any new failure is attributable to
windowing. You need a real TTY, not a terminal inside the desktop:

```bash
sudo systemctl isolate multi-user.target     # or: sudo systemctl stop lightdm
SDL_VIDEODRIVER=kmsdrm ./build/examples/ExampleBasic
```

- [ ] Renders full-screen on the panel
- [ ] Keyboard and mouse/touch reach the app
- [ ] A few more examples run the same way (`Showcase`, `Paths`)
- [ ] Exits cleanly and hands the console back

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

- [ ] Rendering is rotated correctly (the present pass does it via per-corner UVs)
- [ ] **Input lands where you press** — rotation transforms input coordinates too,
      and this is the half that is easy to get wrong
- [ ] Test whichever of `Rotate90` / `Rotate180` / `Rotate270` matches the panel;
      test all four if the orientation is not settled

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

- [ ] Status line recorded
- [ ] **Click "Measure both (200x)" and record the CPU and GPU microsecond
      figures.** This is the number that decides whether GPU FFT is worth
      keeping at all. Reference on the dev machine (RTX 3080 Ti): **167 µs CPU
      vs 19 µs GPU**. Expect the Pi's CPU figure to be far larger; the GPU figure
      is the one that matters.
- [ ] Compositing works: the spectrum is visible *through* the translucent panel,
      and the area outside the panel shows the visualizer, not black
- [ ] Frame rate holds at the panel's refresh rate
- [ ] Toggle CPU ↔ GPU compute — the picture should look the same either way
      (the two backends fill the same texture)

Audio note: on a bare TTY the recording device may not open, in which case the
example falls back to **"Test tone"** automatically. That is expected and the
visualizer still works; it does not indicate a problem.

---

## Stage 8 — product-shape checks

- [ ] Sustained frame rate over 10+ minutes (Pi 4 thermal throttling)
- [ ] No memory growth over time
- [ ] **Run it alongside your actual audio DSP load.** "CPU saved" only means
      something when the CPU is contended — this is the measurement that
      justifies or kills the compute FFT for the product.
- [ ] Startup time acceptable (the visualizer runs a 200-iteration benchmark at
      launch; if that hitch is too long on a Pi, drop the call to
      `measureBackends()` in `initializeVisualizer()`)

---

## Failure playbook

| Symptom | Likely cause | Where to look |
|---|---|---|
| `kmsdrm` missing from configure output | dev packages installed after configuring | stage 0, then `rm -rf build` |
| Path fills mangled / inverted | front-face convention differs on v3d | `glFrontFace(GL_CW)` in `initGlBackend`, `fs_path_fill.glsl` |
| Blurs blocky or black | `EXT_color_buffer_float` absent → R16F falls back to R8 | `bgfx_gl.cpp` `r16f_renderable` probe |
| Shader compile errors at startup | v3d's GLSL ES compiler is stricter | error text comes straight from the driver log |
| Everything black outside the UI panel | transparent-background compositing not taking effect | `Canvas::setTransparentBackground`, present blend in `bgfx_gl.cpp` |
| Visualizer draws but UI is invisible (or vice versa) | draw order / framebuffer binding | `onDrawBackground()` fires after `drawWindow()`, before `present()` |
| Compute kernel disagrees with CPU | barriers or `bitfieldReverse` on v3d | `kComputeSource` in `examples/AudioVisualizer/visualizer.cpp` |
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
