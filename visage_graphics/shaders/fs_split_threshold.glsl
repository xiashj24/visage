in vec2 v_coordinates;

out vec4 fragColor;

uniform vec4 u_threshold;
uniform vec4 u_limit_mult;
uniform vec4 u_mult;

uniform sampler2D s_texture;

void main() {
  vec4 color = texture(s_texture, v_coordinates);
  float value = max(max(color.r, color.b), color.g) + 0.0001;
  float mult = max(0.0, 1.0 - u_threshold.x / value);
  vec3 bleed = color.rgb * mult;
  vec3 remain = color.rgb - bleed;
  fragColor = vec4(bleed * bleed * u_mult.rgb + remain * u_limit_mult.rgb, 1.0);
}
