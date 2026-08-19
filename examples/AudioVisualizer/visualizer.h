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

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace visage {
  class Window;
  struct WindowRenderTarget;
}

namespace viz {
  // Audio analysis and full-screen rendering, entirely outside visage. The
  // analysis and the CPU FFT live here; the renderer, the GPU FFT and the
  // Resources they need come from visualizer_gl.cpp or visualizer_gpu.cpp,
  // whichever backend the build selected. Both drive the same panel, so the
  // CPU/GPU comparison reads the same either way.
  class Visualizer {
  public:
    static constexpr int kFftSize = 2048;
    static constexpr int kFftBits = 11;  // log2(kFftSize)
    static constexpr int kBins = kFftSize / 2;
    static constexpr int kChannels = 2;

    // Layout of the CPU analysis result. Each backend packs it into whatever
    // its GPU kernel writes, which is not the same texture on both.
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

    Visualizer();
    ~Visualizer();

    // `window` is the one being drawn into: the gl backend loads its entry
    // points through it, the SDL_GPU backend needs its swapchain format.
    bool initialize(visage::Window* window);
    void shutdown();

    // Appends interleaved stereo frames to the analysis window.
    void pushSamples(const float* interleaved, size_t frames);

    // Runs each backend's main-thread block repeatedly over one window and
    // replaces the rolling averages with the result, so the two are comparable
    // without waiting for both to have been live.
    void measureBackends();

    // Analyses the current window with the selected backend and draws over the
    // whole drawable. `target` is what the application renders into; on the gl
    // backend only its dimensions are filled in.
    void render(const visage::WindowRenderTarget& target, float seconds);

    Backend backend() const { return backend_; }
    void setBackend(Backend backend) { backend_ = gpuAvailable() ? backend : Backend::Cpu; }

    bool gpuAvailable() const { return gpu_available_; }
    const std::string& status() const { return status_; }

    // GPU FFT kernels this backend offers. The SDL_GPU backend has two, so the
    // packed real-to-complex transform and the plain complex one can be
    // compared live; the gl backend has the one.
    int kernelCount() const;
    const char* kernelName(int index) const;
    int kernel() const { return kernel_; }
    void setKernel(int index) { kernel_ = index; }
    // Fenced GPU time for one FFT, or negative where the backend cannot
    // measure it. Filled in by measureBackends().
    double kernelMicros(int index) const;

    // Largest disagreement between the two backends over one shared window, as
    // a fraction of full scale. Negative when the comparison could not run
    // (reading the analysis texture back is not universally supported).
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
    // GPU objects, defined by whichever backend file is compiled.
    struct Resources;

    // ---- implemented per backend ----
    bool initializeResources(visage::Window* window);
    void shutdownResources();
    // Uploads the CPU analysis result to the texture the renderer samples.
    void uploadRows();
    // Split so a batch of dispatches can be timed over one upload.
    void uploadGpuInput();
    void dispatchGpuKernel(float attack, float release);
    // Reads the analysis texture back in kRows layout; false when unsupported.
    bool readRows(std::vector<float>& out);
    void resetGpuEnvelope();
    // Brackets GPU work so a batch of it costs one submission. During a frame
    // the application records onto the target's command buffer and visage
    // submits it; off-frame - measuring, verifying - the backend uses one of
    // its own.
    void beginBatch(const visage::WindowRenderTarget* target = nullptr);
    void endBatch();
    void measureKernels();
    void draw(const visage::WindowRenderTarget& target, float seconds);

    // ---- shared ----
    void analyseGpu(float attack, float release) {
      uploadGpuInput();
      dispatchGpuKernel(attack, release);
    }

    // Time-orders the ring into channel-major `window_` and returns the peak.
    float collectWindow();
    void analyseCpu(float attack, float release);
    void computeNormFactor();
    void forwardFft(float* real, float* imaginary) const;

    // Runs both backends over one synthetic window and records the largest
    // difference, so a broken kernel shows up as a number instead of as an
    // image that merely looks plausible.
    void verifyKernel();

    std::unique_ptr<Resources> resources_;

    bool gpu_available_ = false;
    Backend backend_ = Backend::Cpu;
    int kernel_ = 0;
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
