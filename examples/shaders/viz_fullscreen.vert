#version 450

// Full-screen pass for the AudioVisualizer example. Not a visage shader: the
// example drives SDL_GPU itself, so shadertool compiles this verbatim.

void main() {
  // Single oversized triangle covering the whole drawable; no vertex buffer.
  vec2 corner = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
  gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
