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

#include "shader_editor.h"

#include "embedded/fonts.h"
#include "embedded/icons.h"
#include "visage_utils/file_system.h"

namespace visage {
  inline int shaderEditTime(const std::string& file_path) {
    std::error_code error;
    auto edit_time = std::filesystem::last_write_time(file_path, error).time_since_epoch();
    return std::chrono::duration_cast<std::chrono::seconds>(edit_time).count();
  }

  void ShaderCompiler::compile(std::string shader_name, std::string code,
                               std::function<void(std::string)> callback) {
    // GL shader creation must happen where the context is current.
    runOnEventThread([shader_name = std::move(shader_name), code = std::move(code),
                      callback = std::move(callback)] {
      if (ShaderCache::swapShader(shader_name, code.c_str(), code.size())) {
        ProgramCache::refreshAllProgramsWithShader(shader_name);
        callback("");
      }
      else
        callback(ShaderCache::lastError());
    });
  }

  void ShaderCompiler::run() {
    while (!watched_edit_times_.empty() && shouldRun()) {
      sleep(100);
      for (auto& shader : watched_edit_times_) {
        if (!shouldRun())
          break;
        checkShaderForEdits(shader.first);
      }
    }
  }

  void ShaderCompiler::watchShaderFolder(const std::string& folder_path) {
    std::vector<File> files = searchForFiles(folder_path, ".glsl$");
    for (const auto& file : files)
      watched_edit_times_[file.string()] = shaderEditTime(file.string());

    if (!running())
      start();
  }

  void ShaderCompiler::checkShaderForEdits(const std::string& file_path) {
    int seconds = shaderEditTime(file_path);
    if (watched_edit_times_[file_path] < seconds) {
      watched_edit_times_[file_path] = seconds;
      std::string code = loadFileAsString(file_path);
      compile(fileStem(file_path), std::move(code), [](const std::string& error) {
        if (!error.empty())
          VISAGE_LOG(error);
      });
    }
  }

  ShaderEditor::ShaderEditor() {
    addChild(&editor_);
    addChild(&error_);
    addChild(&status_);
    editor_.setMultiLine(true);
    editor_.setMargin(15, 10);
    editor_.setFont(Font(16, fonts::DroidSansMono_ttf));
    editor_.setJustification(Font::kTopLeft);
    editor_.setDefaultText("No shader set");

    editor_.onTextChange() += [this] {
      if (shader_.data) {
        std::string text = editor_.text().toUtf8();
        auto callback = [this](const std::string& error) {
          error_.setText(error);
          status_.redraw();
          redraw();
        };
        compiler_.compile(shader_, text, callback);
      }
    };

    error_.setMultiLine(true);
    error_.setMargin(15, 10);
    error_.setFont(Font(16, fonts::DroidSansMono_ttf));
    error_.setJustification(Font::kTopLeft);
    error_.setActive(false);

    status_.onDraw() = [this](Canvas& canvas) {
      if (error_.text().isEmpty()) {
        canvas.setColor(0xff66ff66);
        canvas.svg(icons::check_circle_svg, 0.0f, 0.0f, status_.width(), status_.height());
      }
      else {
        canvas.setColor(0xffff6666);
        canvas.svg(icons::x_circle_svg, 0, 0, status_.width(), status_.height());
      }
    };
  }

  void ShaderEditor::draw(Canvas& canvas) {
    canvas.setColor(0xff222222);
    canvas.fill(0, 0, width(), height());
  }

  void ShaderEditor::resized() {
    int info_height = height() * kInfoHeightRatio;

    int editor_width = width() - 2 * kPaddingHeight;
    error_.setBounds(kPaddingHeight, height() - info_height - kPaddingHeight, editor_width, info_height);
    editor_.setBounds(kPaddingHeight, kPaddingHeight, editor_width, error_.y() - 2 * kPaddingHeight);

    int font_size = editor_.font().size();
    status_.setBounds(error_.right() - font_size - kPaddingHeight, error_.y() + kPaddingHeight,
                      font_size, font_size);
  }
}
