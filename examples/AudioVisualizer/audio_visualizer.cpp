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

// visage as an overlay: the application draws an audio visualizer into the
// window with programs of its own - including the FFT, as a compute shader
// where the device has them - and visage composites its UI on top.
//
// The three pieces that make that work:
//   setTransparentBackground(true)  areas the UI does not cover stay empty and
//                                   the finished frame composites over the
//                                   window instead of replacing it
//   onDrawBackground()              runs after the UI is drawn and before it
//                                   goes on screen; the visualizer draws there
//   windowRenderTarget()            what to draw into: the swapchain texture
//                                   and command buffer on SDL_GPU, and just a
//                                   size on OpenGL, where the visualizer binds
//                                   the default framebuffer itself
//
// The visualizer never touches visage's renderer, and visage never learns the
// visualizer exists.

#include "audio_input.h"
#include "embedded/example_fonts.h"
#include "visualizer.h"

#include <cstdio>
#include <SDL3/SDL_timer.h>
#include <visage/app.h>
#include <visage_widgets/button.h>

namespace {
  constexpr float kPanelWidth = 300.0f;
  constexpr float kRowHeight = 26.0f;
  constexpr float kMargin = 14.0f;
  constexpr float kButtonGap = 8.0f;

  double seconds() {
    return static_cast<double>(SDL_GetTicks()) / 1000.0;
  }

  std::string microsecondText(double micros, bool measured) {
    if (!measured)
      return "--";

    char text[32];
    std::snprintf(text, sizeof(text), "%.1f us", micros);
    return text;
  }
}

// An ordinary visage Frame: clipped, hovered and clicked like any other. Its
// translucent background is what makes the compositing visible.
class ControlPanel : public visage::Frame {
public:
  ControlPanel(viz::Visualizer& visualizer, viz::AudioInput& audio) :
      visualizer_(visualizer), audio_(audio), cpu_button_("CPU"), gpu_button_("GPU compute"),
      device_button_("Input"), signal_button_("Test tone"), measure_button_("Measure both (200x)"),
      title_font_(17, resources::fonts::Lato_Regular_ttf),
      font_(13, resources::fonts::Lato_Regular_ttf) {
    visage::Font button_font(14, resources::fonts::Lato_Regular_ttf);
    for (visage::UiButton* button :
         { &cpu_button_, &gpu_button_, &device_button_, &signal_button_, &measure_button_ }) {
      button->setFont(button_font);
      addChild(button);
    }

    // One button per kernel, when the backend has more than one to compare.
    for (int i = 0; visualizer_.kernelCount() > 1 && i < visualizer_.kernelCount(); ++i) {
      auto button = std::make_unique<visage::UiButton>(visualizer_.kernelName(i));
      button->setFont(button_font);
      button->onToggle() += [this, i](visage::Button*, bool) {
        visualizer_.setKernel(i);
        refreshButtons();
      };
      addChild(button.get());
      kernel_buttons_.push_back(std::move(button));
    }

    cpu_button_.onToggle() += [this](visage::Button*, bool) {
      visualizer_.setBackend(viz::Visualizer::Backend::Cpu);
      refreshButtons();
    };
    gpu_button_.onToggle() += [this](visage::Button*, bool) {
      visualizer_.setBackend(viz::Visualizer::Backend::Gpu);
      refreshButtons();
    };
    device_button_.onToggle() += [this](visage::Button*, bool) {
      audio_.setSource(viz::AudioInput::Source::Device);
      refreshButtons();
    };
    signal_button_.onToggle() += [this](visage::Button*, bool) {
      audio_.setSource(viz::AudioInput::Source::TestSignal);
      refreshButtons();
    };
    measure_button_.onToggle() += [this](visage::Button*, bool) { visualizer_.measureBackends(); };

    gpu_button_.setActive(visualizer_.gpuAvailable());
    device_button_.setActive(audio_.deviceAvailable());
    refreshButtons();
  }

