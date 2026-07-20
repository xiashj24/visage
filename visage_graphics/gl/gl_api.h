/* Copyright Vital Audio, LLC
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#pragma once

// Self-contained OpenGL 3.3 core / OpenGL ES 3.0 access. No system GL headers;
// every entry point is resolved at runtime through a proc-address getter the
// application injects, so this library never links a GL loader or SDL.

#include <cstddef>
#include <cstdint>

// GL entry points use the system calling convention on 32-bit Windows.
#if defined(_WIN32)
#define VISAGE_GL_CALL __stdcall
#else
#define VISAGE_GL_CALL
#endif

namespace visage {
  using GLenum = uint32_t;
  using GLboolean = uint8_t;
  using GLbitfield = uint32_t;
  using GLint = int32_t;
  using GLuint = uint32_t;
  using GLsizei = int32_t;
  using GLfloat = float;
  using GLchar = char;
  using GLintptr = intptr_t;
  using GLsizeiptr = ptrdiff_t;

  enum : GLenum {
    GL_FALSE = 0,
    GL_TRUE = 1,
    GL_ZERO = 0,
    GL_ONE = 1,

    GL_TRIANGLES = 0x0004,
    GL_TRIANGLE_STRIP = 0x0005,

    GL_SRC_ALPHA = 0x0302,
    GL_ONE_MINUS_SRC_ALPHA = 0x0303,
    GL_DST_COLOR = 0x0306,

    GL_FUNC_ADD = 0x8006,
    GL_FUNC_REVERSE_SUBTRACT = 0x800B,

    GL_BLEND = 0x0BE2,
    GL_SCISSOR_TEST = 0x0C11,
    GL_DEPTH_TEST = 0x0B71,
    GL_CULL_FACE = 0x0B44,
    GL_CW = 0x0900,
    GL_CCW = 0x0901,

    GL_UNPACK_ALIGNMENT = 0x0CF5,
    GL_PACK_ALIGNMENT = 0x0D05,

    GL_TEXTURE_2D = 0x0DE1,
    GL_TEXTURE0 = 0x84C0,
    GL_TEXTURE_MIN_FILTER = 0x2801,
    GL_TEXTURE_MAG_FILTER = 0x2800,
    GL_TEXTURE_WRAP_S = 0x2802,
    GL_TEXTURE_WRAP_T = 0x2803,
    GL_TEXTURE_SWIZZLE_R = 0x8E42,
    GL_TEXTURE_SWIZZLE_G = 0x8E43,
    GL_TEXTURE_SWIZZLE_B = 0x8E44,
    GL_TEXTURE_SWIZZLE_A = 0x8E45,
    GL_NEAREST = 0x2600,
    GL_LINEAR = 0x2601,
    GL_CLAMP_TO_EDGE = 0x812F,
    GL_REPEAT = 0x2901,
    GL_RED = 0x1903,
    GL_GREEN = 0x1904,
    GL_BLUE = 0x1905,
    GL_ALPHA = 0x1906,
    GL_RGBA = 0x1908,
    GL_RGBA8 = 0x8058,
    GL_R8 = 0x8229,
    GL_R16F = 0x822D,
    GL_R32F = 0x822E,
    GL_RGBA16F = 0x881A,
    GL_RGB10_A2 = 0x8059,
    GL_UNSIGNED_BYTE = 0x1401,
    GL_UNSIGNED_SHORT = 0x1403,
    GL_UNSIGNED_INT = 0x1405,
    GL_FLOAT = 0x1406,
    GL_HALF_FLOAT = 0x140B,
    GL_UNSIGNED_INT_2_10_10_10_REV = 0x8368,

    GL_ARRAY_BUFFER = 0x8892,
    GL_ELEMENT_ARRAY_BUFFER = 0x8893,
    GL_STATIC_DRAW = 0x88E4,
    GL_STREAM_DRAW = 0x88E0,

    GL_FRAGMENT_SHADER = 0x8B30,
    GL_VERTEX_SHADER = 0x8B31,
    GL_COMPILE_STATUS = 0x8B81,
    GL_LINK_STATUS = 0x8B82,

    GL_FRAMEBUFFER = 0x8D40,
    GL_READ_FRAMEBUFFER = 0x8CA8,
    GL_DRAW_FRAMEBUFFER = 0x8CA9,
    GL_COLOR_ATTACHMENT0 = 0x8CE0,
    GL_FRAMEBUFFER_COMPLETE = 0x8CD5,
    GL_COLOR_BUFFER_BIT = 0x00004000,

    GL_NO_ERROR = 0,
    GL_VENDOR = 0x1F00,
    GL_RENDERER = 0x1F01,
    GL_VERSION = 0x1F02,
    GL_EXTENSIONS = 0x1F03,
    GL_SHADING_LANGUAGE_VERSION = 0x8B8C,
    GL_NUM_EXTENSIONS = 0x821D,
    GL_MAX_TEXTURE_SIZE = 0x0D33,
  };

  // Function-pointer table for the exact GL entry points this backend uses.
  // All calls must happen on the thread where the context is current.
  struct GlApi {
    void(VISAGE_GL_CALL* enable)(GLenum cap) = nullptr;
    void(VISAGE_GL_CALL* disable)(GLenum cap) = nullptr;
    void(VISAGE_GL_CALL* frontFace)(GLenum mode) = nullptr;
    GLenum(VISAGE_GL_CALL* getError)() = nullptr;
    const uint8_t*(VISAGE_GL_CALL* getString)(GLenum name) = nullptr;
    const uint8_t*(VISAGE_GL_CALL* getStringi)(GLenum name, GLuint index) = nullptr;
    void(VISAGE_GL_CALL* getIntegerv)(GLenum pname, GLint* data) = nullptr;
    void(VISAGE_GL_CALL* viewport)(GLint x, GLint y, GLsizei width, GLsizei height) = nullptr;
    void(VISAGE_GL_CALL* scissor)(GLint x, GLint y, GLsizei width, GLsizei height) = nullptr;
    void(VISAGE_GL_CALL* clearColor)(GLfloat r, GLfloat g, GLfloat b, GLfloat a) = nullptr;
    void(VISAGE_GL_CALL* clear)(GLbitfield mask) = nullptr;
    void(VISAGE_GL_CALL* colorMask)(GLboolean r, GLboolean g, GLboolean b, GLboolean a) = nullptr;
    void(VISAGE_GL_CALL* blendFuncSeparate)(GLenum src_rgb, GLenum dst_rgb, GLenum src_alpha,
                                            GLenum dst_alpha) = nullptr;
    void(VISAGE_GL_CALL* blendEquationSeparate)(GLenum mode_rgb, GLenum mode_alpha) = nullptr;
    void(VISAGE_GL_CALL* pixelStorei)(GLenum pname, GLint param) = nullptr;
    void(VISAGE_GL_CALL* flush)() = nullptr;
    void(VISAGE_GL_CALL* finish)() = nullptr;

    GLuint(VISAGE_GL_CALL* createShader)(GLenum type) = nullptr;
    void(VISAGE_GL_CALL* deleteShader)(GLuint shader) = nullptr;
    void(VISAGE_GL_CALL* shaderSource)(GLuint shader, GLsizei count, const GLchar* const* strings,
                                       const GLint* lengths) = nullptr;
    void(VISAGE_GL_CALL* compileShader)(GLuint shader) = nullptr;
    void(VISAGE_GL_CALL* getShaderiv)(GLuint shader, GLenum pname, GLint* param) = nullptr;
    void(VISAGE_GL_CALL* getShaderInfoLog)(GLuint shader, GLsizei max_length, GLsizei* length,
                                           GLchar* info_log) = nullptr;
    GLuint(VISAGE_GL_CALL* createProgram)() = nullptr;
    void(VISAGE_GL_CALL* deleteProgram)(GLuint program) = nullptr;
    void(VISAGE_GL_CALL* attachShader)(GLuint program, GLuint shader) = nullptr;
    void(VISAGE_GL_CALL* bindAttribLocation)(GLuint program, GLuint index, const GLchar* name) = nullptr;
    void(VISAGE_GL_CALL* linkProgram)(GLuint program) = nullptr;
    void(VISAGE_GL_CALL* getProgramiv)(GLuint program, GLenum pname, GLint* param) = nullptr;
    void(VISAGE_GL_CALL* getProgramInfoLog)(GLuint program, GLsizei max_length, GLsizei* length,
                                            GLchar* info_log) = nullptr;
    void(VISAGE_GL_CALL* useProgram)(GLuint program) = nullptr;
    GLint(VISAGE_GL_CALL* getUniformLocation)(GLuint program, const GLchar* name) = nullptr;
    void(VISAGE_GL_CALL* uniform1i)(GLint location, GLint v0) = nullptr;
    void(VISAGE_GL_CALL* uniform4fv)(GLint location, GLsizei count, const GLfloat* value) = nullptr;
    void(VISAGE_GL_CALL* uniformMatrix3fv)(GLint location, GLsizei count, GLboolean transpose,
                                           const GLfloat* value) = nullptr;
    void(VISAGE_GL_CALL* uniformMatrix4fv)(GLint location, GLsizei count, GLboolean transpose,
                                           const GLfloat* value) = nullptr;

    void(VISAGE_GL_CALL* genBuffers)(GLsizei n, GLuint* buffers) = nullptr;
    void(VISAGE_GL_CALL* deleteBuffers)(GLsizei n, const GLuint* buffers) = nullptr;
    void(VISAGE_GL_CALL* bindBuffer)(GLenum target, GLuint buffer) = nullptr;
    void(VISAGE_GL_CALL* bufferData)(GLenum target, GLsizeiptr size, const void* data, GLenum usage) = nullptr;
    void(VISAGE_GL_CALL* genVertexArrays)(GLsizei n, GLuint* arrays) = nullptr;
    void(VISAGE_GL_CALL* deleteVertexArrays)(GLsizei n, const GLuint* arrays) = nullptr;
    void(VISAGE_GL_CALL* bindVertexArray)(GLuint array) = nullptr;
    void(VISAGE_GL_CALL* enableVertexAttribArray)(GLuint index) = nullptr;
    void(VISAGE_GL_CALL* disableVertexAttribArray)(GLuint index) = nullptr;
    void(VISAGE_GL_CALL* vertexAttribPointer)(GLuint index, GLint size, GLenum type,
                                              GLboolean normalized, GLsizei stride,
                                              const void* pointer) = nullptr;
    void(VISAGE_GL_CALL* drawElements)(GLenum mode, GLsizei count, GLenum type, const void* indices) = nullptr;
    void(VISAGE_GL_CALL* drawArrays)(GLenum mode, GLint first, GLsizei count) = nullptr;

    void(VISAGE_GL_CALL* genTextures)(GLsizei n, GLuint* textures) = nullptr;
    void(VISAGE_GL_CALL* deleteTextures)(GLsizei n, const GLuint* textures) = nullptr;
    void(VISAGE_GL_CALL* bindTexture)(GLenum target, GLuint texture) = nullptr;
    void(VISAGE_GL_CALL* activeTexture)(GLenum texture) = nullptr;
    void(VISAGE_GL_CALL* texImage2D)(GLenum target, GLint level, GLint internal_format, GLsizei width,
                                     GLsizei height, GLint border, GLenum format, GLenum type,
                                     const void* pixels) = nullptr;
    void(VISAGE_GL_CALL* texSubImage2D)(GLenum target, GLint level, GLint x, GLint y, GLsizei width,
                                        GLsizei height, GLenum format, GLenum type,
                                        const void* pixels) = nullptr;
    void(VISAGE_GL_CALL* texParameteri)(GLenum target, GLenum pname, GLint param) = nullptr;

    void(VISAGE_GL_CALL* genFramebuffers)(GLsizei n, GLuint* framebuffers) = nullptr;
    void(VISAGE_GL_CALL* deleteFramebuffers)(GLsizei n, const GLuint* framebuffers) = nullptr;
    void(VISAGE_GL_CALL* bindFramebuffer)(GLenum target, GLuint framebuffer) = nullptr;
    void(VISAGE_GL_CALL* framebufferTexture2D)(GLenum target, GLenum attachment, GLenum tex_target,
                                               GLuint texture, GLint level) = nullptr;
    GLenum(VISAGE_GL_CALL* checkFramebufferStatus)(GLenum target) = nullptr;
    void(VISAGE_GL_CALL* blitFramebuffer)(GLint src_x0, GLint src_y0, GLint src_x1, GLint src_y1,
                                          GLint dst_x0, GLint dst_y0, GLint dst_x1, GLint dst_y1,
                                          GLbitfield mask, GLenum filter) = nullptr;
    void(VISAGE_GL_CALL* readPixels)(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format,
                                     GLenum type, void* pixels) = nullptr;
  };

  extern GlApi gl;

  using GlProcAddressGetter = void* (*)(const char* name);

  // Resolves every entry point in `gl` against the caller's current GL
  // context. Returns false if any required function is missing.
  bool loadGlApi(GlProcAddressGetter get_proc_address);
  bool glApiLoaded();

  // True when the current context is OpenGL ES rather than desktop GL,
  // detected from the GL_VERSION string at load time.
  bool glContextIsEs();
  bool glContextHasExtension(const char* name);
}
