// Third-party sample: stock Shadertoy convention, no audio input.

void mainImage(out vec4 fragColor, in vec2 fragCoord) {
  vec2 uv = (fragCoord - 0.5 * iResolution.xy) / iResolution.y;

  float d = length(uv);
  float a = atan(uv.y, uv.x);
  float v = sin(8.0 * d - iTime * 2.0 + 3.0 * a);
  vec3 color = 0.5 + 0.5 * cos(vec3(0.0, 2.0, 4.0) + v + iTime * 0.3);

  fragColor = vec4(color * smoothstep(0.9, 0.2, d), 1.0);
}
