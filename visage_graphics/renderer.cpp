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

#include "renderer.h"

#include "gl/gl_api.h"
#include "visage_utils/string_utils.h"

#include <bgfx/bgfx.h>

namespace visage {
  Renderer& Renderer::instance() {
    static Renderer renderer;
    return renderer;
  }

  bool Renderer::initialize(void* (*get_proc_address)(const char*)) {
    if (initialized_)
      return supported_;

    if (!loadGlApi(get_proc_address) || !bgfx::initGlBackend()) {
      error_message_ = "Failed to load OpenGL 3.3 / OpenGL ES 3.0 functions.";
      VISAGE_LOG(error_message_.c_str());
      VISAGE_ASSERT(false);
      return false;
    }

    initialized_ = true;
    supported_ = true;
    swap_chain_supported_ = bgfx::getCaps()->supported & BGFX_CAPS_SWAP_CHAIN;

    VISAGE_LOG(String("GL_VENDOR: ") + reinterpret_cast<const char*>(gl.getString(GL_VENDOR)));
    VISAGE_LOG(String("GL_RENDERER: ") + reinterpret_cast<const char*>(gl.getString(GL_RENDERER)));
    VISAGE_LOG(String("GL_VERSION: ") + reinterpret_cast<const char*>(gl.getString(GL_VERSION)));
    VISAGE_LOG(String("GL_SHADING_LANGUAGE_VERSION: ") +
               reinterpret_cast<const char*>(gl.getString(GL_SHADING_LANGUAGE_VERSION)));
    return true;
  }

  void Renderer::initializeWindowless() {
    // The application must have initialized against some current GL context
    // already - a hidden window works; visage cannot create a context itself.
    VISAGE_ASSERT(initialized_);
  }

  void Renderer::setScreenshotData(const uint8_t* data, int width, int height, int pitch, bool blue_red) {
    screenshot_ = Screenshot(data, width, height, pitch, blue_red);
  }
}
