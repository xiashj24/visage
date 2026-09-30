in vec2 v_coordinates;
in vec2 v_position;
in vec4 v_gradient_texture_pos;
in vec4 v_gradient_pos;
in vec4 v_gradient_pos2;

out vec4 fragColor;

uniform sampler2D s_gradient;
uniform sampler2D s_texture;
// Text's polarity correction: x the coverage exponent for black text, y for
// white, z nonzero while it applies. Only a glyph's texels are white, so an
// emoji sharing the atlas keeps its colours.
uniform vec4 u_text_gamma;

void main() {
  vec4 color = gradient(s_gradient, v_gradient_texture_pos, v_gradient_pos, v_gradient_pos2, v_position);
  vec4 texel = texture(s_texture, v_coordinates);
  if (u_text_gamma.z != 0.0 && min(texel.r, min(texel.g, texel.b)) > 0.999) {
    float luminance = dot(color.rgb, vec3(0.2126, 0.7152, 0.0722));
    texel.a = pow(texel.a, mix(u_text_gamma.x, u_text_gamma.y, luminance));
  }
  fragColor = color * texel;
}
