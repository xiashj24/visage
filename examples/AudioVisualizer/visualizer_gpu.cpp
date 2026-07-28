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

// The SDL_GPU half of the visualizer. It renders on visage's device but
// through pipelines of its own, recorded onto the command buffer that visage
// then composites its UI onto - so the two never touch each other's state.
//
// The analysis texture is RG32F, 512x2: row 0 = spectrum, row 1 = waveform,
// .x = left and .y = right. That is the layout both compute kernels write and
// what Shadertoy calls iChannel0, so a third-party effect could read it too.

#include "embedded/example_shaders.h"
#include "visualizer.h"
#include "visualizer_shared.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <SDL3/SDL_gpu.h>
#include <visage_graphics/renderer.h>
#include <visage_windowing/windowing.h>

namespace viz {
  namespace {
    constexpr int kTextureRows = 2;
    // One dispatch of one workgroup; the kernels split 512 threads across both
    // channels and keep the whole transform in shared memory.
    constexpr int kKernelCount = 2;

    struct KernelSource {
      const char* name;
      const visage::EmbeddedFile& blob;
    };

    const KernelSource kKernels[kKernelCount] = {
      { "R2C 512", resources::shaders::fft_r2c },
      { "complex 1024", resources::shaders::fft_complex },
    };

    // std140 layout of the kernels' Params block.
    struct ComputeParams {
      float norm_factor;
      float db_floor;
      float attack;
      float release;
    };

    // std140 layout of viz_spectrum.frag's Params block.
    struct DrawParams {
      float resolution[2];
      float time;
      float amplitude;
    };

    SDL_GPUBuffer* createBuffer(SDL_GPUDevice* device, SDL_GPUBufferUsageFlags usage, int floats) {
      SDL_GPUBufferCreateInfo info {};
      info.usage = usage;
      info.size = floats * sizeof(float);
      return SDL_CreateGPUBuffer(device, &info);
    }

    SDL_GPUTransferBuffer* createTransferBuffer(SDL_GPUDevice* device, int floats) {
      SDL_GPUTransferBufferCreateInfo info {};
      info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
      info.size = floats * sizeof(float);
      return SDL_CreateGPUTransferBuffer(device, &info);
    }
  }

  struct Visualizer::Resources {
    SDL_GPUDevice* device = nullptr;
    SDL_Window* window = nullptr;

    SDL_GPUTexture* audio_texture = nullptr;
    SDL_GPUSampler* sampler = nullptr;
    SDL_GPUTransferBuffer* rows_transfer = nullptr;
    SDL_GPUGraphicsPipeline* pipeline = nullptr;

    SDL_GPUBuffer* input_buffer = nullptr;     // time-ordered window, kernel input
    SDL_GPUBuffer* envelope_buffer = nullptr;  // persistent smoothed spectrum
    SDL_GPUTransferBuffer* input_transfer = nullptr;
    SDL_GPUComputePipeline* kernels[kKernelCount] {};
    double kernel_micros[kKernelCount] = { -1.0, -1.0 };

    // Null outside a batch. During a frame this is visage's own command
    // buffer, which visage submits; otherwise the batch owns it.
    SDL_GPUCommandBuffer* command_buffer = nullptr;
    bool owns_command_buffer = false;
  };

  Visualizer::Visualizer() = default;
  Visualizer::~Visualizer() = default;

