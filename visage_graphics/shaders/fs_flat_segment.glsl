in vec2 v_coordinates;
in vec2 v_dimensions;
in vec4 v_shader_values;
in vec4 v_shader_values1;
in vec2 v_position;
in vec4 v_gradient_texture_pos;
in vec4 v_gradient_pos;
in vec4 v_gradient_pos2;

out vec4 fragColor;

uniform sampler2D s_gradient;
uniform vec4 u_origin_flip;

void main() {
  fragColor = gradient(s_gradient, v_gradient_texture_pos, v_gradient_pos, v_gradient_pos2, v_position);
  vec2 flip_mult = vec2(1.0, u_origin_flip.x);
  fragColor.a = fragColor.a * flatSegment(v_coordinates, v_dimensions, v_shader_values.zw * flip_mult, v_shader_values1.xy * flip_mult, v_shader_values.x);
}
