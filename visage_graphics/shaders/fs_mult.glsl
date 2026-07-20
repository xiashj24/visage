in vec2 v_coordinates;

out vec4 fragColor;

uniform sampler2D s_texture;

uniform vec4 u_mult;

void main() {
  fragColor = texture(s_texture, v_coordinates) * u_mult;
}
