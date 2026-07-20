in vec2 v_texture_uv;

out vec4 fragColor;

uniform sampler2D s_texture;

void main() {
  fragColor = u_color_mult * texture(s_texture, v_texture_uv);
}
