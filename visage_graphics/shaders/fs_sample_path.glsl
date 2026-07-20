in vec2 v_coordinates;
in vec2 v_position;
in vec2 v_dimensions;
in vec4 v_gradient_texture_pos;
in vec4 v_gradient_pos;
in vec4 v_gradient_pos2;

out vec4 fragColor;

uniform sampler2D s_gradient;
uniform sampler2D s_texture;

void main() {
  vec4 color = gradient(s_gradient, v_gradient_texture_pos, v_gradient_pos, v_gradient_pos2, v_position);
  fragColor = color;
  float coverage = abs(texture(s_texture, v_coordinates).r);
  float t = mod(coverage, 2.0);
  float alpha = v_dimensions.x * (1.0 - abs(t - 1.0)) + (1.0 - v_dimensions.x) * clamp(coverage, 0.0, 1.0);
  fragColor.a = fragColor.a * alpha;
}
