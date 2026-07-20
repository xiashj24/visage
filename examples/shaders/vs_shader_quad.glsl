in vec4 a_position;
in vec4 a_color0;
in vec4 a_color1;
in vec4 a_color2;
in vec4 a_texcoord0;
in vec4 a_texcoord1;
in vec4 a_texcoord2;

out vec2 v_coordinates;
out vec2 v_dimensions;
out vec4 v_shader_values;
out vec2 v_position;
out vec4 v_gradient_texture_pos;
out vec4 v_gradient_pos;
out vec4 v_gradient_pos2;

uniform vec4 u_bounds;

void main() {
  vec2 minimum = a_texcoord1.xy;
  vec2 maximum = a_texcoord1.zw;
  vec2 clamped = clamp(a_position.xy, minimum, maximum);
  vec2 delta = clamped - a_position.xy;

  v_position = a_position.xy;
  v_gradient_texture_pos = a_color0;
  v_gradient_pos = a_color1;
  v_gradient_pos2 = a_color2;
  v_dimensions = a_texcoord0.zw;
  v_coordinates = a_texcoord0.xy + 2.0 * delta / v_dimensions;
  vec2 adjusted_position = clamped * u_bounds.xy + u_bounds.zw;
  gl_Position = vec4(adjusted_position, 0.5, 1.0);
  v_shader_values = a_texcoord2;
}
