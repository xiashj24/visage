#version 450

// AudioVisualizer's spectrum + waveform view, matching what the example's
// OpenGL path draws. Not a visage shader: the example drives SDL_GPU itself,
// so shadertool compiles this verbatim and the sets/bindings below are the
// example's own.

layout(location = 0) out vec4 fragColor;

// Row 0 = smoothed spectrum, row 1 = waveform; .x = left, .y = right. RG32F is
// not filterable everywhere, so bins are fetched and blended by hand.
layout(set = 2, binding = 0) uniform sampler2D s_audio;

layout(set = 3, binding = 0, std140) uniform Params {
  vec2 u_resolution;
  float u_time;
  float u_amplitude;
  float u_rotation;  // screen rotation in quarter turns
};

const float kBins = 512.0;

// The application draws into the window surface, and visage's present pass
// rotates only the UI layer it composites on top - so without this the picture
// below it stays the way the panel is wired while the interface turns.
//
// This is the present pass's own mapping, not a guess at it: its screen corners
// ring BL, BR, TR, TL against texture corners in the same order, and a quarter
// turn shifts that ring, which for one turn puts the image's top edge along the
// screen's right edge. Feeding screen position in gives logical position back.
vec2 logicalUv(vec2 uv, int turns) {
  if (turns == 1)
    return vec2(1.0 - uv.y, uv.x);
  if (turns == 2)
    return vec2(1.0 - uv.x, 1.0 - uv.y);
  if (turns == 3)
    return vec2(uv.y, 1.0 - uv.x);
  return uv;
}

vec2 audioRow(float bin, int row) {
  float clamped = clamp(bin, 0.0, kBins - 1.0);
  int low = int(clamped);
  int high = min(low + 1, int(kBins) - 1);
  vec2 a = texelFetch(s_audio, ivec2(low, row), 0).rg;
  vec2 b = texelFetch(s_audio, ivec2(high, row), 0).rg;
  return mix(a, b, clamped - float(low));
}

// Log-frequency axis: bin 2 to bin 480 spread evenly across the width.
float binForX(float x) {
  return exp2(mix(1.0, log2(480.0), x));
}

void main() {
  // gl_FragCoord is y-down here; the picture below is written y-up.
  vec2 uv = gl_FragCoord.xy / u_resolution;
  uv.y = 1.0 - uv.y;
  uv = logicalUv(uv, int(u_rotation));

  vec2 spectrum = audioRow(binForX(uv.x), 0);

  vec3 color = mix(vec3(0.015, 0.02, 0.05), vec3(0.05, 0.05, 0.12), uv.y);
  float middle = 1.0 - abs(uv.y - 0.5) * 2.0;
  color += vec3(0.07, 0.03, 0.13) * pow(middle, 3.0) * u_amplitude;

  vec3 hue = 0.5 + 0.5 * cos(6.2831853 * (uv.x * 0.55 + vec3(0.0, 0.33, 0.67) + 0.006 * u_time));

  // Spectrum mirrored around the middle: left channel up, right channel down.
  float top = 0.5 + spectrum.x * 0.46;
  float bottom = 0.5 - spectrum.y * 0.46;
  float fill = smoothstep(top, top - 0.01, uv.y) * step(0.5, uv.y) +
               smoothstep(bottom, bottom + 0.01, uv.y) * step(uv.y, 0.5);
  color += hue * fill * 0.5;
  color += hue * (exp(-abs(uv.y - top) * 240.0) + exp(-abs(uv.y - bottom) * 240.0)) * 1.5;

  vec2 wave = 0.5 + audioRow(uv.x * kBins, 1) * 0.3;
  color += vec3(0.6, 0.85, 1.0) * exp(-abs(uv.y - wave.x) * 400.0) * 0.7;
  color += vec3(1.0, 0.6, 0.85) * exp(-abs(uv.y - wave.y) * 400.0) * 0.7;

  vec2 centered = uv - 0.5;
  color *= 1.0 - dot(centered, centered) * 0.6;
  fragColor = vec4(color, 1.0);
}
