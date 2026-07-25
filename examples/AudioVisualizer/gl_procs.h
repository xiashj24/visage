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

// The visualizer owns its own GL objects rather than going through visage's
// renderer, so it resolves the entry points it needs itself. visage's internal
// gl_api is deliberately not reused: this file is what an application outside
// the library would have to write.

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#define VIZ_GL_CALL __stdcall
#else
#define VIZ_GL_CALL
#endif

namespace viz {
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

    GL_BLEND = 0x0BE2,
    GL_TEXTURE_2D = 0x0DE1,
    GL_TEXTURE0 = 0x84C0,
    GL_TEXTURE_MIN_FILTER = 0x2801,
    GL_TEXTURE_MAG_FILTER = 0x2800,
    GL_TEXTURE_WRAP_S = 0x2802,
    GL_TEXTURE_WRAP_T = 0x2803,
    GL_NEAREST = 0x2600,
    GL_CLAMP_TO_EDGE = 0x812F,

    GL_RED = 0x1903,
    GL_R32F = 0x822E,
    GL_FLOAT = 0x1406,

    GL_ARRAY_BUFFER = 0x8892,
    GL_SHADER_STORAGE_BUFFER = 0x90D2,
    GL_DYNAMIC_DRAW = 0x88E8,

    GL_VERTEX_SHADER = 0x8B31,
    GL_FRAGMENT_SHADER = 0x8B30,
    GL_COMPUTE_SHADER = 0x91B9,
    GL_COMPILE_STATUS = 0x8B81,
    GL_LINK_STATUS = 0x8B82,

    GL_WRITE_ONLY = 0x88B9,
    GL_SHADER_IMAGE_ACCESS_BARRIER_BIT = 0x00000020,
    GL_TEXTURE_FETCH_BARRIER_BIT = 0x00000008,
    GL_SHADER_STORAGE_BARRIER_BIT = 0x00002000,