  bool Visualizer::initializeResources(visage::Window* window) {
    resources_ = std::make_unique<Resources>();
    visage::Renderer& renderer = visage::Renderer::instance();
    Resources& resources = *resources_;

    resources.device = static_cast<SDL_GPUDevice*>(renderer.gpuDevice());
    resources.window = static_cast<SDL_Window*>(window->nativeHandle());
    if (resources.device == nullptr || resources.window == nullptr) {
      status_ = "no SDL_GPU device to render on";
      return false;
    }

    auto vertex = static_cast<SDL_GPUShader*>(
        renderer.createGpuShader(resources::shaders::viz_fullscreen.data,
                                 resources::shaders::viz_fullscreen.size));
    auto fragment = static_cast<SDL_GPUShader*>(
        renderer.createGpuShader(resources::shaders::viz_spectrum.data,
                                 resources::shaders::viz_spectrum.size));
    if (vertex == nullptr || fragment == nullptr) {
      status_ = "visualizer shaders failed to load";
      return false;
    }

    // visage claims a window's swapchain on the first frame, which may not
    // have happened yet; without it there is no format to build against.
    window->makeContextCurrent();

    SDL_GPUColorTargetDescription color_target {};
    color_target.format = SDL_GetGPUSwapchainTextureFormat(resources.device, resources.window);

    SDL_GPUGraphicsPipelineCreateInfo pipeline_info {};
    pipeline_info.vertex_shader = vertex;
    pipeline_info.fragment_shader = fragment;
    pipeline_info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pipeline_info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    pipeline_info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    pipeline_info.target_info.color_target_descriptions = &color_target;
    pipeline_info.target_info.num_color_targets = 1;
    resources.pipeline = SDL_CreateGPUGraphicsPipeline(resources.device, &pipeline_info);

    // The pipeline holds what it needs from here.
    renderer.destroyGpuShader(vertex);
    renderer.destroyGpuShader(fragment);
    if (resources.pipeline == nullptr) {
      status_ = std::string("visualizer pipeline failed: ") + SDL_GetError();
      return false;
    }

    // The kernels write the analysis texture directly, so it needs to be a
    // compute target as well as a sampled one.
    bool storage_writable = SDL_GPUTextureSupportsFormat(resources.device,
                                                         SDL_GPU_TEXTUREFORMAT_R32G32_FLOAT,
                                                         SDL_GPU_TEXTURETYPE_2D,
                                                         SDL_GPU_TEXTUREUSAGE_SAMPLER |
                                                             SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE);

    SDL_GPUTextureCreateInfo texture_info {};
    texture_info.type = SDL_GPU_TEXTURETYPE_2D;
    texture_info.format = SDL_GPU_TEXTUREFORMAT_R32G32_FLOAT;
    texture_info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    if (storage_writable)
      texture_info.usage |= SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
    texture_info.width = kBins;
    texture_info.height = kTextureRows;
    texture_info.layer_count_or_depth = 1;
    texture_info.num_levels = 1;
    resources.audio_texture = SDL_CreateGPUTexture(resources.device, &texture_info);

    // The shader fetches exact texels and blends them itself, so filtering
    // 32-bit float texels is never asked for.
    SDL_GPUSamplerCreateInfo sampler_info {};
    sampler_info.min_filter = SDL_GPU_FILTER_NEAREST;
    sampler_info.mag_filter = SDL_GPU_FILTER_NEAREST;
    sampler_info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sampler_info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    resources.sampler = SDL_CreateGPUSampler(resources.device, &sampler_info);

    resources.rows_transfer = createTransferBuffer(resources.device, kBins * kTextureRows * 2);
    if (resources.audio_texture == nullptr || resources.sampler == nullptr ||
        resources.rows_transfer == nullptr) {
      status_ = std::string("analysis texture failed: ") + SDL_GetError();
      return false;
    }

    if (!storage_writable) {
      status_ = "no compute FFT: RG32F storage writes unsupported";
      return true;
    }

    resources.input_buffer = createBuffer(resources.device, SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ,
                                          kChannels * kFftSize);
    resources.envelope_buffer = createBuffer(resources.device,
                                             SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ |
                                                 SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE,
                                             kChannels * kBins);
    resources.input_transfer = createTransferBuffer(resources.device, kChannels * kFftSize);
    if (resources.input_buffer == nullptr || resources.envelope_buffer == nullptr ||
        resources.input_transfer == nullptr) {
      status_ = std::string("compute FFT buffers failed: ") + SDL_GetError();
      return true;
    }

    for (int i = 0; i < kKernelCount; ++i) {
      resources.kernels[i] = static_cast<SDL_GPUComputePipeline*>(
          renderer.createComputePipeline(kKernels[i].blob.data, kKernels[i].blob.size));
      if (resources.kernels[i] == nullptr) {
        status_ = std::string("no compute FFT: ") + kKernels[i].name + ": " +
                  renderer.lastShaderError();
        return true;
      }
    }

    resetGpuEnvelope();
    gpu_available_ = true;
    return true;
  }

