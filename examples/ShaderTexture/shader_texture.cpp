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

// Feeds a per-frame texture to a shader, the path an audio-reactive effect
// uses. The signal here is synthetic so the example needs no audio device;
// swap fillSpectrum() for real FFT output and nothing else changes.

#include "embedded/example_shaders.h"

#include <cmath>
#include <visage/app.h>
#include <visage_widgets/shader_quad.h>

static constexpr int kBins = 512;
static constexpr int kRows = 2;  // Row 0 spectrum, row 1 waveform.

static void fillSpectrum(uint8_t* pixels, float time) {
  for (int x = 0; x < kBins; ++x) {
    float t = x / static_cast<float>(kBins - 1);

    float level = 0.0f;
    for (int partial = 1; partial <= 4; ++partial) {
      float centre = 0.08f * partial + 0.03f * sinf(time * 0.7f * partial);
      float width = 0.02f + 0.01f * partial;
      level += expf(-((t - centre) * (t - centre)) / (2.0f * width * width)) / partial;
    }
    level *= 0.55f + 0.45f * sinf(time * 1.3f);

    float wave = 0.5f + 0.35f * sinf(t * 40.0f + time * 4.0f) * sinf(t * 3.0f + time);

    uint8_t spectrum_byte = static_cast<uint8_t>(std::min(1.0f, std::max(0.0f, level)) * 255.0f);
    uint8_t wave_byte = static_cast<uint8_t>(std::min(1.0f, std::max(0.0f, wave)) * 255.0f);

    uint8_t* spectrum_row = pixels + x * 4;
    uint8_t* waveform_row = pixels + (kBins + x) * 4;
    spectrum_row[0] = spectrum_row[1] = spectrum_row[2] = spectrum_byte;
    spectrum_row[3] = 255;
    waveform_row[0] = waveform_row[1] = waveform_row[2] = wave_byte;
    waveform_row[3] = 255;
  }
}

int runExample() {
  visage::ApplicationWindow app;

  visage::ShaderTexture audio(kBins, kRows);
  visage::ShaderQuad spectrum(resources::shaders::vs_shader_quad,
                              resources::shaders::fs_spectrum, visage::BlendMode::Alpha);
  spectrum.shader().setTexture("s_texture", &audio);

  app.addChild(&spectrum);
  app.onResize() = [&app, &spectrum] {
    spectrum.setBounds(0, 0, app.width(), app.height());
  };

  std::vector<uint8_t> pixels(static_cast<size_t>(kBins) * kRows * 4);
  app.onDraw() = [&](visage::Canvas& canvas) {
    fillSpectrum(pixels.data(), canvas.time());
    audio.setPixels(pixels.data());

    // Hue drifts so the custom-uniform path is visible, not just present.
    float phase = canvas.time() * 0.6f;
    spectrum.shader().setUniformValue("u_spectrum_tint", 0.15f + 0.15f * sinf(phase),
                                      0.55f + 0.25f * sinf(phase + 2.0f), 0.95f, 1.0f);
    spectrum.shader().setUniformValue("u_waveform_tint", 1.0f, 0.55f, 0.2f, 1.0f);

    canvas.setColor(0xff0a0a14);
    canvas.fill(0, 0, app.width(), app.height());
  };

  app.setTitle("Visage Shader Texture Example");
  app.show(900, 500);
  app.runEventLoop();
  return 0;
}
