in vec2 v_coordinates;
in vec2 v_dimensions;
in vec4 v_shader_values;
in vec2 v_position;
in vec4 v_gradient_texture_pos;
in vec4 v_gradient_pos;
in vec4 v_gradient_pos2;

out vec4 fragColor;

uniform sampler2D s_gradient;
uniform sampler2D s_texture;

uniform vec4 u_atlas_scale;

void main() {
  float y = v_coordinates.y * 0.5 + 0.5;
  float base_pow = pow(2.0, -v_shader_values.y);
  float numerator = pow(base_pow, y) - 1.0;
  float denominator = base_pow - 1.0;
  y = denominator == 0.0 ? y : numerator / denominator;
  y = v_shader_values.x * y;
  vec2 texture_position = (v_shader_values.zw + vec2(0.0, y)) * u_atlas_scale.xy;
  float value = texture(s_texture, texture_position).r;
  fragColor = sampleGradient(s_gradient, v_gradient_texture_pos.xy, v_gradient_texture_pos.zw, clamp(value, 0.0, 1.0));
}
