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

// The OpenGL half of the visualizer: GLSL compiled at runtime against the
// context visage is drawing with, and a compute kernel where the context has
// them. The analysis texture is R32F with one row per channel per quantity,
// because GLES 3.1 guarantees only r32f as an image format.

#include "gl_procs.h"
#include "visualizer.h"
#include "visualizer_shared.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <visage_graphics/renderer.h>
#include <visage_windowing/windowing.h>

namespace viz {
  namespace {
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
uniform int u_rotation;  // screen rotation in quarter turns

const float kBins = 512.0;

// The application draws into the window surface, and visage's present pass
// rotates only the UI layer it composites on top - so without this the picture
// below it stays the way the panel is wired while the interface turns.
//
// This is the present pass's own mapping, not a guess at it: its screen corners
// ring BL, BR, TR, TL against texture corners in the same order, and a quarter
// turn shifts that ring, which for one turn puts the image's top edge along the
// screen's right edge. Feeding screen position in gives logical position back.
// Kept identical to viz_spectrum.frag, which the gpu backend uses instead.
vec2 logicalUv(vec2 uv, int turns) {
  if (turns == 1)
    return vec2(1.0 - uv.y, uv.x);
  if (turns == 2)
    return vec2(1.0 - uv.x, 1.0 - uv.y);
  if (turns == 3)
    return vec2(uv.y, 1.0 - uv.x);
  return uv;
}

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
  vec2 uv = logicalUv(gl_FragCoord.xy / u_resolution, u_rotation);
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

  struct Visualizer::Resources {
    GLuint render_program = 0;
    GLuint compute_program = 0;
    GLuint vao = 0;
    GLuint texture = 0;
    GLuint sample_buffer = 0;    // channel-major window, compute input
    GLuint envelope_buffer = 0;  // persistent smoothed spectrum
    GLint u_resolution = -1;
    GLint u_time = -1;
    GLint u_amplitude = -1;
    GLint u_rotation = -1;
    GLint u_audio = -1;
    GLint u_norm_factor = -1;
    GLint u_db_floor = -1;
    GLint u_attack = -1;
    GLint u_release = -1;
  };

  Visualizer::Visualizer() = default;
  Visualizer::~Visualizer() = default;

  bool Visualizer::initializeResources(visage::Window* window) {
    resources_ = std::make_unique<Resources>();

    if (!loadGlProcs(window->glProcAddressGetter())) {
      status_ = "could not load the OpenGL entry points the visualizer needs";
      return false;
    }

    std::string error;
    GLuint stages[2] = {};
    stages[0] = compileStage(GL_VERTEX_SHADER, renderHeader(), kVertexSource, error);
    stages[1] = compileStage(GL_FRAGMENT_SHADER, renderHeader(), kFragmentSource, error);
    if (stages[0] == 0 || stages[1] == 0) {
      status_ = "visualizer shader failed: " + error;
      return false;
    }

    resources_->render_program = linkProgram(stages, 2, error);
    gl.deleteShader(stages[0]);
    gl.deleteShader(stages[1]);
    if (resources_->render_program == 0) {
      status_ = "visualizer link failed: " + error;
      return false;
    }

    resources_->u_resolution = gl.getUniformLocation(resources_->render_program, "u_resolution");
    resources_->u_time = gl.getUniformLocation(resources_->render_program, "u_time");
    resources_->u_amplitude = gl.getUniformLocation(resources_->render_program, "u_amplitude");
    resources_->u_rotation = gl.getUniformLocation(resources_->render_program, "u_rotation");
    resources_->u_audio = gl.getUniformLocation(resources_->render_program, "s_audio");

    gl.genVertexArrays(1, &resources_->vao);

    gl.genTextures(1, &resources_->texture);
    gl.bindTexture(GL_TEXTURE_2D, resources_->texture);
    // Immutable storage: GLES 3.1 rejects mutable textures on image units.
    gl.texStorage2D(GL_TEXTURE_2D, 1, GL_R32F, kBins, kRows);
    gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    if (!glHasCompute()) {
      char message[160];
      std::snprintf(message, sizeof(message), "no compute shaders: %s %d.%d (needs %s)",
                    glIsEs() ? "GLES" : "GL", glVersion() / 100, glVersion() % 100,
                    glIsEs() ? "GLES 3.1" : "GL 4.3");
      status_ = message;
      return true;
    }

    GLuint stage = compileStage(GL_COMPUTE_SHADER, computeHeader(), kComputeSource, error);
    if (stage == 0) {
      status_ = "FFT kernel failed: " + error;
      return true;
    }

    resources_->compute_program = linkProgram(&stage, 1, error);
    gl.deleteShader(stage);
    if (resources_->compute_program == 0) {
      status_ = "FFT kernel link failed: " + error;
      return true;
    }

    GLint max_invocations = 0;
    gl.getIntegerv(GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS, &max_invocations);
    if (max_invocations < kThreads) {
      status_ = "FFT kernel needs 128 invocations per workgroup";
      gl.deleteProgram(resources_->compute_program);
      resources_->compute_program = 0;
      return true;
    }

    resources_->u_norm_factor = gl.getUniformLocation(resources_->compute_program, "u_norm_factor");
    resources_->u_db_floor = gl.getUniformLocation(resources_->compute_program, "u_db_floor");
    resources_->u_attack = gl.getUniformLocation(resources_->compute_program, "u_attack");
    resources_->u_release = gl.getUniformLocation(resources_->compute_program, "u_release");

    gl.genBuffers(1, &resources_->sample_buffer);
    gl.bindBuffer(GL_SHADER_STORAGE_BUFFER, resources_->sample_buffer);
    gl.bufferData(GL_SHADER_STORAGE_BUFFER, window_.size() * sizeof(float), nullptr, GL_DYNAMIC_DRAW);

    // Zeroed: the first frame's "previous" spectrum.
    std::vector<float> zeros(kChannels * kBins, 0.0f);
    gl.genBuffers(1, &resources_->envelope_buffer);
    gl.bindBuffer(GL_SHADER_STORAGE_BUFFER, resources_->envelope_buffer);
    gl.bufferData(GL_SHADER_STORAGE_BUFFER, zeros.size() * sizeof(float), zeros.data(), GL_DYNAMIC_DRAW);
    gl.bindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    gpu_available_ = true;
    return true;
  }

