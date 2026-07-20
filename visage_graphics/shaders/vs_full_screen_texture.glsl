in vec4 a_position;

out vec2 v_coordinates;

void main() {
  gl_Position = vec4(a_position.xy, 0.5, 1.0);
  v_coordinates = a_position.zw;
}
