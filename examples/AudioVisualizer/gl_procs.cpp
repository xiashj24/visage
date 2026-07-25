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

#include "gl_procs.h"

#include <cstring>

namespace viz {
  GlProcs gl;

  namespace {
    int g_version = 0;
    bool g_is_es = false;
    bool g_has_compute = false;

    template<typename T>
    bool load(GlProcAddressGetter get, T& target, const char* name) {
      target = reinterpret_cast<T>(get(name));
      return target != nullptr;
    }

    // "OpenGL ES 3.1 ..." or "4.6.0 ..." - the first two integers are the
    // version in both forms.
    void parseVersion(const char* version) {
      if (version == nullptr)
        return;

      g_is_es = std::strstr(version, "OpenGL ES") != nullptr;
      const char* digits = version;
      while (*digits && (*digits < '0' || *digits > '9'))
        ++digits;

      int major = 0;
      for (; *digits >= '0' && *digits <= '9'; ++digits)
        major = major * 10 + (*digits - '0');
      if (*digits != '.')
        return;

      int minor = 0;
      for (++digits; *digits >= '0' && *digits <= '9'; ++digits)
        minor = minor * 10 + (*digits - '0');
      g_version = major * 100 + minor;
    }
  }

  bool loadGlProcs(GlProcAddressGetter get) {
    bool ok = true;
    ok &= load(get, gl.viewport, "glViewport");
    ok &= load(get, gl.blendFunc, "glBlendFunc");
    ok &= load(get, gl.getIntegerv, "glGetIntegerv");
    ok &= load(get, gl.getString, "glGetString");
    ok &= load(get, gl.getError, "glGetError");
    ok &= load(get, gl.bindFramebuffer, "glBindFramebuffer");
    ok &= load(get, gl.genFramebuffers, "glGenFramebuffers");
    ok &= load(get, gl.deleteFramebuffers, "glDeleteFramebuffers");
    ok &= load(get, gl.framebufferTexture2D, "glFramebufferTexture2D");
    ok &= load(get, gl.checkFramebufferStatus, "glCheckFramebufferStatus");
    ok &= load(get, gl.readPixels, "glReadPixels");

    ok &= load(get, gl.createShader, "glCreateShader");
    ok &= load(get, gl.shaderSource, "glShaderSource");
    ok &= load(get, gl.compileShader, "glCompileShader");
    ok &= load(get, gl.getShaderiv, "glGetShaderiv");
    ok &= load(get, gl.getShaderInfoLog, "glGetShaderInfoLog");
    ok &= load(get, gl.deleteShader, "glDeleteShader");
    ok &= load(get, gl.createProgram, "glCreateProgram");
    ok &= load(get, gl.attachShader, "glAttachShader");
    ok &= load(get, gl.linkProgram, "glLinkProgram");
    ok &= load(get, gl.getProgramiv, "glGetProgramiv");
    ok &= load(get, gl.getProgramInfoLog, "glGetProgramInfoLog");
    ok &= load(get, gl.useProgram, "glUseProgram");
    ok &= load(get, gl.deleteProgram, "glDeleteProgram");
    ok &= load(get, gl.getUniformLocation, "glGetUniformLocation");
    ok &= load(get, gl.uniform1i, "glUniform1i");
    ok &= load(get, gl.uniform1f, "glUniform1f");
    ok &= load(get, gl.uniform2f, "glUniform2f");

    ok &= load(get, gl.genVertexArrays, "glGenVertexArrays");
    ok &= load(get, gl.bindVertexArray, "glBindVertexArray");
    ok &= load(get, gl.deleteVertexArrays, "glDeleteVertexArrays");
    ok &= load(get, gl.drawArrays, "glDrawArrays");

    ok &= load(get, gl.genBuffers, "glGenBuffers");
    ok &= load(get, gl.bindBuffer, "glBindBuffer");
    ok &= load(get, gl.bufferData, "glBufferData");
    ok &= load(get, gl.bufferSubData, "glBufferSubData");
    ok &= load(get, gl.deleteBuffers, "glDeleteBuffers");

    ok &= load(get, gl.genTextures, "glGenTextures");
    ok &= load(get, gl.bindTexture, "glBindTexture");
    ok &= load(get, gl.texStorage2D, "glTexStorage2D");
    ok &= load(get, gl.texSubImage2D, "glTexSubImage2D");
    ok &= load(get, gl.texParameteri, "glTexParameteri");
    ok &= load(get, gl.activeTexture, "glActiveTexture");
    ok &= load(get, gl.deleteTextures, "glDeleteTextures");

    if (!ok)
      return false;

    parseVersion(reinterpret_cast<const char*>(gl.getString(GL_VERSION)));

    bool compute = load(get, gl.bindBufferBase, "glBindBufferBase");
    compute &= load(get, gl.bindImageTexture, "glBindImageTexture");
    compute &= load(get, gl.dispatchCompute, "glDispatchCompute");
    compute &= load(get, gl.memoryBarrier, "glMemoryBarrier");
    g_has_compute = compute && g_version >= (g_is_es ? 301 : 403);
    return true;
  }

  int glVersion() {
    return g_version;
  }

  bool glIsEs() {
    return g_is_es;
  }

  bool glHasCompute() {
    return g_has_compute;
  }
}