    GL_FRAMEBUFFER = 0x8D40,
    GL_COLOR_ATTACHMENT0 = 0x8CE0,
    GL_FRAMEBUFFER_COMPLETE = 0x8CD5,
    GL_NO_ERROR = 0,
    GL_RENDERER = 0x1F01,
    GL_VERSION = 0x1F02,
    GL_MAX_COMPUTE_SHARED_MEMORY_SIZE = 0x8262,
    GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS = 0x90EB,
  };

  struct GlProcs {
    void(VIZ_GL_CALL* viewport)(GLint x, GLint y, GLsizei width, GLsizei height) = nullptr;
    void(VIZ_GL_CALL* blendFunc)(GLenum src, GLenum dst) = nullptr;
    void(VIZ_GL_CALL* getIntegerv)(GLenum pname, GLint* data) = nullptr;
    const uint8_t*(VIZ_GL_CALL* getString)(GLenum name) = nullptr;
    GLenum(VIZ_GL_CALL* getError)() = nullptr;
    void(VIZ_GL_CALL* bindFramebuffer)(GLenum target, GLuint framebuffer) = nullptr;
    void(VIZ_GL_CALL* genFramebuffers)(GLsizei n, GLuint* framebuffers) = nullptr;
    void(VIZ_GL_CALL* deleteFramebuffers)(GLsizei n, const GLuint* framebuffers) = nullptr;
    void(VIZ_GL_CALL* framebufferTexture2D)(GLenum target, GLenum attachment, GLenum tex_target,
                                            GLuint texture, GLint level) = nullptr;
    GLenum(VIZ_GL_CALL* checkFramebufferStatus)(GLenum target) = nullptr;
    void(VIZ_GL_CALL* readPixels)(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format,
                                  GLenum type, void* pixels) = nullptr;

    GLuint(VIZ_GL_CALL* createShader)(GLenum type) = nullptr;
    void(VIZ_GL_CALL* shaderSource)(GLuint shader, GLsizei count, const GLchar* const* strings,
                                    const GLint* lengths) = nullptr;
    void(VIZ_GL_CALL* compileShader)(GLuint shader) = nullptr;
    void(VIZ_GL_CALL* getShaderiv)(GLuint shader, GLenum pname, GLint* param) = nullptr;
    void(VIZ_GL_CALL* getShaderInfoLog)(GLuint shader, GLsizei max_length, GLsizei* length,
                                        GLchar* log) = nullptr;
    void(VIZ_GL_CALL* deleteShader)(GLuint shader) = nullptr;
    GLuint(VIZ_GL_CALL* createProgram)() = nullptr;
    void(VIZ_GL_CALL* attachShader)(GLuint program, GLuint shader) = nullptr;
    void(VIZ_GL_CALL* linkProgram)(GLuint program) = nullptr;
    void(VIZ_GL_CALL* getProgramiv)(GLuint program, GLenum pname, GLint* param) = nullptr;
    void(VIZ_GL_CALL* getProgramInfoLog)(GLuint program, GLsizei max_length, GLsizei* length,
                                         GLchar* log) = nullptr;
    void(VIZ_GL_CALL* useProgram)(GLuint program) = nullptr;
    void(VIZ_GL_CALL* deleteProgram)(GLuint program) = nullptr;
    GLint(VIZ_GL_CALL* getUniformLocation)(GLuint program, const GLchar* name) = nullptr;
    void(VIZ_GL_CALL* uniform1i)(GLint location, GLint v0) = nullptr;
    void(VIZ_GL_CALL* uniform1f)(GLint location, GLfloat v0) = nullptr;
    void(VIZ_GL_CALL* uniform2f)(GLint location, GLfloat v0, GLfloat v1) = nullptr;

    void(VIZ_GL_CALL* genVertexArrays)(GLsizei n, GLuint* arrays) = nullptr;
    void(VIZ_GL_CALL* bindVertexArray)(GLuint array) = nullptr;
    void(VIZ_GL_CALL* deleteVertexArrays)(GLsizei n, const GLuint* arrays) = nullptr;
    void(VIZ_GL_CALL* drawArrays)(GLenum mode, GLint first, GLsizei count) = nullptr;

    void(VIZ_GL_CALL* genBuffers)(GLsizei n, GLuint* buffers) = nullptr;
    void(VIZ_GL_CALL* bindBuffer)(GLenum target, GLuint buffer) = nullptr;
    void(VIZ_GL_CALL* bufferData)(GLenum target, GLsizeiptr size, const void* data, GLenum usage) = nullptr;
    void(VIZ_GL_CALL* bufferSubData)(GLenum target, GLintptr offset, GLsizeiptr size,
                                     const void* data) = nullptr;
    void(VIZ_GL_CALL* deleteBuffers)(GLsizei n, const GLuint* buffers) = nullptr;

    void(VIZ_GL_CALL* genTextures)(GLsizei n, GLuint* textures) = nullptr;
    void(VIZ_GL_CALL* bindTexture)(GLenum target, GLuint texture) = nullptr;
    void(VIZ_GL_CALL* texStorage2D)(GLenum target, GLsizei levels, GLenum internal_format,
                                    GLsizei width, GLsizei height) = nullptr;
    void(VIZ_GL_CALL* texSubImage2D)(GLenum target, GLint level, GLint x, GLint y, GLsizei width,
                                     GLsizei height, GLenum format, GLenum type,
                                     const void* pixels) = nullptr;
    void(VIZ_GL_CALL* texParameteri)(GLenum target, GLenum pname, GLint param) = nullptr;
    void(VIZ_GL_CALL* activeTexture)(GLenum texture) = nullptr;
    void(VIZ_GL_CALL* deleteTextures)(GLsizei n, const GLuint* textures) = nullptr;

    // Compute-only; null unless the context reached GL 4.3 / GLES 3.1.
    void(VIZ_GL_CALL* bindBufferBase)(GLenum target, GLuint index, GLuint buffer) = nullptr;
    void(VIZ_GL_CALL* bindImageTexture)(GLuint unit, GLuint texture, GLint level, GLboolean layered,
                                        GLint layer, GLenum access, GLenum format) = nullptr;
    void(VIZ_GL_CALL* dispatchCompute)(GLuint x, GLuint y, GLuint z) = nullptr;
    void(VIZ_GL_CALL* memoryBarrier)(GLbitfield barriers) = nullptr;
  };

  extern GlProcs gl;

  using GlProcAddressGetter = void* (*)(const char*);

  // Resolves entry points against the caller's current context. Returns false
  // if a non-compute function is missing.
  bool loadGlProcs(GlProcAddressGetter get_proc_address);

  // GL_VERSION as major * 100 + minor, and whether it is an ES context.
  int glVersion();
  bool glIsEs();

  // True when the context supports compute shaders and every compute entry
  // point resolved.
  bool glHasCompute();
}
