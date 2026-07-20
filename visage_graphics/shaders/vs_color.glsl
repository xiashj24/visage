in vec4 a_position;
in vec4 a_color0;
in vec4 a_color1;
in vec4 a_color2;
in vec4 a_texcoord0;
in vec4 a_texcoord1;

out vec2 v_position;
out vec4 v_gradient_texture_pos;
out vec4 v_gradient_pos;
out vec4 v_gradient_pos2;

uniform vec4 u_bounds;

void main() {
  vec2 minimum = a_texcoord1.xy;
  vec2 maximum = a_texcoord1.zw;
  vec2 clamped = clamp(a_position.xy + a_texcoord0.xy, minimum, maximum);

  v_position = clamped;
  v_gradient_texture_pos = a_color0;
  v_gradient_pos = a_color1;
  v_gradient_pos2 = a_color2;
  gl_Position = vec4(clamped * u_bounds.xy + u_bounds.zw, 0.5, 1.0);
}
