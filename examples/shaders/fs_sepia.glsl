in vec2 v_texture_uv;

out vec4 fragColor;

uniform sampler2D s_texture;

void main() {
  vec4 color = texture(s_texture, v_texture_uv);
  fragColor.r = dot(color.rgb, vec3(0.393, 0.769, 0.189)) * u_color_mult.x;
  fragColor.g = dot(color.rgb, vec3(0.349, 0.686, 0.168)) * u_color_mult.x;
  fragColor.b = dot(color.rgb, vec3(0.272, 0.534, 0.131)) * u_color_mult.x;
  fragColor.a = color.a;
}
