in vec2 v_coordinates;
in vec2 v_dimensions;
in vec4 v_shader_values;
in vec2 v_position;
in vec4 v_gradient_texture_pos;
in vec4 v_gradient_pos;
in vec4 v_gradient_pos2;

out vec4 fragColor;

// No s_gradient: this quad colours itself from uniforms, and declaring a
// sampler it never reads leaves the survivors on non-contiguous bindings.
uniform sampler2D s_texture;

uniform vec4 u_time;
uniform vec4 u_spectrum_tint;
uniform vec4 u_waveform_tint;

void main() {
  vec2 uv = v_coordinates * 0.5 + vec2(0.5, 0.5);
  float level = texture(s_texture, vec2(uv.x, 0.25)).x;
  float waveform = texture(s_texture, vec2(uv.x, 0.75)).x;

  float from_bottom = 1.0 - uv.y;
  float bar = smoothed(level + 0.01, level - 0.01, from_bottom);
  float trace = 1.0 - smoothed(0.0, 0.012, abs(from_bottom - waveform));

  // Premultiplied, so colour and coverage scale together.
  fragColor = u_spectrum_tint * bar * 0.8 + u_waveform_tint * trace;
}
