in vec2 v_coordinates;

out vec4 fragColor;

uniform vec4 u_pixel_size;
uniform sampler2D s_texture;

void main() {
  vec2 offset = 1.3333333333333333 * u_pixel_size.xy;
  fragColor = texture(s_texture, v_coordinates) * 0.29411764705882354;
  fragColor += texture(s_texture, v_coordinates + offset) * 0.35294117647058826;
  fragColor += texture(s_texture, v_coordinates - offset) * 0.35294117647058826;
}
