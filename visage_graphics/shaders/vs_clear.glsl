in vec4 a_position;

void main() {
  gl_Position = vec4(a_position.xy, 0.5, 1.0);
}