  void Visualizer::shutdownResources() {
    if (resources_ == nullptr)
      return;

    if (resources_->render_program)
      gl.deleteProgram(resources_->render_program);
    if (resources_->compute_program)
      gl.deleteProgram(resources_->compute_program);
    if (resources_->vao)
      gl.deleteVertexArrays(1, &resources_->vao);
    if (resources_->texture)
      gl.deleteTextures(1, &resources_->texture);
    if (resources_->sample_buffer)
      gl.deleteBuffers(1, &resources_->sample_buffer);
    if (resources_->envelope_buffer)
      gl.deleteBuffers(1, &resources_->envelope_buffer);

    resources_.reset();
    gpu_available_ = false;
  }

  // GL commands go straight onto the context, so there is no batch to open.
  void Visualizer::beginBatch(const visage::WindowRenderTarget*) { }
  void Visualizer::endBatch() { }

  void Visualizer::uploadRows() {
    gl.bindTexture(GL_TEXTURE_2D, resources_->texture);
    gl.texSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kBins, kRows, GL_RED, GL_FLOAT, rows_.data());
  }

  void Visualizer::uploadGpuInput() {
    gl.bindBuffer(GL_SHADER_STORAGE_BUFFER, resources_->sample_buffer);
    gl.bufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
                     static_cast<GLsizeiptr>(window_.size() * sizeof(float)), window_.data());
  }

  void Visualizer::dispatchGpuKernel(float attack, float release) {
    gl.useProgram(resources_->compute_program);
    gl.uniform1f(resources_->u_norm_factor, norm_factor_);
    gl.uniform1f(resources_->u_db_floor, kDbFloor);
    gl.uniform1f(resources_->u_attack, attack);
    gl.uniform1f(resources_->u_release, release);
    gl.bindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, resources_->sample_buffer);
    gl.bindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, resources_->envelope_buffer);
    gl.bindImageTexture(0, resources_->texture, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32F);
    gl.dispatchCompute(kChannels, 1, 1);
    gl.memoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
  }

  void Visualizer::resetGpuEnvelope() {
    std::vector<float> zeros(kChannels * kBins, 0.0f);
    gl.bindBuffer(GL_SHADER_STORAGE_BUFFER, resources_->envelope_buffer);
    gl.bufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
                     static_cast<GLsizeiptr>(zeros.size() * sizeof(float)), zeros.data());
    gl.bindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
  }

  bool Visualizer::readRows(std::vector<float>& out) {
    out.assign(kRows * kBins, 0.0f);

    GLuint framebuffer = 0;
    gl.genFramebuffers(1, &framebuffer);
    gl.bindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    gl.framebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, resources_->texture, 0);

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

  int Visualizer::kernelCount() const {
    return 1;
  }

  const char* Visualizer::kernelName(int) const {
    return "GL compute";
  }

  // GL timer queries are an extension GLES 3.1 does not require, so the
  // dispatch's own execution is not measured here.
  double Visualizer::kernelMicros(int) const {
    return -1.0;
  }

  void Visualizer::measureKernels() { }

  void Visualizer::draw(const visage::WindowRenderTarget& target, float seconds) {
    // A layer framebuffer is still bound, so bind the default one: that is
    // what visage composites its UI over.
    gl.bindFramebuffer(GL_FRAMEBUFFER, 0);
    gl.viewport(0, 0, target.width, target.height);
    // GL_BLEND stays on for visage; ONE/ZERO makes this draw opaque anyway.
    gl.blendFunc(GL_ONE, GL_ZERO);

    gl.useProgram(resources_->render_program);
    gl.uniform2f(resources_->u_resolution, static_cast<float>(target.width),
                 static_cast<float>(target.height));
    gl.uniform1f(resources_->u_time, seconds);
    gl.uniform1f(resources_->u_amplitude, std::min(amplitude_, 1.0f));
    // Resolution stays physical, matching gl_FragCoord; the shader rotates the
    // normalized coordinates it derives from them into the UI's logical space.
    gl.uniform1i(resources_->u_rotation,
                 visage::screenRotationQuarterTurns(visage::screenRotation()));
    gl.activeTexture(GL_TEXTURE0);
    gl.bindTexture(GL_TEXTURE_2D, resources_->texture);
    gl.uniform1i(resources_->u_audio, 0);

    gl.bindVertexArray(resources_->vao);
    gl.drawArrays(GL_TRIANGLES, 0, 3);
  }
}
