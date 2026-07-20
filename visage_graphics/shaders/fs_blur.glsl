in vec2 v_coordinates;

out vec4 fragColor;

uniform vec4 u_pixel_size;
uniform sampler2D s_texture;

void main() {
  vec2 pixel_step = u_pixel_size.xy;
  fragColor =
    texture(s_texture, v_coordinates + pixel_step * -10.0) * 0.001331642384180115 +
    texture(s_texture, v_coordinates + pixel_step * -9.0)  * 0.0031311897862488025 +
    texture(s_texture, v_coordinates + pixel_step * -8.0)  * 0.006728909236626558 +
    texture(s_texture, v_coordinates + pixel_step * -7.0)  * 0.01321579963304265 +
    texture(s_texture, v_coordinates + pixel_step * -6.0)  * 0.02372224120933465 +
    texture(s_texture, v_coordinates + pixel_step * -5.0)  * 0.038916294930399935 +
    texture(s_texture, v_coordinates + pixel_step * -4.0)  * 0.0583472982820951 +
    texture(s_texture, v_coordinates + pixel_step * -3.0)  * 0.07995092874022597 +
    texture(s_texture, v_coordinates + pixel_step * -2.0)  * 0.10012436424202198 +
    texture(s_texture, v_coordinates + pixel_step * -1.0)  * 0.11459601788478359 +
    texture(s_texture, v_coordinates)                      * 0.11987062734208122 +
    texture(s_texture, v_coordinates + pixel_step * 1.0)   * 0.11459601788478359 +
    texture(s_texture, v_coordinates + pixel_step * 2.0)   * 0.10012436424202198 +
    texture(s_texture, v_coordinates + pixel_step * 3.0)   * 0.07995092874022597 +
    texture(s_texture, v_coordinates + pixel_step * 4.0)   * 0.0583472982820951 +
    texture(s_texture, v_coordinates + pixel_step * 5.0)   * 0.038916294930399935 +
    texture(s_texture, v_coordinates + pixel_step * 6.0)   * 0.02372224120933465 +
    texture(s_texture, v_coordinates + pixel_step * 7.0)   * 0.01321579963304265 +
    texture(s_texture, v_coordinates + pixel_step * 8.0)   * 0.006728909236626558 +
    texture(s_texture, v_coordinates + pixel_step * 9.0)   * 0.0031311897862488025 +
    texture(s_texture, v_coordinates + pixel_step * 10.0)  * 0.001331642384180115;
}
