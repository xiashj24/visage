in vec2 v_texture_uv;

out vec4 fragColor;

uniform sampler2D s_texture;

uniform vec4 u_alpha;

void main() {
  fragColor = texture(s_texture, v_texture_uv) * u_alpha;
}
