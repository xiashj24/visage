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

#include "visualizer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <SDL3/SDL_timer.h>

namespace viz {
  namespace {
    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kTwoPi = 2.0f * kPi;

    // Spectrum mapping, passed to the kernel as uniforms so the two backends
    // cannot drift apart.
    constexpr float kDbFloor = 80.0f;
    constexpr float kAttack = 0.5f;
    constexpr float kRelease = 0.2f;

    constexpr int kThreads = 128;  // GLES 3.1 only guarantees 128 invocations

    const char* renderHeader() {
      return glIsEs() ? "#version 300 es\nprecision highp float;\nprecision highp int;\n" :
                        "#version 330 core\n";
    }

    // Image variables need an explicit precision in ESSL and desktop GLSL does
    // not accept one on them, so the qualifier arrives as a macro.
    const char* computeHeader() {
      return glIsEs() ? "#version 310 es\nprecision highp float;\nprecision highp int;\n"
                        "#define VIZ_IMAGE_PRECISION highp\n" :
                        "#version 430 core\n#define VIZ_IMAGE_PRECISION\n";
    }

    const char* kVertexSource = R"GLSL(
void main() {
  // Single oversized triangle covering the whole drawable; no vertex buffer.
  vec2 corner = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
  gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

    const char* kFragmentSource = R"GLSL(
out vec4 fragColor;

uniform sampler2D s_audio;
uniform vec2 u_resolution;
uniform float u_time;
uniform float u_amplitude;

const float kBins = 512.0;

// Rows: 0/1 = smoothed spectrum L/R, 2/3 = waveform L/R. R32F is not
// filterable without an extension, so bins are fetched and blended by hand.
float audioRow(float bin, int row) {
  float clamped = clamp(bin, 0.0, kBins - 1.0);
  int low = int(clamped);
  int high = min(low + 1, int(kBins) - 1);
  float a = texelFetch(s_audio, ivec2(low, row), 0).r;
  float b = texelFetch(s_audio, ivec2(high, row), 0).r;
  return mix(a, b, clamped - float(low));
}

// Log-frequency axis: bin 2 to bin 480 spread evenly across the width.
float binForX(float x) {
  return exp2(mix(1.0, log2(480.0), x));
}

void main() {
  vec2 uv = gl_FragCoord.xy / u_resolution;
  float bin = binForX(uv.x);
  float left = audioRow(bin, 0);
  float right = audioRow(bin, 1);

  vec3 color = mix(vec3(0.015, 0.02, 0.05), vec3(0.05, 0.05, 0.12), uv.y);
  float middle = 1.0 - abs(uv.y - 0.5) * 2.0;
  color += vec3(0.07, 0.03, 0.13) * pow(middle, 3.0) * u_amplitude;

  vec3 hue = 0.5 + 0.5 * cos(6.2831853 * (uv.x * 0.55 + vec3(0.0, 0.33, 0.67) + 0.006 * u_time));

  // Spectrum mirrored around the middle: left channel up, right channel down.
  float top = 0.5 + left * 0.46;
  float bottom = 0.5 - right * 0.46;
  float fill = smoothstep(top, top - 0.01, uv.y) * step(0.5, uv.y) +
               smoothstep(bottom, bottom + 0.01, uv.y) * step(uv.y, 0.5);
  color += hue * fill * 0.5;
  color += hue * (exp(-abs(uv.y - top) * 240.0) + exp(-abs(uv.y - bottom) * 240.0)) * 1.5;

  float wave_left = 0.5 + audioRow(uv.x * kBins, 2) * 0.3;
  float wave_right = 0.5 + audioRow(uv.x * kBins, 3) * 0.3;
  color += vec3(0.6, 0.85, 1.0) * exp(-abs(uv.y - wave_left) * 400.0) * 0.7;
  color += vec3(1.0, 0.6, 0.85) * exp(-abs(uv.y - wave_right) * 400.0) * 0.7;

  vec2 centered = uv - 0.5;
  color *= 1.0 - dot(centered, centered) * 0.6;
  fragColor = vec4(color, 1.0);
}
)GLSL";

