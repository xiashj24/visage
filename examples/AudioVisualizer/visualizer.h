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

#include "gl_procs.h"

#include <array>
#include <string>
#include <vector>

namespace viz {
  // Audio analysis and full-screen rendering, entirely outside visage. Both FFT
  // backends fill the same texture so the fragment shader is identical either
  // way and the two can be compared frame to frame.
  class Visualizer {
  public:
    static constexpr int kFftSize = 1024;
    static constexpr int kBins = kFftSize / 2;
    static constexpr int kChannels = 2;

    // Rows of the analysis texture. The compute kernel writes one channel per
    // workgroup, so no two workgroups touch the same texel.
    enum Row {
      kSpectrumLeft,
      kSpectrumRight,
      kWaveLeft,
      kWaveRight,
      kRows
    };

    enum class Backend {
      Cpu,
      Gpu
    };

    bool initialize();
    void shutdown();

    // Appends interleaved stereo frames to the analysis window.
    void pushSamples(const float* interleaved, size_t frames);

    // Runs each backend's main-thread block repeatedly over one window and
    // replaces the rolling averages with the result, so the two are comparable
    // without waiting for both to have been live.
    void measureBackends();

    // Analyses the current window with the selected backend and draws over the
    // whole drawable. Leaves the GL state visage's backend relies on untouched:
    // GL_BLEND stays enabled, scissor/cull/depth and the front face are not
    // changed, and only the blend function is set (which visage sets per draw).
    void render(int width, int height, float seconds);

    Backend backend() const { return backend_; }
    void setBackend(Backend backend) { backend_ = gpuAvailable() ? backend : Backend::Cpu; }

    bool gpuAvailable() const { return gpu_available_; }
    const std::string& status() const { return status_; }

    // Largest disagreement between the two backends over one shared window, as
    // a fraction of full scale. Negative when the comparison could not run
    // (reading back an R32F attachment is not universally supported).
    float kernelAgreement() const { return kernel_agreement_; }

    float amplitude() const { return amplitude_; }
    // Rolling average of the main-thread microseconds each backend's analysis
    // costs. The GPU figure excludes the dispatch's own execution, which is
    // the point: that work moves off the CPU.
    double cpuMicros() const { return cpu_micros_; }
    double gpuMicros() const { return gpu_micros_; }
    bool cpuMeasured() const { return cpu_measured_; }
    bool gpuMeasured() const { return gpu_measured_; }

  private:
    bool buildRenderProgram();
    bool buildComputeProgram();
    void createTexture();

    // Time-orders the ring into channel-major `window_` and returns the peak.
    float collectWindow();
    void analyseCpu(float attack, float release);
    void analyseGpu(float attack, float release);
    void computeNormFactor();
    void forwardFft(float* re, float* im) const;

    // Runs both backends over one synthetic window and records the largest
    // difference, so a broken kernel shows up as a number instead of as an
    // image that merely looks plausible.
    void verifyKernel();
    bool readTexture(std::vector<float>& out);

    GLuint render_program_ = 0;
    GLuint compute_program_ = 0;
    GLuint vao_ = 0;
    GLuint texture_ = 0;
    GLuint sample_buffer_ = 0;  // channel-major window, compute input
    GLuint envelope_buffer_ = 0;  // persistent smoothed spectrum
    GLint u_resolution_ = -1;
    GLint u_time_ = -1;
    GLint u_amplitude_ = -1;
    GLint u_audio_ = -1;
    GLint u_norm_factor_ = -1;
    GLint u_db_floor_ = -1;
    GLint u_attack_ = -1;
    GLint u_release_ = -1;

    bool gpu_available_ = false;
    Backend backend_ = Backend::Cpu;
    std::string status_;

    // Interleaved history, read back from the newest sample. Sized well beyond
    // one analysis window so a frame's read is never overtaken.
    static constexpr size_t kRingFrames = 8192;
    std::vector<float> ring_ = std::vector<float>(kRingFrames * kChannels, 0.0f);
    size_t ring_position_ = 0;

    std::array<float, kFftSize> hann_ {};
    std::array<int, kFftSize> bit_reversal_ {};
    // exp(-2i*pi*k/kFftSize); stage twiddles index this with a stride, so the
    // CPU path is not paying for trigonometry per butterfly.
    std::array<float, kBins> twiddle_real_ {};
    std::array<float, kBins> twiddle_imaginary_ {};
    std::vector<float> window_ = std::vector<float>(kChannels * kFftSize, 0.0f);
    std::vector<float> rows_ = std::vector<float>(kRows * kBins, 0.0f);
    std::array<float, kChannels * kBins> envelope_ {};
    float norm_factor_ = 1.0f;
    float amplitude_ = 0.0f;
    float kernel_agreement_ = -1.0f;

    double cpu_micros_ = 0.0;
    double gpu_micros_ = 0.0;
    bool cpu_measured_ = false;
    bool gpu_measured_ = false;
  };
}
