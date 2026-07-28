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

#include "graphics_utils.h"
#include "visage_file_embed/embedded_file.h"

#include <map>
#include <memory>
#include <string>

namespace visage {
  class Canvas;
  class ShaderTextureData;

  // A texture the application fills and hands to a Shader - an audio spectrum
  // for a reactive effect, most typically. Upload happens on the render thread
  // the next time the owning shader draws.
  class ShaderTexture {
  public:
    enum class Format { Rgba8, Red32f };

    ShaderTexture(int width, int height, Format format = Format::Rgba8);
    ~ShaderTexture();

    ShaderTexture(const ShaderTexture&) = delete;
    ShaderTexture& operator=(const ShaderTexture&) = delete;

    // Rows run top-down and must be width() * bytesPerPixel() long. Safe to
    // call from any thread; the bytes are copied.
    void setPixels(const void* data);

    int width() const { return width_; }
    int height() const { return height_; }
    int bytesPerPixel() const { return format_ == Format::Rgba8 ? 4 : 4; }

    // Render thread only.
    bgfx::TextureHandle textureHandle();

  private:
    std::unique_ptr<ShaderTextureData> data_;
    int width_ = 0;
    int height_ = 0;
    Format format_ = Format::Rgba8;

    VISAGE_LEAK_CHECKER(ShaderTexture)
  };

  class Shader {
  public:
    struct UniformData {
      float data[4] {};
    };

    Shader() = delete;
    Shader(const EmbeddedFile& vertex_shader, const EmbeddedFile& fragment_shader, BlendMode state) :
        vertex_shader_(vertex_shader), fragment_shader_(fragment_shader), state_(state) { }
    virtual ~Shader() = default;

    const EmbeddedFile& vertexShader() const { return vertex_shader_; }
    const EmbeddedFile& fragmentShader() const { return fragment_shader_; }
    BlendMode state() const { return state_; }

    void setUniformValue(const std::string& name, float value) {
      uniforms_[name] = { value, value, value, value };
    }
    void setUniformValue(const std::string& name, float value1, float value2, float value3,
                         float value4) {
      uniforms_[name] = { value1, value2, value3, value4 };
    }
    void removeUniform(const std::string& name) { uniforms_.erase(name); }
    const std::map<std::string, UniformData>& uniforms() const { return uniforms_; }

    // `sampler_name` is the sampler as the shader declares it - "s_texture" for
    // a Shadertoy effect's iChannel0. Binding slot is resolved from the name,
    // because slot alone is ambiguous: 0 is the gradient atlas for a library
    // shader quad and the effect's own texture for a third-party one.
    void setTexture(const std::string& sampler_name, ShaderTexture* texture) {
      textures_[sampler_name] = texture;
    }
    void removeTexture(const std::string& sampler_name) { textures_.erase(sampler_name); }
    const std::map<std::string, ShaderTexture*>& textures() const { return textures_; }

  private:
    EmbeddedFile vertex_shader_;
    EmbeddedFile fragment_shader_;
    BlendMode state_ = BlendMode::Alpha;
    std::map<std::string, UniformData> uniforms_;
    std::map<std::string, ShaderTexture*> textures_;
  };
}
