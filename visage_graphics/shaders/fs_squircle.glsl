in vec2 v_coordinates;
in vec2 v_dimensions;
in vec4 v_shader_values;
in vec2 v_position;
in vec4 v_gradient_texture_pos;
in vec4 v_gradient_pos;
in vec4 v_gradient_pos2;

out vec4 fragColor;

uniform sampler2D s_gradient;

void main() {
  fragColor = gradient(s_gradient, v_gradient_texture_pos, v_gradient_pos, v_gradient_pos2, v_position);
  fragColor.a = fragColor.a * squircle(v_coordinates, v_dimensions, v_shader_values.z, v_shader_values.x, v_shader_values.y);
}
