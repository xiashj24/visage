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

// The backend-independent half of the visualizer: the sample ring, the CPU
// FFT both backends are checked against, and the measurement that makes the
// two comparable.

#include "visualizer.h"

#include "visualizer_shared.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace viz {
  bool Visualizer::initialize(visage::Window* window) {
    for (int i = 0; i < kFftSize; ++i)
      hann_[i] = 0.5f - 0.5f * std::cos(kTwoPi * i / kFftSize);

    for (int i = 0; i < kFftSize; ++i) {
      int reversed = 0;
      for (int bit = 0; (1 << bit) < kFftSize; ++bit)
        reversed |= ((i >> bit) & 1) << (10 - 1 - bit);
      bit_reversal_[i] = reversed;
    }

    for (int k = 0; k < kBins; ++k) {
      twiddle_real_[k] = std::cos(-kTwoPi * k / kFftSize);
      twiddle_imaginary_[k] = std::sin(-kTwoPi * k / kFftSize);
    }

    computeNormFactor();

    if (!initializeResources(window))
      return false;

    if (gpu_available_) {
      backend_ = Backend::Gpu;
      verifyKernel();

      char message[160];
      if (kernel_agreement_ < 0.0f)
        std::snprintf(message, sizeof(message), "compute FFT ready (kernel check unavailable)");
      else
        std::snprintf(message, sizeof(message), "compute FFT ready, matches CPU to %.5f",
                      static_cast<double>(kernel_agreement_));
      status_ = message;
    }
    return true;
  }

  void Visualizer::shutdown() {
    shutdownResources();
  }

  void Visualizer::pushSamples(const float* interleaved, size_t frames) {
    for (size_t i = 0; i < frames; ++i) {
      ring_[ring_position_ * kChannels + 0] = interleaved[i * kChannels + 0];
      ring_[ring_position_ * kChannels + 1] = interleaved[i * kChannels + 1];
      ring_position_ = (ring_position_ + 1) % kRingFrames;
    }
  }

  float Visualizer::collectWindow() {
    size_t start = (ring_position_ + kRingFrames - kFftSize) % kRingFrames;
    float peak = 0.0f;
    for (int i = 0; i < kFftSize; ++i) {
      size_t frame = (start + i) % kRingFrames;
      float left = ring_[frame * kChannels + 0];
      float right = ring_[frame * kChannels + 1];
      window_[i] = left;
      window_[kFftSize + i] = right;
      peak = std::max(peak, std::max(std::abs(left), std::abs(right)));
    }
    return peak;
  }

  void Visualizer::forwardFft(float* real, float* imaginary) const {
    for (int span = 2; span <= kFftSize; span <<= 1) {
      int half_span = span >> 1;
      int stride = kFftSize / span;
      for (int start = 0; start < kFftSize; start += span) {
        for (int j = 0; j < half_span; ++j) {
          int low = start + j;
          int high = low + half_span;
          float twiddle_real = twiddle_real_[j * stride];
          float twiddle_imaginary = twiddle_imaginary_[j * stride];
          float product_real = twiddle_real * real[high] - twiddle_imaginary * imaginary[high];
          float product_imaginary = twiddle_real * imaginary[high] + twiddle_imaginary * real[high];
          real[high] = real[low] - product_real;
          imaginary[high] = imaginary[low] - product_imaginary;
          real[low] += product_real;
          imaginary[low] += product_imaginary;
        }
      }
    }
  }

  void Visualizer::computeNormFactor() {
    // A full-scale tone at fs/4 should land near 1.0 after windowing.
    std::array<float, kFftSize> real {}, imaginary {};
    for (int i = 0; i < kFftSize; ++i) {
      int source = bit_reversal_[i];
      real[i] = std::sin(kTwoPi * 0.25f * source) * hann_[source];
    }
    forwardFft(real.data(), imaginary.data());

    float peak = 0.0f;
    for (int bin = 0; bin < kBins; ++bin)
      peak = std::max(peak, std::sqrt(real[bin] * real[bin] + imaginary[bin] * imaginary[bin]));
    norm_factor_ = peak > 0.0f ? 1.0f / peak : 1.0f;
  }

  void Visualizer::analyseCpu(float attack, float release) {
    for (int channel = 0; channel < kChannels; ++channel) {
      const float* samples = window_.data() + channel * kFftSize;
      std::array<float, kFftSize> real {}, imaginary {};
      for (int i = 0; i < kFftSize; ++i) {
        int source = bit_reversal_[i];
        real[i] = samples[source] * hann_[source];
      }
      forwardFft(real.data(), imaginary.data());

      float* spectrum = rows_.data() + (kSpectrumLeft + channel) * kBins;
      float* wave = rows_.data() + (kWaveLeft + channel) * kBins;
      for (int bin = 0; bin < kBins; ++bin) {
        float magnitude = std::sqrt(real[bin] * real[bin] + imaginary[bin] * imaginary[bin]) * norm_factor_;
        float decibels = 10.0f * std::log10(magnitude * magnitude + 1e-30f);
        float target = std::max(0.0f, std::min(1.0f, (decibels + kDbFloor) / kDbFloor));
        float& previous = envelope_[channel * kBins + bin];
        previous += (target - previous) * (target > previous ? attack : release);
        spectrum[bin] = previous;
        wave[bin] = samples[kFftSize - kBins + bin];
      }
    }
  }

  void Visualizer::verifyKernel() {
    // A spread of tones plus an impulse: exercises every bin and every stage
    // rather than the handful a steady tone would light up.
    for (int channel = 0; channel < kChannels; ++channel) {
      for (int i = 0; i < kFftSize; ++i) {
        float value = 0.0f;
        for (int harmonic = 1; harmonic <= 6; ++harmonic)
          value += std::sin(kTwoPi * (harmonic * 13 + channel * 5) * i / kFftSize) / harmonic;
        window_[channel * kFftSize + i] = value * 0.2f + (i == 37 ? 0.5f : 0.0f);
      }
    }

    // Smoothing off, so this compares the transforms and not the envelopes.
    envelope_.fill(0.0f);
    resetGpuEnvelope();

    beginBatch();
    analyseGpu(1.0f, 1.0f);
    endBatch();

    std::vector<float> gpu_rows;
    if (!readRows(gpu_rows))
      return;

    analyseCpu(1.0f, 1.0f);

    float worst = 0.0f;
    for (size_t i = 0; i < rows_.size(); ++i)
      worst = std::max(worst, std::abs(rows_[i] - gpu_rows[i]));
    kernel_agreement_ = worst;

    // Leave no analysis state behind from the check.
    envelope_.fill(0.0f);
    resetGpuEnvelope();
    cpu_measured_ = gpu_measured_ = false;
  }

  void Visualizer::measureBackends() {
    constexpr int kIterations = 200;

    beginBatch();
    double start = microseconds();
    for (int i = 0; i < kIterations; ++i) {
      collectWindow();
      analyseCpu(kAttack, kRelease);
      uploadRows();
    }
    cpu_micros_ = (microseconds() - start) / kIterations;
    cpu_measured_ = true;
    endBatch();

    if (!gpu_available_)
      return;

    beginBatch();
    start = microseconds();
    for (int i = 0; i < kIterations; ++i) {
      collectWindow();
      analyseGpu(kAttack, kRelease);
    }
    gpu_micros_ = (microseconds() - start) / kIterations;
    gpu_measured_ = true;
    endBatch();

    measureKernels();
  }

  void Visualizer::render(const visage::WindowRenderTarget& target, float seconds) {
    beginBatch(&target);

    double analysis_start = microseconds();
    amplitude_ = collectWindow();
    if (backend_ == Backend::Gpu) {
      analyseGpu(kAttack, kRelease);
      updateAverage(gpu_micros_, gpu_measured_, microseconds() - analysis_start);
    }
    else {
      analyseCpu(kAttack, kRelease);
      uploadRows();
      updateAverage(cpu_micros_, cpu_measured_, microseconds() - analysis_start);
    }

    draw(target, seconds);
    endBatch();
  }
}