  void refreshButtons() {
    cpu_button_.setActionButton(visualizer_.backend() == viz::Visualizer::Backend::Cpu);
    gpu_button_.setActionButton(visualizer_.backend() == viz::Visualizer::Backend::Gpu);
    device_button_.setActionButton(audio_.source() == viz::AudioInput::Source::Device);
    signal_button_.setActionButton(audio_.source() == viz::AudioInput::Source::TestSignal);
    for (size_t i = 0; i < kernel_buttons_.size(); ++i) {
      kernel_buttons_[i]->setActive(visualizer_.gpuAvailable());
      kernel_buttons_[i]->setActionButton(visualizer_.kernel() == static_cast<int>(i));
    }
    redraw();
  }

  void resized() override {
    float button_width = (width() - kMargin * 2.0f - kButtonGap) * 0.5f;
    float row = kMargin + 48.0f;
    cpu_button_.setBounds(kMargin, row, button_width, kRowHeight);
    gpu_button_.setBounds(kMargin + button_width + kButtonGap, row, button_width, kRowHeight);
    row += kRowHeight + kButtonGap;
    device_button_.setBounds(kMargin, row, button_width, kRowHeight);
    signal_button_.setBounds(kMargin + button_width + kButtonGap, row, button_width, kRowHeight);

    if (!kernel_buttons_.empty()) {
      row += kRowHeight + kButtonGap;
      float count = kernel_buttons_.size();
      float kernel_width = (width() - kMargin * 2.0f - kButtonGap * (count - 1.0f)) / count;
      for (size_t i = 0; i < kernel_buttons_.size(); ++i) {
        kernel_buttons_[i]->setBounds(kMargin + (kernel_width + kButtonGap) * i, row, kernel_width,
                                      kRowHeight);
      }
    }

    measure_button_.setBounds(kMargin, height() - kMargin - kRowHeight, width() - kMargin * 2.0f, kRowHeight);
  }

  // Where the readouts start: below the two or three rows of buttons.
  float readingsTop() const {
    return kMargin + 48.0f + (kRowHeight + kButtonGap) * (kernel_buttons_.empty() ? 2.0f : 3.0f) + 6.0f;
  }

  // Tall enough for the readings this backend has: one row per kernel it can
  // time, which is none on OpenGL.
  float preferredHeight() const {
    float readings = 80.0f + 20.0f * static_cast<float>(kernel_buttons_.size());
    return readingsTop() + readings + kButtonGap + kRowHeight + kMargin;
  }

  void draw(visage::Canvas& canvas) override {
    float text_width = width() - kMargin * 2.0f;

    // Translucent, so the visualizer underneath shows through the panel.
    canvas.setColor(0xc0121620);
    canvas.roundedRectangle(0, 0, width(), height(), 10);
    canvas.setColor(0x30ffffff);
    canvas.roundedRectangleBorder(0, 0, width(), height(), 10, 1);

    canvas.setColor(0xffffffff);
    canvas.text("GPU Audio Visualizer", title_font_, visage::Font::kLeft, kMargin, kMargin, text_width, 22);
    canvas.setColor(visualizer_.gpuAvailable() ? 0xff88ddaa : 0xffddbb66);
    canvas.text(visualizer_.status(), font_, visage::Font::kLeft, kMargin, kMargin + 21.0f, text_width, 18);

    char text[80];
    float row = readingsTop();
    canvas.setColor(0xffffffff);
    std::snprintf(text, sizeof(text), "%.0f FPS", static_cast<double>(frames_per_second_));
    canvas.text(text, font_, visage::Font::kLeft, kMargin, row, text_width, 18);
    std::snprintf(text, sizeof(text), "amplitude %.2f", static_cast<double>(visualizer_.amplitude()));
    canvas.text(text, font_, visage::Font::kRight, kMargin, row, text_width, 18);

    row += 22.0f;
    drawReading(canvas, row, "CPU analysis",
                microsecondText(visualizer_.cpuMicros(), visualizer_.cpuMeasured()));
    row += 20.0f;
    drawReading(canvas, row, "GPU analysis",
                microsecondText(visualizer_.gpuMicros(), visualizer_.gpuMeasured()));

    // Kernel time is the GPU's own, fenced: what the transform costs, as
    // opposed to what running it costs this thread.
    for (int i = 0; i < visualizer_.kernelCount(); ++i) {
      double micros = visualizer_.kernelMicros(i);
      if (micros < 0.0)
        continue;

      row += 20.0f;
      canvas.setColor(0xffaab4cc);
      drawReading(canvas, row, visualizer_.kernelName(i), microsecondText(micros, true));
    }

    row += 20.0f;
    if (visualizer_.cpuMeasured() && visualizer_.gpuMeasured()) {
      double saved = visualizer_.cpuMicros() - visualizer_.gpuMicros();
      std::snprintf(text, sizeof(text), "%.1f us (%.1f%% of 16.7ms)", saved,
                    saved * 100.0 / (1.0e6 / 60.0));
      canvas.setColor(0xff88ddaa);
      drawReading(canvas, row, "CPU saved", text);
    }
    else {
      canvas.setColor(0xff888888);
      drawReading(canvas, row, "CPU saved", "try both backends");
    }
  }

