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

float distanceSquared(vec2 position, vec2 point1, vec2 point2) {
  vec2 position_delta = position - point1;
  vec2 line_delta = point2 - point1;
  float t = clamp(dot(position_delta, line_delta) / dot(line_delta, line_delta), 0.0, 1.0);
  vec2 delta = position_delta - line_delta * t;
  delta = delta * delta;
  return delta.x + delta.y;
}

void main() {
  fragColor = gradient(s_gradient, v_gradient_texture_pos, v_gradient_pos, v_gradient_pos2, v_position);
  float thickness = 0.5 * v_shader_values.x;
  float resolution = v_shader_values.y;
  vec2 start_data = v_shader_values.zw * u_atlas_scale.xy;
  float data_width = resolution * u_atlas_scale.x;

  vec2 percent = v_coordinates * 0.5 + vec2(0.5, 0.5);
  vec2 pos = percent * v_dimensions;
  float convert = resolution / v_dimensions.x;
  float range = thickness * convert;
  float range_start = floor(percent.x * resolution - range);
  float range_end = ceil(percent.x * resolution + range);

  float last_x = range_start * v_dimensions.x / resolution;
  float last_y = v_dimensions.y * texture(s_texture, start_data + vec2(clamp(u_atlas_scale.x * range_start, 0.0, data_width), 0.0)).r;

  float min_distance = (thickness + 1.0) * (thickness + 1.0);
  float span = min(20.0, max(1.0, range_end - range_start));
  for (float i = 0.0; i <= span; ++i) {
    float sample_index = range_start + i;
    float x = sample_index / convert;
    float y = v_dimensions.y * texture(s_texture, start_data + vec2(clamp(u_atlas_scale.x * sample_index, 0.0, data_width), 0.0)).r;
    min_distance = min(min_distance, distanceSquared(pos, vec2(last_x, last_y), vec2(x, y)));
    last_x = x;
    last_y = y;
  }

  float alpha = 1.0 - smoothed(-0.5, 0.5, sqrt(min_distance) - thickness);
  fragColor.a = fragColor.a * alpha;
}
