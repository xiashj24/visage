# Third-party shaders

Drop a fragment shader here, run `cmake --build <dir> --target shaders`, and it
compiles to a SPIR-V + MSL blob. You write **stock Shadertoy** — no
visage-specific syntax, no wrapper, nothing to learn.

```glsl
void mainImage(out vec4 fragColor, in vec2 fragCoord) {
  vec2 uv = fragCoord / iResolution.xy;
  fragColor = vec4(uv, 0.5 + 0.5 * sin(iTime), 1.0);
}
```

Defining `mainImage` is what marks a file as an effect. Library shaders write
`main()` and declare their own varyings; the two can never be confused.

## What you get

| | |
|---|---|
| `iResolution` | `vec3(width, height, 1.0)` in pixels |
| `iTime` | seconds, float |
| `iChannel0` | audio texture — see below |
| `fragCoord` | pixels, y-up from bottom-left, as Shadertoy defines it |

**Everything else Shadertoy defines is deliberately absent.** `iMouse`,
`iFrame`, `iTimeDelta`, `iDate`, `iChannel1`–`iChannel3` fail to compile with an
undeclared-identifier error naming the line. Nothing populates them, so
declaring them would hand you silent garbage instead of a build failure.

## Audio input

`iChannel0` is a 512×2 texture: **spectrum on row 0, waveform on row 1** —
Shadertoy's audio layout exactly, so audio-reactive Shadertoy shaders port
unmodified. Convenience helpers are injected when you reference it:

```glsl
float spectrum01(float x);   // x in 0..1 across the spectrum
float waveform(float x);
vec2  spectrum01LR(float x); // .x left, .y right
vec2  waveformLR(float x);
```

The sampler is declared only when your shader mentions it — an unbound sampler
is a validation error.

## Not supported

A shader here is **one fragment pass**. Multi-pass Buffer A/B/C/D, feedback from
the previous frame, and cubemap channels have no equivalent and are rejected
rather than compiled into something that renders wrong.

## Verification, and its limits

The offline compile checks that the shader builds to SPIR-V and fits the
target's resource ceilings (`targetLimits()` in `tools/shadertool/main.cpp`). A
failure returns a non-zero exit code and writes no blob, so a bad shader fails
the build.

**It cannot tell you the shader is fast enough.** A Shadertoy raymarcher
compiles perfectly and runs at single-digit fps on a Pi 4. Frame time is an
on-device measurement — see stage 7 of `PI4_BRINGUP.md` for the pattern.

Two caveats worth knowing:

- The resource limits are still glslang's desktop defaults. Real v3d maxima land
  in Phase 0 of `SDLGPU_PORT.md`; until then a shader can pass here and still
  overrun the Pi.
- An unbounded loop can hang the GPU. On a KMSDRM device that wedges the display
  with no desktop to fall back to, so bound your loops.
