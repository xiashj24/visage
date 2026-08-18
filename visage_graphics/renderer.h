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

#include "screenshot.h"
#include "visage_utils/space.h"

#include <string>

namespace visage {
  // Where an application renders content the UI is composited over, handed out
  // by ApplicationEditor::windowRenderTarget() during onDrawBackground(). On
  // the SDL_GPU backend these are the SDL_GPUCommandBuffer and SDL_GPUTexture
  // of the window's swapchain for this frame; the application records its
  // passes onto that command buffer and must not submit it.
  //
  // All null on the OpenGL backend, where the application binds the default
  // framebuffer itself.
  struct WindowRenderTarget {
    void* command_buffer = nullptr;
    void* texture = nullptr;
    int width = 0;
    int height = 0;
  };

  class Renderer {
  public:
    static Renderer& instance();

    Renderer() = default;

    // Loads GL through the injected getter against the caller's current GL
    // context. The application owns that context and the buffer swap; visage
    // never creates one. Safe to call more than once; only the first call
    // initializes.
    bool initialize(void* (*get_proc_address)(const char*));

    // Rendering without a window still needs a current GL context (e.g. from
    // a hidden window); initialize() must already have run.
    void initializeWindowless();

    // The SDL_GPUDevice rendering goes through, for claiming a window's
    // swapchain against it or drawing into it from an application. Null before
    // initialize(), and always null on the OpenGL backend.
    void* gpuDevice() const;

    // What is actually driving the GPU - the SDL_GPU driver name ("vulkan",
    // "metal") or the OpenGL flavour. For diagnostics; do not branch on it.
    const char* backendName() const;

    // Shader objects built from shadertool blobs, for an application drawing
    // into gpuDevice() with pipelines of its own: an SDL_GPUShader for a
    // graphics stage and an SDL_GPUComputePipeline for a kernel. Null on the
    // OpenGL backend, where the application compiles GLSL against its own
    // context.
    void* createGpuShader(const void* blob, int size) const;
    void destroyGpuShader(void* shader) const;
    void* createComputePipeline(const void* blob, int size) const;
    void destroyComputePipeline(void* pipeline) const;

    // Why the most recent of those returned null.
    const char* lastShaderError() const;

    // Tell the backend whether the context is gone, so it stops trying to
    // release GPU resources it can no longer reach. See bgfx::setContextLost().
    void setContextLost(bool lost) const;

    static void resetResolution(int width, int height) { }

    void setScreenshotData(const uint8_t* data, int width, int height, int pitch, bool blue_red);
    const Screenshot& screenshot() const { return screenshot_; }

    const std::string& errorMessage() const { return error_message_; }
    bool supported() const { return supported_; }
    bool swapChainSupported() const { return swap_chain_supported_; }
    bool initialized() const { return initialized_; }

  private:
    bool initialized_ = false;
    bool supported_ = false;
    bool swap_chain_supported_ = false;

    Screenshot screenshot_;
    std::string error_message_;
  };
}