    // One workgroup per channel, so no two workgroups write the same texel.
    const char* kComputeSource = R"GLSL(
layout(local_size_x = 128) in;

const uint kFftSize = 1024u;
const uint kBins = 512u;
const uint kThreads = 128u;
const float kPi = 3.141592653589793;

layout(std430, binding = 0) readonly buffer SampleWindow { float samples[]; };
layout(std430, binding = 1) buffer Envelope { float envelope[]; };
layout(r32f, binding = 0) writeonly uniform VIZ_IMAGE_PRECISION image2D u_output;

uniform float u_norm_factor;
uniform float u_db_floor;
uniform float u_attack;
uniform float u_release;

shared vec2 s_data[1024];

void main() {
  uint channel = gl_WorkGroupID.x;
  uint thread = gl_LocalInvocationID.x;
  uint base = channel * kFftSize;

  // Bit-reversed windowed load, so the butterflies below run in place.
  for (uint i = thread; i < kFftSize; i += kThreads) {
    uint source = bitfieldReverse(i) >> 22u;
    float window = 0.5 - 0.5 * cos(2.0 * kPi * float(source) / float(kFftSize));
    s_data[i] = vec2(samples[base + source] * window, 0.0);
  }
  memoryBarrierShared();
  barrier();

  for (uint span = 2u; span <= kFftSize; span <<= 1u) {
    uint half_span = span >> 1u;
    for (uint b = thread; b < kBins; b += kThreads) {
      uint j = b & (half_span - 1u);
      uint low = ((b - j) << 1u) + j;
      float angle = -kPi * float(j) / float(half_span);
      vec2 twiddle = vec2(cos(angle), sin(angle));
      vec2 high = s_data[low + half_span];
      vec2 product = vec2(twiddle.x * high.x - twiddle.y * high.y,
                          twiddle.x * high.y + twiddle.y * high.x);
      vec2 even = s_data[low];
      s_data[low] = even + product;
      s_data[low + half_span] = even - product;
    }
    memoryBarrierShared();
    barrier();
  }

  for (uint bin = thread; bin < kBins; bin += kThreads) {
    float magnitude = length(s_data[bin]) * u_norm_factor;
    float decibels = 10.0 * log2(magnitude * magnitude + 1e-30) * 0.30103;
    float target = clamp((decibels + u_db_floor) / u_db_floor, 0.0, 1.0);

    uint slot = channel * kBins + bin;
    float previous = envelope[slot];
    float smoothed = previous +
                     (target - previous) * (target > previous ? u_attack : u_release);
    envelope[slot] = smoothed;

    imageStore(u_output, ivec2(int(bin), int(channel)), vec4(smoothed, 0.0, 0.0, 0.0));
    imageStore(u_output, ivec2(int(bin), int(channel) + 2),
               vec4(samples[base + kFftSize - kBins + bin], 0.0, 0.0, 0.0));
  }
}
)GLSL";

    double microseconds() {
      return static_cast<double>(SDL_GetPerformanceCounter()) * 1.0e6 /
             static_cast<double>(SDL_GetPerformanceFrequency());
    }

    void updateAverage(double& average, bool& seeded, double sample) {
      average = seeded ? average * 0.9 + sample * 0.1 : sample;
      seeded = true;
    }

    GLuint compileStage(GLenum stage, const char* header, const char* body, std::string& error) {
      GLuint shader = gl.createShader(stage);
      const GLchar* sources[] = { header, body };
      gl.shaderSource(shader, 2, sources, nullptr);
      gl.compileShader(shader);

      GLint compiled = 0;
      gl.getShaderiv(shader, GL_COMPILE_STATUS, &compiled);
      if (!compiled) {
        char log[2048] = {};
        gl.getShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
        error = log;
        gl.deleteShader(shader);
        return 0;
      }
      return shader;
    }

