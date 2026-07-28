// Third-party sample: stock Shadertoy convention, no visage-specific syntax.
// Audio-reactive spectrum bars using iChannel0's FFT row.

void mainImage(out vec4 fragColor, in vec2 fragCoord) {
  vec2 uv = fragCoord / iResolution.xy;

  float level = texture(iChannel0, vec2(uv.x, 0.25)).x;
  float bar = step(1.0 - level, uv.y);

  vec3 color = mix(vec3(0.05, 0.05, 0.1),
                   vec3(0.2 + 0.8 * uv.x, 0.4, 1.0 - 0.6 * uv.x), bar);
  color += 0.15 * sin(iTime + uv.x * 12.0) * bar;

  fragColor = vec4(color, 1.0);
}
