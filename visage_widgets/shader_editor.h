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

#include "text_editor.h"
#include "visage_graphics/graphics_caches.h"
#include "visage_ui/frame.h"
#include "visage_utils/thread_utils.h"

#include <map>
#include <string>

// For shader development purposes only.
// Not for production use.

namespace visage {
  // Shaders are plain GLSL compiled by the driver, so live editing is just
  // swapping new source text into the shader cache on the UI thread. The
  // thread only exists to poll watched shader files for edits.
  class ShaderCompiler : public Thread {
  public:
    ShaderCompiler() : Thread("Shader Compiler") { }
    ~ShaderCompiler() override { stop(); }

    // Compiles on the UI thread and reports the GL error log ("" on success).
    void compile(std::string shader_name, std::string code, std::function<void(std::string)> callback);

    void compile(const EmbeddedFile& shader, std::string code, std::function<void(std::string)> callback) {
      compile(shader.name, std::move(code), std::move(callback));
    }

    void run() override;
    void watchShaderFolder(const std::string& folder_path);

  private:
    void checkShaderForEdits(const std::string& file_path);

    std::map<std::string, int> watched_edit_times_;
  };

  class ShaderEditor : public Frame {
  public:
    static constexpr float kPaddingHeight = 8.0f;
    static constexpr float kInfoHeightRatio = 0.3f;

    ShaderEditor();

    void setShader(const EmbeddedFile& shader) {
      shader_ = shader;
      editor_.setText(std::string(reinterpret_cast<const char*>(shader_.data), shader_.size));
    }

    void draw(Canvas& canvas) override;
    void resized() override;

  private:
    ShaderCompiler compiler_;
    EmbeddedFile shader_;

    TextEditor error_;
    TextEditor editor_;
    Frame status_;
  };
}