  void Visualizer::shutdownResources() {
    if (resources_ == nullptr)
      return;

    visage::Renderer& renderer = visage::Renderer::instance();
    Resources& resources = *resources_;
    if (resources.device) {
      for (SDL_GPUComputePipeline* kernel : resources.kernels)
        renderer.destroyComputePipeline(kernel);
      if (resources.pipeline)
        SDL_ReleaseGPUGraphicsPipeline(resources.device, resources.pipeline);
      if (resources.audio_texture)
        SDL_ReleaseGPUTexture(resources.device, resources.audio_texture);
      if (resources.sampler)
        SDL_ReleaseGPUSampler(resources.device, resources.sampler);
      if (resources.rows_transfer)
        SDL_ReleaseGPUTransferBuffer(resources.device, resources.rows_transfer);
      if (resources.input_buffer)
        SDL_ReleaseGPUBuffer(resources.device, resources.input_buffer);
      if (resources.envelope_buffer)
        SDL_ReleaseGPUBuffer(resources.device, resources.envelope_buffer);
      if (resources.input_transfer)
        SDL_ReleaseGPUTransferBuffer(resources.device, resources.input_transfer);
    }

    resources_.reset();
    gpu_available_ = false;
  }

  void Visualizer::beginBatch(const visage::WindowRenderTarget* target) {
    Resources& resources = *resources_;
    resources.owns_command_buffer = target == nullptr || target->command_buffer == nullptr;
    resources.command_buffer = resources.owns_command_buffer ?
                                   SDL_AcquireGPUCommandBuffer(resources.device) :
                                   static_cast<SDL_GPUCommandBuffer*>(target->command_buffer);
  }

  void Visualizer::endBatch() {
    Resources& resources = *resources_;
    if (resources.owns_command_buffer && resources.command_buffer)
      SDL_SubmitGPUCommandBuffer(resources.command_buffer);

    resources.command_buffer = nullptr;
    resources.owns_command_buffer = false;
  }

  void Visualizer::uploadRows() {
    Resources& resources = *resources_;
    auto texels = static_cast<float*>(SDL_MapGPUTransferBuffer(resources.device,
                                                               resources.rows_transfer, true));
    if (texels == nullptr)
      return;

    // kRows blocks of one channel each, into RG texels of both.
    for (int bin = 0; bin < kBins; ++bin) {
      texels[bin * 2 + 0] = rows_[kSpectrumLeft * kBins + bin];
      texels[bin * 2 + 1] = rows_[kSpectrumRight * kBins + bin];
      texels[(kBins + bin) * 2 + 0] = rows_[kWaveLeft * kBins + bin];
      texels[(kBins + bin) * 2 + 1] = rows_[kWaveRight * kBins + bin];
    }
    SDL_UnmapGPUTransferBuffer(resources.device, resources.rows_transfer);

    SDL_GPUTextureTransferInfo source {};
    source.transfer_buffer = resources.rows_transfer;
    source.pixels_per_row = kBins;
    source.rows_per_layer = kTextureRows;

    SDL_GPUTextureRegion destination {};
    destination.texture = resources.audio_texture;
    destination.w = kBins;
    destination.h = kTextureRows;
    destination.d = 1;

    SDL_GPUCopyPass* copy_pass = SDL_BeginGPUCopyPass(resources.command_buffer);
    SDL_UploadToGPUTexture(copy_pass, &source, &destination, true);
    SDL_EndGPUCopyPass(copy_pass);
  }