    GLuint linkProgram(const GLuint* stages, int count, std::string& error) {
      GLuint program = gl.createProgram();
      for (int i = 0; i < count; ++i)
        gl.attachShader(program, stages[i]);
      gl.linkProgram(program);

      GLint linked = 0;
      gl.getProgramiv(program, GL_LINK_STATUS, &linked);
      if (!linked) {
        char log[2048] = {};
        gl.getProgramInfoLog(program, sizeof(log) - 1, nullptr, log);
        error = log;
        gl.deleteProgram(program);
        return 0;
      }
      return program;
    }
  }

  bool Visualizer::initialize() {
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

    if (!buildRenderProgram())
      return false;

    gl.genVertexArrays(1, &vao_);
    createTexture();

    gpu_available_ = glHasCompute() && buildComputeProgram();
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
    else if (!glHasCompute()) {
      char message[160];
      std::snprintf(message, sizeof(message), "no compute shaders: %s %d.%d (needs %s)",
                    glIsEs() ? "GLES" : "GL", glVersion() / 100, glVersion() % 100,
                    glIsEs() ? "GLES 3.1" : "GL 4.3");
      status_ = message;
    }
    return true;
  }

  void Visualizer::shutdown() {
    if (render_program_)
      gl.deleteProgram(render_program_);
    if (compute_program_)
      gl.deleteProgram(compute_program_);
    if (vao_)
      gl.deleteVertexArrays(1, &vao_);
    if (texture_)
      gl.deleteTextures(1, &texture_);
    if (sample_buffer_)
      gl.deleteBuffers(1, &sample_buffer_);
    if (envelope_buffer_)
      gl.deleteBuffers(1, &envelope_buffer_);

    render_program_ = compute_program_ = vao_ = texture_ = 0;
    sample_buffer_ = envelope_buffer_ = 0;
  }

  bool Visualizer::buildRenderProgram() {
    std::string error;
    GLuint stages[2] = {};
    stages[0] = compileStage(GL_VERTEX_SHADER, renderHeader(), kVertexSource, error);
    stages[1] = compileStage(GL_FRAGMENT_SHADER, renderHeader(), kFragmentSource, error);
    if (stages[0] == 0 || stages[1] == 0) {
      status_ = "visualizer shader failed: " + error;
      return false;
    }

    render_program_ = linkProgram(stages, 2, error);
    gl.deleteShader(stages[0]);
    gl.deleteShader(stages[1]);
    if (render_program_ == 0) {
      status_ = "visualizer link failed: " + error;
      return false;
    }

    u_resolution_ = gl.getUniformLocation(render_program_, "u_resolution");
    u_time_ = gl.getUniformLocation(render_program_, "u_time");
    u_amplitude_ = gl.getUniformLocation(render_program_, "u_amplitude");
    u_audio_ = gl.getUniformLocation(render_program_, "s_audio");
    return true;
  }

  bool Visualizer::buildComputeProgram() {
    std::string error;
    GLuint stage = compileStage(GL_COMPUTE_SHADER, computeHeader(), kComputeSource, error);
    if (stage == 0) {
      status_ = "FFT kernel failed: " + error;
      return false;
    }

    compute_program_ = linkProgram(&stage, 1, error);
    gl.deleteShader(stage);
    if (compute_program_ == 0) {
      status_ = "FFT kernel link failed: " + error;
      return false;
    }

    GLint max_invocations = 0;
    gl.getIntegerv(GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS, &max_invocations);
    if (max_invocations < kThreads) {
      status_ = "FFT kernel needs 128 invocations per workgroup";
      gl.deleteProgram(compute_program_);
      compute_program_ = 0;
      return false;
    }

    u_norm_factor_ = gl.getUniformLocation(compute_program_, "u_norm_factor");
    u_db_floor_ = gl.getUniformLocation(compute_program_, "u_db_floor");
    u_attack_ = gl.getUniformLocation(compute_program_, "u_attack");
    u_release_ = gl.getUniformLocation(compute_program_, "u_release");

    gl.genBuffers(1, &sample_buffer_);
    gl.bindBuffer(GL_SHADER_STORAGE_BUFFER, sample_buffer_);
    gl.bufferData(GL_SHADER_STORAGE_BUFFER, window_.size() * sizeof(float), nullptr, GL_DYNAMIC_DRAW);

    // Zeroed: the first frame's "previous" spectrum.
    std::vector<float> zeros(kChannels * kBins, 0.0f);
    gl.genBuffers(1, &envelope_buffer_);
    gl.bindBuffer(GL_SHADER_STORAGE_BUFFER, envelope_buffer_);
    gl.bufferData(GL_SHADER_STORAGE_BUFFER, zeros.size() * sizeof(float), zeros.data(), GL_DYNAMIC_DRAW);
    gl.bindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    return true;
  }

  void Visualizer::createTexture() {
    gl.genTextures(1, &texture_);
    gl.bindTexture(GL_TEXTURE_2D, texture_);
    // Immutable storage: GLES 3.1 rejects mutable textures on image units.
    gl.texStorage2D(GL_TEXTURE_2D, 1, GL_R32F, kBins, kRows);
    gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
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

    gl.bindTexture(GL_TEXTURE_2D, texture_);
    gl.texSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kBins, kRows, GL_RED, GL_FLOAT, rows_.data());
  }

  void Visualizer::analyseGpu(float attack, float release) {
    gl.bindBuffer(GL_SHADER_STORAGE_BUFFER, sample_buffer_);
    gl.bufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
                     static_cast<GLsizeiptr>(window_.size() * sizeof(float)), window_.data());

    gl.useProgram(compute_program_);
    gl.uniform1f(u_norm_factor_, norm_factor_);
    gl.uniform1f(u_db_floor_, kDbFloor);
    gl.uniform1f(u_attack_, attack);
    gl.uniform1f(u_release_, release);
    gl.bindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, sample_buffer_);
    gl.bindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, envelope_buffer_);
    gl.bindImageTexture(0, texture_, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32F);
    gl.dispatchCompute(kChannels, 1, 1);
    gl.memoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
  }

  bool Visualizer::readTexture(std::vector<float>& out) {
    out.assign(kRows * kBins, 0.0f);

    GLuint framebuffer = 0;
    gl.genFramebuffers(1, &framebuffer);
    gl.bindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    gl.framebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture_, 0);

    bool complete = gl.checkFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (complete) {
      while (gl.getError() != GL_NO_ERROR) {
      }
      gl.readPixels(0, 0, kBins, kRows, GL_RED, GL_FLOAT, out.data());
      complete = gl.getError() == GL_NO_ERROR;
    }

    gl.bindFramebuffer(GL_FRAMEBUFFER, 0);
    gl.deleteFramebuffers(1, &framebuffer);
    return complete;
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
    std::vector<float> zeros(kChannels * kBins, 0.0f);
    gl.bindBuffer(GL_SHADER_STORAGE_BUFFER, envelope_buffer_);
    gl.bufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
                     static_cast<GLsizeiptr>(zeros.size() * sizeof(float)), zeros.data());

    analyseGpu(1.0f, 1.0f);
    std::vector<float> gpu_rows;
    if (!readTexture(gpu_rows))
      return;

    std::vector<float> cpu_rows;
    analyseCpu(1.0f, 1.0f);
    cpu_rows = rows_;

    float worst = 0.0f;
    for (size_t i = 0; i < cpu_rows.size(); ++i)
      worst = std::max(worst, std::abs(cpu_rows[i] - gpu_rows[i]));
    kernel_agreement_ = worst;

    // Leave no analysis state behind from the check.
    envelope_.fill(0.0f);
    gl.bindBuffer(GL_SHADER_STORAGE_BUFFER, envelope_buffer_);
    gl.bufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
                     static_cast<GLsizeiptr>(zeros.size() * sizeof(float)), zeros.data());
    gl.bindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    cpu_measured_ = gpu_measured_ = false;
  }

  void Visualizer::measureBackends() {
    constexpr int kIterations = 200;

    double start = microseconds();
    for (int i = 0; i < kIterations; ++i) {
      collectWindow();
      analyseCpu(kAttack, kRelease);
    }
    cpu_micros_ = (microseconds() - start) / kIterations;
    cpu_measured_ = true;

    if (!gpu_available_)
      return;

    start = microseconds();
    for (int i = 0; i < kIterations; ++i) {
      collectWindow();
      analyseGpu(kAttack, kRelease);
    }
    gpu_micros_ = (microseconds() - start) / kIterations;
    gpu_measured_ = true;
  }

  void Visualizer::render(int width, int height, float seconds) {
    double analysis_start = microseconds();
    amplitude_ = collectWindow();
    if (backend_ == Backend::Gpu) {
      analyseGpu(kAttack, kRelease);
      updateAverage(gpu_micros_, gpu_measured_, microseconds() - analysis_start);
    }
    else {
      analyseCpu(kAttack, kRelease);
      updateAverage(cpu_micros_, cpu_measured_, microseconds() - analysis_start);
    }

    gl.bindFramebuffer(GL_FRAMEBUFFER, 0);
    gl.viewport(0, 0, width, height);
    // GL_BLEND stays on for visage; ONE/ZERO makes this draw opaque anyway.
    gl.blendFunc(GL_ONE, GL_ZERO);

    gl.useProgram(render_program_);
    gl.uniform2f(u_resolution_, static_cast<float>(width), static_cast<float>(height));
    gl.uniform1f(u_time_, seconds);
    gl.uniform1f(u_amplitude_, std::min(amplitude_, 1.0f));
    gl.activeTexture(GL_TEXTURE0);
    gl.bindTexture(GL_TEXTURE_2D, texture_);
    gl.uniform1i(u_audio_, 0);

    gl.bindVertexArray(vao_);
    gl.drawArrays(GL_TRIANGLES, 0, 3);
  }
}
