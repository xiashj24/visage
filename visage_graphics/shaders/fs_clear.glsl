out vec4 fragColor;

uniform vec4 u_color;

void main() {
  fragColor = vec4(u_color.r, 0.0, 0.0, 0.0);
}