  void Visualizer::uploadGpuInput() {
    Resources& resources = *resources_;
    void* mapped = SDL_MapGPUTransferBuffer(resources.device, resources.input_transfer, true);
    if (mapped == nullptr)
      return;

    // `window_` is already the kernels' input layout: the L block, then the R
    // block, each time-ordered oldest to newest.
    std::memcpy(mapped, window_.data(), window_.size() * sizeof(float));
    SDL_UnmapGPUTransferBuffer(resources.device, resources.input_transfer);

    SDL_GPUTransferBufferLocation source {};
    source.transfer_buffer = resources.input_transfer;
    SDL_GPUBufferRegion destination {};
    destination.buffer = resources.input_buffer;
    destination.size = static_cast<Uint32>(window_.size() * sizeof(float));

    SDL_GPUCopyPass* copy_pass = SDL_BeginGPUCopyPass(resources.command_buffer);
    SDL_UploadToGPUBuffer(copy_pass, &source, &destination, true);
    SDL_EndGPUCopyPass(copy_pass);
  }

  void Visualizer::dispatchGpuKernel(float attack, float release) {
    Resources& resources = *resources_;
    SDL_GPUStorageTextureReadWriteBinding texture_binding {};
    texture_binding.texture = resources.audio_texture;
    texture_binding.cycle = true;  // Fully overwritten each dispatch.
    SDL_GPUStorageBufferReadWriteBinding envelope_binding {};
    envelope_binding.buffer = resources.envelope_buffer;
    envelope_binding.cycle = false;  // Smoothing state carries across frames.

    SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(resources.command_buffer, &texture_binding, 1,
                                                       &envelope_binding, 1);
    SDL_BindGPUComputePipeline(pass, resources.kernels[kernel_]);
    SDL_BindGPUComputeStorageBuffers(pass, 0, &resources.input_buffer, 1);
    const ComputeParams params { norm_factor_, kDbFloor, attack, release };
    SDL_PushGPUComputeUniformData(resources.command_buffer, 0, &params, sizeof(params));
    SDL_DispatchGPUCompute(pass, 1, 1, 1);
    SDL_EndGPUComputePass(pass);
  }

  void Visualizer::resetGpuEnvelope() {
    Resources& resources = *resources_;
    if (resources.envelope_buffer == nullptr)
      return;

    void* mapped = SDL_MapGPUTransferBuffer(resources.device, resources.input_transfer, false);
    if (mapped == nullptr)
      return;

    const Uint32 size = kChannels * kBins * sizeof(float);
    std::memset(mapped, 0, size);
    SDL_UnmapGPUTransferBuffer(resources.device, resources.input_transfer);

    SDL_GPUTransferBufferLocation source {};
    source.transfer_buffer = resources.input_transfer;
    SDL_GPUBufferRegion destination {};
    destination.buffer = resources.envelope_buffer;
    destination.size = size;

    SDL_GPUCommandBuffer* command_buffer = SDL_AcquireGPUCommandBuffer(resources.device);
    SDL_GPUCopyPass* copy_pass = SDL_BeginGPUCopyPass(command_buffer);
    SDL_UploadToGPUBuffer(copy_pass, &source, &destination, false);
    SDL_EndGPUCopyPass(copy_pass);
    SDL_SubmitGPUCommandBuffer(command_buffer);
  }

  bool Visualizer::readRows(std::vector<float>& out) {
    Resources& resources = *resources_;
    out.assign(kRows * kBins, 0.0f);

    const Uint32 size = kBins * kTextureRows * 2 * sizeof(float);
    SDL_GPUTransferBufferCreateInfo transfer_info {};
    transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    transfer_info.size = size;
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(resources.device, &transfer_info);
    if (transfer == nullptr)
      return false;

    SDL_GPUTextureRegion source {};
    source.texture = resources.audio_texture;
    source.w = kBins;
    source.h = kTextureRows;
    source.d = 1;
    SDL_GPUTextureTransferInfo destination {};
    destination.transfer_buffer = transfer;
    destination.pixels_per_row = kBins;
    destination.rows_per_layer = kTextureRows;

    SDL_GPUCommandBuffer* command_buffer = SDL_AcquireGPUCommandBuffer(resources.device);
    SDL_GPUCopyPass* copy_pass = SDL_BeginGPUCopyPass(command_buffer);
    SDL_DownloadFromGPUTexture(copy_pass, &source, &destination);
    SDL_EndGPUCopyPass(copy_pass);

    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command_buffer);
    SDL_WaitForGPUFences(resources.device, true, &fence, 1);
    SDL_ReleaseGPUFence(resources.device, fence);

