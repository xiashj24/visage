in vec4 a_position;
in vec4 a_texcoord0;
in vec4 a_texcoord1;

out vec2 v_texture_uv;

uniform vec4 u_bounds;
uniform vec4 u_atlas_scale;
uniform vec4 u_zoom;

void main() {
  vec2 minimum = a_texcoord1.xy;
  vec2 maximum = a_texcoord1.zw;
  vec2 clamped = clamp(a_position.xy, minimum, maximum);
  vec2 delta = clamped - a_position.xy;

  v_texture_uv = (a_texcoord0.xy + delta) * u_atlas_scale.xy;
  vec2 adjusted_position = clamped * u_bounds.xy + u_bounds.zw;
  gl_Position = vec4(adjusted_position * u_zoom.x, 0.5, 1.0);
}
