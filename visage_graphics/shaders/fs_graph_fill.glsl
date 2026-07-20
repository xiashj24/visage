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
  fragColor = gradient(s_gradient, v_gradient_texture_pos, v_gradient_pos, v_gradient_pos2, v_position);
  float center_y = v_shader_values.x * v_dimensions.y;
  float resolution = v_shader_values.y;
  vec2 start_data = v_shader_values.zw * u_atlas_scale.xy;

  vec2 percent = v_coordinates * 0.5 + vec2(0.5, 0.5);
  vec2 pos = percent * v_dimensions;

  vec2 sample_pos = start_data + vec2(u_atlas_scale.x * percent.x * resolution, 0.0);
  float line_y1 = v_dimensions.y * texture(s_texture, sample_pos + vec2(-0.0001, 0.0)).r;
  float line_y2 = v_dimensions.y * texture(s_texture, sample_pos + vec2(0.0001, 0.0)).r;
  float line_y = 0.5 * (line_y1 + line_y2);
  float adjust = abs(line_y1 - line_y2) / (v_dimensions.x * 0.0002);
  float fade = sqrt(1.0 + adjust * adjust);

  float mult = sign(pos.y - center_y);
  float alpha = 1.0 - smoothed(-0.5 * fade, 0.5 * fade, mult * (pos.y - line_y)) * smoothed(-0.5, 0.5, mult * (pos.y - center_y));
  fragColor.a = fragColor.a * alpha;
}
