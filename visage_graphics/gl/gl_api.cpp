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

#include "gl_api.h"

#include "visage_utils/defines.h"

#include <cstring>
#include <string>

namespace visage {
  GlApi gl;

  namespace {
    bool g_loaded = false;
    bool g_context_es = false;
    std::string g_extensions;

    template<typename T>
    bool resolve(T& function, GlProcAddressGetter get_proc_address, const char* name) {
      function = reinterpret_cast<T>(get_proc_address(name));
      if (function == nullptr)
        VISAGE_LOG(std::string("Missing GL entry point: ") + name);
      return function != nullptr;
    }
  }

  bool loadGlApi(GlProcAddressGetter get_proc_address) {
    if (get_proc_address == nullptr)
      return false;

    bool ok = true;
    ok &= resolve(gl.enable, get_proc_address, "glEnable");
    ok &= resolve(gl.disable, get_proc_address, "glDisable");
    ok &= resolve(gl.frontFace, get_proc_address, "glFrontFace");
    ok &= resolve(gl.getError, get_proc_address, "glGetError");
    ok &= resolve(gl.getString, get_proc_address, "glGetString");
    ok &= resolve(gl.getStringi, get_proc_address, "glGetStringi");
    ok &= resolve(gl.getIntegerv, get_proc_address, "glGetIntegerv");
    ok &= resolve(gl.viewport, get_proc_address, "glViewport");
    ok &= resolve(gl.scissor, get_proc_address, "glScissor");
    ok &= resolve(gl.clearColor, get_proc_address, "glClearColor");
    ok &= resolve(gl.clear, get_proc_address, "glClear");
    ok &= resolve(gl.colorMask, get_proc_address, "glColorMask");
    ok &= resolve(gl.blendFuncSeparate, get_proc_address, "glBlendFuncSeparate");
    ok &= resolve(gl.blendEquationSeparate, get_proc_address, "glBlendEquationSeparate");
    ok &= resolve(gl.pixelStorei, get_proc_address, "glPixelStorei");
    ok &= resolve(gl.flush, get_proc_address, "glFlush");
    ok &= resolve(gl.finish, get_proc_address, "glFinish");

    ok &= resolve(gl.createShader, get_proc_address, "glCreateShader");
    ok &= resolve(gl.deleteShader, get_proc_address, "glDeleteShader");
    ok &= resolve(gl.shaderSource, get_proc_address, "glShaderSource");
    ok &= resolve(gl.compileShader, get_proc_address, "glCompileShader");
    ok &= resolve(gl.getShaderiv, get_proc_address, "glGetShaderiv");
    ok &= resolve(gl.getShaderInfoLog, get_proc_address, "glGetShaderInfoLog");
    ok &= resolve(gl.createProgram, get_proc_address, "glCreateProgram");
    ok &= resolve(gl.deleteProgram, get_proc_address, "glDeleteProgram");
    ok &= resolve(gl.attachShader, get_proc_address, "glAttachShader");
    ok &= resolve(gl.bindAttribLocation, get_proc_address, "glBindAttribLocation");
    ok &= resolve(gl.linkProgram, get_proc_address, "glLinkProgram");
    ok &= resolve(gl.getProgramiv, get_proc_address, "glGetProgramiv");
    ok &= resolve(gl.getProgramInfoLog, get_proc_address, "glGetProgramInfoLog");
    ok &= resolve(gl.useProgram, get_proc_address, "glUseProgram");
    ok &= resolve(gl.getUniformLocation, get_proc_address, "glGetUniformLocation");
    ok &= resolve(gl.uniform1i, get_proc_address, "glUniform1i");
    ok &= resolve(gl.uniform4fv, get_proc_address, "glUniform4fv");
    ok &= resolve(gl.uniformMatrix3fv, get_proc_address, "glUniformMatrix3fv");
    ok &= resolve(gl.uniformMatrix4fv, get_proc_address, "glUniformMatrix4fv");

    ok &= resolve(gl.genBuffers, get_proc_address, "glGenBuffers");
    ok &= resolve(gl.deleteBuffers, get_proc_address, "glDeleteBuffers");
    ok &= resolve(gl.bindBuffer, get_proc_address, "glBindBuffer");
    ok &= resolve(gl.bufferData, get_proc_address, "glBufferData");
    ok &= resolve(gl.genVertexArrays, get_proc_address, "glGenVertexArrays");
    ok &= resolve(gl.deleteVertexArrays, get_proc_address, "glDeleteVertexArrays");
    ok &= resolve(gl.bindVertexArray, get_proc_address, "glBindVertexArray");
    ok &= resolve(gl.enableVertexAttribArray, get_proc_address, "glEnableVertexAttribArray");
    ok &= resolve(gl.disableVertexAttribArray, get_proc_address, "glDisableVertexAttribArray");
    ok &= resolve(gl.vertexAttribPointer, get_proc_address, "glVertexAttribPointer");
    ok &= resolve(gl.drawElements, get_proc_address, "glDrawElements");
    ok &= resolve(gl.drawArrays, get_proc_address, "glDrawArrays");

    ok &= resolve(gl.genTextures, get_proc_address, "glGenTextures");
    ok &= resolve(gl.deleteTextures, get_proc_address, "glDeleteTextures");
    ok &= resolve(gl.bindTexture, get_proc_address, "glBindTexture");
    ok &= resolve(gl.activeTexture, get_proc_address, "glActiveTexture");
    ok &= resolve(gl.texImage2D, get_proc_address, "glTexImage2D");
    ok &= resolve(gl.texSubImage2D, get_proc_address, "glTexSubImage2D");
    ok &= resolve(gl.texParameteri, get_proc_address, "glTexParameteri");

    ok &= resolve(gl.genFramebuffers, get_proc_address, "glGenFramebuffers");
    ok &= resolve(gl.deleteFramebuffers, get_proc_address, "glDeleteFramebuffers");
    ok &= resolve(gl.bindFramebuffer, get_proc_address, "glBindFramebuffer");
    ok &= resolve(gl.framebufferTexture2D, get_proc_address, "glFramebufferTexture2D");
    ok &= resolve(gl.checkFramebufferStatus, get_proc_address, "glCheckFramebufferStatus");
    ok &= resolve(gl.blitFramebuffer, get_proc_address, "glBlitFramebuffer");
    ok &= resolve(gl.readPixels, get_proc_address, "glReadPixels");

    if (!ok)
      return false;

    const char* version = reinterpret_cast<const char*>(gl.getString(GL_VERSION));
    if (version == nullptr)
      return false;
    g_context_es = std::strncmp(version, "OpenGL ES", 9) == 0;

    // Cache the extension list; queried per-name later for caps fallbacks.
    g_extensions.clear();
    GLint num_extensions = 0;
    gl.getIntegerv(GL_NUM_EXTENSIONS, &num_extensions);
    for (GLint i = 0; i < num_extensions; ++i) {
      const char* extension = reinterpret_cast<const char*>(gl.getStringi(GL_EXTENSIONS, i));
      if (extension) {
        g_extensions += extension;
        g_extensions += ' ';
      }
    }

    g_loaded = true;
    return true;
  }

  bool glApiLoaded() {
    return g_loaded;
  }

  bool glContextIsEs() {
    return g_context_es;
  }

  bool glContextHasExtension(const char* name) {
    std::string token = std::string(name) + ' ';
    return g_extensions.find(token) != std::string::npos;
  }
}
