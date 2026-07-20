in vec2 v_coordinates;

out vec4 fragColor;

uniform sampler2D s_texture;

uniform vec4 u_pixel_size;

void main() {
  vec2 offset = 0.5 * u_pixel_size.xy;
  vec4 c = texture(s_texture, v_coordinates + vec2(-offset.x, -offset.y)) +
           texture(s_texture, v_coordinates + vec2(offset.x, -offset.y)) +
           texture(s_texture, v_coordinates + vec2(-offset.x, offset.y)) +
           texture(s_texture, v_coordinates + vec2(offset.x, offset.y));
  fragColor = c * 0.25;
}