  void setFramesPerSecond(float fps) { frames_per_second_ = fps; }

private:
  void drawReading(visage::Canvas& canvas, float y, const std::string& label, const std::string& value) {
    float text_width = width() - kMargin * 2.0f;
    canvas.text(label, font_, visage::Font::kLeft, kMargin, y, text_width, 18);
    canvas.text(value, font_, visage::Font::kRight, kMargin, y, text_width, 18);
  }

  viz::Visualizer& visualizer_;
  viz::AudioInput& audio_;
  visage::UiButton cpu_button_;
  visage::UiButton gpu_button_;
  visage::UiButton device_button_;
  visage::UiButton signal_button_;
  visage::UiButton measure_button_;
  std::vector<std::unique_ptr<visage::UiButton>> kernel_buttons_;
  visage::Font title_font_;
  visage::Font font_;
  float frames_per_second_ = 0.0f;
};

class AudioVisualizerWindow : public visage::ApplicationWindow {
public:
  AudioVisualizerWindow() {
    setTitle("Visage Audio Visualizer");

    // The UI has no background of its own, so the window shows the visualizer
    // wherever the UI does not draw.
    setTransparentBackground(true);

    onDrawBackground() += [this] { drawVisualizer(); };
  }

  ~AudioVisualizerWindow() override {
    audio_.shutdown();
    visualizer_.shutdown();
  }

  // Call once the window is showing: the renderer exists by then, so the
  // visualizer can build its programs against it.
  bool initializeVisualizer() {
    if (!visualizer_.initialize(window())) {
      VISAGE_LOG(visualizer_.status().c_str());
      return false;
    }
    VISAGE_LOG(visualizer_.status().c_str());

    audio_.initialize(&visualizer_);
    // Seed both figures so the comparison is on screen from the first frame.
    visualizer_.measureBackends();
    panel_ = std::make_unique<ControlPanel>(visualizer_, audio_);
    addChild(panel_.get());
    resized();

    ready_ = true;
    last_time_ = seconds();
    last_fps_time_ = last_time_;
    return true;
  }

  void resized() override {
    if (panel_)
      panel_->setBounds(width() - kPanelWidth - 24.0f, 24.0f, kPanelWidth, panel_->preferredHeight());
  }

private:
  // Runs after visage has drawn its frame and before it is composited into the
  // window, so the visualizer draws underneath it.
  void drawVisualizer() {
    if (!ready_)
      return;

    double now = seconds();
    audio_.update(now - last_time_);
    last_time_ = now;
    visualizer_.render(windowRenderTarget(), static_cast<float>(now));

    frame_count_++;
    if (now - last_fps_time_ >= 0.5) {
      panel_->setFramesPerSecond(static_cast<float>(frame_count_ / (now - last_fps_time_)));
      last_fps_time_ = now;
      frame_count_ = 0;
    }
    // The readouts change every frame, so the panel repaints on the next one.
    panel_->redraw();
  }

  viz::Visualizer visualizer_;
  viz::AudioInput audio_;
  std::unique_ptr<ControlPanel> panel_;

  bool ready_ = false;
  double last_time_ = 0.0;
  double last_fps_time_ = 0.0;
  int frame_count_ = 0;
};

int runExample() {
  AudioVisualizerWindow window;
  // visage::setScreenRotation(visage::ScreenRotation::Rotate90);

  window.show(1100, 640);
  if (!window.initializeVisualizer())
    return 1;

  window.runEventLoop();
  return 0;
}
