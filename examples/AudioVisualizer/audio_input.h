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

#include <vector>

struct SDL_AudioStream;

namespace viz {
  class Visualizer;

  // Stereo float audio into the visualizer, from the default recording device
  // where one opens and from a synthesized test signal otherwise, so the demo
  // has something to show on a machine with no input.
  class AudioInput {
  public:
    enum class Source {
      Device,
      TestSignal
    };

    ~AudioInput();

    // Opens the recording device; falls back to the test signal on failure.
    void initialize(Visualizer* visualizer);
    void shutdown();

    // Drives the test signal and drains the device. Call once per frame.
    void update(double delta_seconds);

    Source source() const { return source_; }
    void setSource(Source source) { source_ = deviceAvailable() ? source : Source::TestSignal; }
    bool deviceAvailable() const { return stream_ != nullptr; }
    float sampleRate() const { return sample_rate_; }

  private:
    void generateTestSignal(int frames);

    Visualizer* visualizer_ = nullptr;
    SDL_AudioStream* stream_ = nullptr;
    Source source_ = Source::TestSignal;
    float sample_rate_ = 48000.0f;
    double phase_ = 0.0;
    double mid_phase_ = 0.0;
    double sweep_ = 0.0;
    double beat_ = 0.0;
    std::vector<float> scratch_;
  };
}
