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

#include "shader.h"

#include <bgfx/bgfx.h>

#include <mutex>
#include <vector>
#include <cstring>

namespace visage {
  // The audio thread writes pixels while the render thread uploads them, so the
  // staging copy is mutex-guarded and only uploaded when it has changed.
  class ShaderTextureData {
  public:
    ShaderTextureData(int width, int height, int bytes_per_pixel) :
        pixels_(static_cast<size_t>(width) * height * bytes_per_pixel) { }

    ~ShaderTextureData() {
      if (bgfx::isValid(handle_))
        bgfx::destroy(handle_);
    }

    void setPixels(const void* data) {
      std::lock_guard<std::mutex> lock(mutex_);
      std::memcpy(pixels_.data(), data, pixels_.size());
      dirty_ = true;
    }

    bgfx::TextureHandle handle(int width, int height, bgfx::TextureFormat::Enum format) {
      if (!bgfx::isValid(handle_)) {
        handle_ = bgfx::createTexture2D(width, height, false, 1, format,
                                        BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
      }

      std::lock_guard<std::mutex> lock(mutex_);
      if (dirty_) {
        bgfx::updateTexture2D(handle_, 0, 0, 0, 0, width, height,
                              bgfx::copy(pixels_.data(), static_cast<uint32_t>(pixels_.size())));
        dirty_ = false;
      }
      return handle_;
    }

  private:
    std::mutex mutex_;
    std::vector<uint8_t> pixels_;
    bool dirty_ = false;
    bgfx::TextureHandle handle_ = BGFX_INVALID_HANDLE;
  };

  ShaderTexture::ShaderTexture(int width, int height, Format format) :
      width_(width), height_(height), format_(format) {
    data_ = std::make_unique<ShaderTextureData>(width, height, bytesPerPixel());
  }

  ShaderTexture::~ShaderTexture() = default;

  void ShaderTexture::setPixels(const void* data) {
    data_->setPixels(data);
  }

  bgfx::TextureHandle ShaderTexture::textureHandle() {
    bgfx::TextureFormat::Enum format = format_ == Format::Rgba8 ? bgfx::TextureFormat::RGBA8 :
                                                                 bgfx::TextureFormat::R32F;
    return data_->handle(width_, height_, format);
  }
}