    auto texels = static_cast<const float*>(SDL_MapGPUTransferBuffer(resources.device, transfer, false));
    if (texels) {
      for (int bin = 0; bin < kBins; ++bin) {
        out[kSpectrumLeft * kBins + bin] = texels[bin * 2 + 0];
        out[kSpectrumRight * kBins + bin] = texels[bin * 2 + 1];
        out[kWaveLeft * kBins + bin] = texels[(kBins + bin) * 2 + 0];
        out[kWaveRight * kBins + bin] = texels[(kBins + bin) * 2 + 1];
      }
      SDL_UnmapGPUTransferBuffer(resources.device, transfer);
    }
    SDL_ReleaseGPUTransferBuffer(resources.device, transfer);
    return texels != nullptr;
  }

  int Visualizer::kernelCount() const {
    return kKernelCount;
  }

  const char* Visualizer::kernelName(int index) const {
    return kKernels[index].name;
  }

  double Visualizer::kernelMicros(int index) const {
    return resources_ ? resources_->kernel_micros[index] : -1.0;
  }

  void Visualizer::measureKernels() {
    constexpr int kIterations = 200;
    Resources& resources = *resources_;
    int selected = kernel_;

    // SDL_GPU has no timestamp queries, so the transform is timed by fencing a
    // batch of dispatches: the GPU is drained first, and the wait afterwards
    // is what puts the kernel's own execution inside the elapsed time. One
    // upload feeds the whole batch, so what differs between the two figures is
    // the transform.
    for (int index = 0; index < kKernelCount; ++index) {
      kernel_ = index;
      beginBatch();
      uploadGpuInput();
      for (int i = 0; i < kIterations; ++i)
        dispatchGpuKernel(kAttack, kRelease);

      SDL_GPUCommandBuffer* command_buffer = resources.command_buffer;
      resources.command_buffer = nullptr;
      resources.owns_command_buffer = false;
      if (command_buffer == nullptr)
        continue;

      SDL_WaitForGPUIdle(resources.device);
      double start = microseconds();
      SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command_buffer);
      if (fence == nullptr)
        continue;

      SDL_WaitForGPUFences(resources.device, true, &fence, 1);
      resources.kernel_micros[index] = (microseconds() - start) / kIterations;
      SDL_ReleaseGPUFence(resources.device, fence);
    }

    kernel_ = selected;
    envelope_.fill(0.0f);
    resetGpuEnvelope();
  }

  void Visualizer::draw(const visage::WindowRenderTarget& target, float seconds) {
    Resources& resources = *resources_;
    if (target.texture == nullptr || resources.command_buffer == nullptr)
      return;

    // DONT_CARE, not LOAD: this covers the whole drawable. visage's own pass
    // is the one that loads, to composite the UI over what this leaves.
    SDL_GPUColorTargetInfo target_info {};
    target_info.texture = static_cast<SDL_GPUTexture*>(target.texture);
    target_info.load_op = SDL_GPU_LOADOP_DONT_CARE;
    target_info.store_op = SDL_GPU_STOREOP_STORE;

    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(resources.command_buffer, &target_info, 1, nullptr);
    SDL_BindGPUGraphicsPipeline(pass, resources.pipeline);

    SDL_GPUTextureSamplerBinding binding {};
    binding.texture = resources.audio_texture;
    binding.sampler = resources.sampler;
    SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);

    const DrawParams params { { static_cast<float>(target.width), static_cast<float>(target.height) },
                              seconds, std::min(amplitude_, 1.0f) };
    SDL_PushGPUFragmentUniformData(resources.command_buffer, 0, &params, sizeof(params));

    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
  }
}
