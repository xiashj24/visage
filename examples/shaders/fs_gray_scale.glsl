in vec2 v_texture_uv;

out vec4 fragColor;

uniform sampler2D s_texture;

void main() {
  vec4 color = texture(s_texture, v_texture_uv);
  float gray = dot(color.rgb, vec3(0.299, 0.587, 0.114)) * u_color_mult.x;
  fragColor = vec4(gray, gray, gray, color.a);
}
