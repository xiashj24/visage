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

#include "bgfx/bgfx.h"

#include "embedded/shaders.h"
#include "renderer.h"
#include "visage_utils/defines.h"
#include "visage_utils/string_utils.h"

#include <SDL3/SDL.h>

#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bgfx {
  using namespace visage;

  namespace {
    // 1-based slot indices so a value-initialized handle (0) is invalid.
    template<typename T>
    class SlotTable {
    public:
      uint16_t create(T value) {
        if (!free_slots_.empty()) {
          uint16_t idx = free_slots_.back();
          free_slots_.pop_back();
          slots_[idx - 1] = std::move(value);
          return idx;
        }
        slots_.push_back(std::move(value));
        return static_cast<uint16_t>(slots_.size());
      }

      T& get(uint16_t idx) {
        VISAGE_ASSERT(idx != kInvalidHandle && idx <= slots_.size());
        return slots_[idx - 1];
      }

      void release(uint16_t idx) { free_slots_.push_back(idx); }

    private:
      std::vector<T> slots_;
      std::vector<uint16_t> free_slots_;
    };

    // ---- shadertool blob ----------------------------------------------------

    constexpr uint32_t kBlobMagic = 0x53475356;  // "VSGS"
    constexpr uint32_t kBlobVersion = 3;

    enum class BlobStage : uint32_t { Vertex, Fragment, Compute };

    struct ParsedBlob {
      bool valid = false;
      BlobStage stage = BlobStage::Vertex;
      uint32_t num_samplers = 0;
      uint32_t num_uniform_buffers = 0;
      std::vector<std::string> uniform_names;
      std::vector<std::string> sampler_names;
      // Compute only, zero otherwise.
      uint32_t num_readonly_storage_textures = 0;
      uint32_t num_readonly_storage_buffers = 0;
      uint32_t num_readwrite_storage_textures = 0;
      uint32_t num_readwrite_storage_buffers = 0;
      uint32_t threadcount[3] = { 1, 1, 1 };
      const uint8_t* code = nullptr;
      uint32_t code_size = 0;
      SDL_GPUShaderFormat format = SDL_GPU_SHADERFORMAT_INVALID;

      bool isFragment() const { return stage == BlobStage::Fragment; }
      // SPIRV-Cross renames main to main0 on the way to MSL.
      const char* entrypoint() const {
        return format == SDL_GPU_SHADERFORMAT_MSL ? "main0" : "main";
      }
    };

    std::vector<std::string> readNames(const uint8_t*& cursor, uint32_t count) {
      std::vector<std::string> names;
      names.reserve(count);
      for (uint32_t i = 0; i < count; ++i) {
        uint8_t length = *cursor++;
        names.emplace_back(reinterpret_cast<const char*>(cursor), length);
        cursor += length;
      }
      return names;
    }

    ParsedBlob parseBlob(const uint8_t* data, uint32_t size, SDL_GPUShaderFormat available) {
      ParsedBlob blob;
      constexpr uint32_t kHeaderWords = 18;
      if (size < kHeaderWords * sizeof(uint32_t))
        return blob;

      uint32_t header[kHeaderWords];
      std::memcpy(header, data, sizeof(header));
      if (header[0] != kBlobMagic || header[1] != kBlobVersion)
        return blob;

      blob.stage = static_cast<BlobStage>(header[2]);
      blob.num_samplers = header[3];
      blob.num_uniform_buffers = header[4];
      blob.num_readonly_storage_textures = header[11];
      blob.num_readonly_storage_buffers = header[12];
      blob.num_readwrite_storage_textures = header[13];
      blob.num_readwrite_storage_buffers = header[14];
      blob.threadcount[0] = header[15];
      blob.threadcount[1] = header[16];
      blob.threadcount[2] = header[17];

      const uint8_t* cursor = data + sizeof(header);
      blob.uniform_names = readNames(cursor, header[5]);
      cursor = data + sizeof(header) + header[6];
      blob.sampler_names = readNames(cursor, header[7]);

      uint32_t names_end = sizeof(header) + header[6] + header[8];
      uint32_t spirv_offset = (names_end + 3u) & ~3u;  // SPIR-V must stay 4-byte aligned.
      uint32_t spirv_size = header[9];
      uint32_t msl_size = header[10];

      if (available & SDL_GPU_SHADERFORMAT_SPIRV) {
        blob.code = data + spirv_offset;
        blob.code_size = spirv_size;
        blob.format = SDL_GPU_SHADERFORMAT_SPIRV;
      }
      else if (available & SDL_GPU_SHADERFORMAT_MSL) {
        blob.code = data + spirv_offset + spirv_size;
        blob.code_size = msl_size;
        blob.format = SDL_GPU_SHADERFORMAT_MSL;
      }
      else
        return blob;

      blob.valid = spirv_offset + spirv_size + msl_size <= size;
      return blob;
    }

    // ---- resources ----------------------------------------------------------

    struct ShaderResource {
      SDL_GPUShader* shader = nullptr;
      bool is_fragment = false;
      std::vector<std::string> uniform_names;
      std::vector<std::string> sampler_names;
      bool has_uniform_buffer = false;
    };

    using PipelineKey = std::tuple<uint64_t, int, const VertexLayout*>;

    struct ProgramResource {
      ShaderHandle vertex_shader;
      ShaderHandle fragment_shader;
      std::map<PipelineKey, SDL_GPUGraphicsPipeline*> pipelines;
    };

    struct UniformResource {
      std::string name;
      UniformType::Enum type = UniformType::Vec4;
    };

    struct TextureResource {
      SDL_GPUTexture* texture = nullptr;
      SDL_GPUSampler* sampler = nullptr;
      TextureFormat::Enum format = TextureFormat::RGBA8;
      uint16_t width = 0;
      uint16_t height = 0;
    };

    struct FrameBufferResource {
      TextureHandle color_texture;
      TextureFormat::Enum format = TextureFormat::RGBA8;
      uint16_t width = 0;
      uint16_t height = 0;
      // The window whose swapchain presentFrameBuffer() draws this into; null
      // for an offscreen target.
      SDL_Window* window = nullptr;
      // Cleared once so the first LOADOP_LOAD reads defined contents.
      bool initialized = false;
    };

    struct BufferResource {
      SDL_GPUBuffer* buffer = nullptr;
      uint32_t size = 0;
      uint32_t count = 0;
      bool index32 = false;
      // Vertex buffers carry their layout: the pipeline is keyed on it and
      // setVertexBuffer(handle) is the only place it can come from.
      VertexLayout layout;
    };

    constexpr int kMaxTextureStages = 4;

    struct PendingTexture {
      bool bound = false;
      std::string sampler_name;
      TextureHandle texture;
    };

    struct UniformValue {
      UniformType::Enum type = UniformType::Vec4;
      float data[16] = {};
    };

    constexpr uint64_t kDefaultState = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A |
                                       BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_ZERO);

    struct DrawState {
      uint64_t state = kDefaultState;
      PendingTexture textures[kMaxTextureStages];

      bool has_vertex_buffer = false;
      VertexBufferHandle vertex_buffer;
      const VertexLayout* vertex_layout = nullptr;
      const uint8_t* pending_vertex_data = nullptr;
      uint32_t pending_vertex_size = 0;

      bool has_index_buffer = false;
      IndexBufferHandle index_buffer;
      bool index32 = false;
      uint32_t index_count = 0;
      const uint8_t* pending_index_data = nullptr;
      uint32_t pending_index_size = 0;

      void reset() {
        state = kDefaultState;
        for (auto& texture : textures)
          texture.bound = false;
        has_vertex_buffer = false;
        vertex_buffer = {};
        pending_vertex_data = nullptr;
        has_index_buffer = false;
        index_buffer = {};
        pending_index_data = nullptr;
      }
    };

    // Recorded at submit(), replayed in frame(). SDL_GPU forbids uploading
    // inside a render pass, so nothing can execute until the whole frame's
    // transient data is known.
    struct DrawCommand {
      ProgramHandle program;
      uint64_t state = kDefaultState;
      FrameBufferHandle target;
      uint16_t view_width = 0;
      uint16_t view_height = 0;

      VertexBufferHandle vertex_buffer;  // Invalid means the transient arena.
      uint32_t vertex_offset = 0;
      const VertexLayout* layout = nullptr;

      IndexBufferHandle index_buffer;
      uint32_t index_offset = 0;
      uint32_t index_count = 0;
      bool index32 = false;

      uint32_t vertex_uniform_offset = 0;
      uint32_t vertex_uniform_size = 0;
      uint32_t fragment_uniform_offset = 0;
      uint32_t fragment_uniform_size = 0;

      SDL_GPUTextureSamplerBinding samplers[kMaxTextureStages] {};
      uint32_t num_samplers = 0;
    };

    struct BackendState {
      bool initialized = false;
      SDL_GPUDevice* device = nullptr;
      SDL_GPUShaderFormat shader_formats = SDL_GPU_SHADERFORMAT_INVALID;

      Caps caps;
      Stats stats;
      uint32_t frame_count = 0;

      FrameBufferHandle view_framebuffer;
      uint16_t view_width = 0;
      uint16_t view_height = 0;

      DrawState draw;
      std::unordered_map<std::string, UniformValue> uniform_values;

      std::vector<DrawCommand> commands;
      std::vector<uint8_t> vertex_arena;
      std::vector<uint8_t> index_arena;
      std::vector<uint8_t> uniform_arena;

      SDL_GPUBuffer* transient_vertex_buffer = nullptr;
      uint32_t transient_vertex_capacity = 0;
      SDL_GPUBuffer* transient_index_buffer = nullptr;
      uint32_t transient_index_capacity = 0;

      std::vector<std::unique_ptr<uint8_t[]>> transient_memory;
      std::string last_shader_error;
    };

    BackendState g_state;
    SlotTable<ShaderResource> g_shaders;
    SlotTable<ProgramResource> g_programs;
    SlotTable<UniformResource> g_uniforms;
    SlotTable<TextureResource> g_textures;
    SlotTable<FrameBufferResource> g_framebuffers;
    SlotTable<BufferResource> g_index_buffers;
    SlotTable<BufferResource> g_vertex_buffers;

    SDL_GPUTextureFormat textureFormat(TextureFormat::Enum format) {
      switch (format) {
      case TextureFormat::RGBA8: return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
      case TextureFormat::BGRA8: return SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
      case TextureFormat::R16F: return SDL_GPU_TEXTUREFORMAT_R16_FLOAT;
      case TextureFormat::R32F: return SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
      case TextureFormat::RGBA16F: return SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
      case TextureFormat::RGB10A2: return SDL_GPU_TEXTUREFORMAT_R10G10B10A2_UNORM;
      default: VISAGE_ASSERT(false); return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
      }
    }

    int bytesPerPixel(TextureFormat::Enum format) {
      switch (format) {
      case TextureFormat::R16F: return 2;
      case TextureFormat::RGBA16F: return 8;
      default: return 4;
      }
    }

    SDL_GPUBlendFactor blendFactor(uint64_t factor) {
      switch (factor) {
      case BGFX_STATE_BLEND_ZERO: return SDL_GPU_BLENDFACTOR_ZERO;
      case BGFX_STATE_BLEND_ONE: return SDL_GPU_BLENDFACTOR_ONE;
      case BGFX_STATE_BLEND_SRC_ALPHA: return SDL_GPU_BLENDFACTOR_SRC_ALPHA;
      case BGFX_STATE_BLEND_INV_SRC_ALPHA: return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
      case BGFX_STATE_BLEND_DST_COLOR: return SDL_GPU_BLENDFACTOR_DST_COLOR;
      default: return SDL_GPU_BLENDFACTOR_ONE;
      }
    }

    // Only the bits that change pipeline state; everything else would split the
    // cache without changing the result.
    constexpr uint64_t kPipelineStateMask = 0xfull | (0x3fffffull << 8);

    SDL_GPUGraphicsPipeline* buildPipeline(ProgramResource& program, uint64_t state,
                                           TextureFormat::Enum target_format,
                                           const VertexLayout& layout) {
      ShaderResource& vertex = g_shaders.get(program.vertex_shader.idx);
      ShaderResource& fragment = g_shaders.get(program.fragment_shader.idx);

      SDL_GPUVertexBufferDescription buffer_description {};
      buffer_description.slot = 0;
      buffer_description.pitch = layout.getStride();
      buffer_description.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

      SDL_GPUVertexAttribute attributes[VertexLayout::kMaxAttributes] {};
      for (int i = 0; i < layout.numEntries(); ++i) {
        const VertexLayout::Entry& entry = layout.entry(i);
        attributes[i].location = static_cast<Uint32>(entry.attrib);
        attributes[i].buffer_slot = 0;
        attributes[i].offset = entry.offset;
        switch (entry.count) {
        case 1: attributes[i].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT; break;
        case 2: attributes[i].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2; break;
        case 3: attributes[i].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3; break;
        default: attributes[i].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4; break;
        }
      }

      SDL_GPUColorTargetBlendState blend {};
      blend.enable_blend = true;
      blend.src_color_blendfactor = blendFactor((state >> 8) & 0x7);
      blend.dst_color_blendfactor = blendFactor((state >> 11) & 0x7);
      blend.src_alpha_blendfactor = blendFactor((state >> 14) & 0x7);
      blend.dst_alpha_blendfactor = blendFactor((state >> 17) & 0x7);
      blend.color_blend_op = ((state >> 20) & 0x1) ? SDL_GPU_BLENDOP_REVERSE_SUBTRACT :
                                                     SDL_GPU_BLENDOP_ADD;
      blend.alpha_blend_op = ((state >> 21) & 0x1) ? SDL_GPU_BLENDOP_REVERSE_SUBTRACT :
                                                     SDL_GPU_BLENDOP_ADD;
      blend.enable_color_write_mask = true;
      blend.color_write_mask = 0;
      if (state & BGFX_STATE_WRITE_R)
        blend.color_write_mask |= SDL_GPU_COLORCOMPONENT_R;
      if (state & BGFX_STATE_WRITE_G)
        blend.color_write_mask |= SDL_GPU_COLORCOMPONENT_G;
      if (state & BGFX_STATE_WRITE_B)
        blend.color_write_mask |= SDL_GPU_COLORCOMPONENT_B;
      if (state & BGFX_STATE_WRITE_A)
        blend.color_write_mask |= SDL_GPU_COLORCOMPONENT_A;

      SDL_GPUColorTargetDescription color_target {};
      color_target.format = textureFormat(target_format);
      color_target.blend_state = blend;

      SDL_GPUGraphicsPipelineCreateInfo info {};
      info.vertex_shader = vertex.shader;
      info.fragment_shader = fragment.shader;
      info.vertex_input_state.vertex_buffer_descriptions = &buffer_description;
      info.vertex_input_state.num_vertex_buffers = 1;
      info.vertex_input_state.vertex_attributes = attributes;
      info.vertex_input_state.num_vertex_attributes = layout.numEntries();
      info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
      // bgfx treats clockwise as front-facing and fs_path_fill's coverage sign
      // depends on gl_FrontFacing matching that.
      info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
      info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
      info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_CLOCKWISE;
      info.target_info.color_target_descriptions = &color_target;
      info.target_info.num_color_targets = 1;

      SDL_GPUGraphicsPipeline* pipeline = SDL_CreateGPUGraphicsPipeline(g_state.device, &info);
      if (pipeline == nullptr)
        VISAGE_LOG(String("SDL_CreateGPUGraphicsPipeline failed: ") + SDL_GetError());
      return pipeline;
    }

    SDL_GPUGraphicsPipeline* pipelineFor(ProgramHandle program_handle, uint64_t state,
                                         TextureFormat::Enum target_format, const VertexLayout* layout) {
      ProgramResource& program = g_programs.get(program_handle.idx);
      PipelineKey key { state & kPipelineStateMask, static_cast<int>(target_format), layout };
      auto found = program.pipelines.find(key);
      if (found != program.pipelines.end())
        return found->second;

      SDL_GPUGraphicsPipeline* pipeline = buildPipeline(program, state, target_format, *layout);
      program.pipelines[key] = pipeline;
      return pipeline;
    }

    // Packs the stage's std140 block in the order the blob recorded, so the
    // shader's member layout and these bytes cannot drift apart.
    uint32_t packUniforms(const ShaderResource& shader, uint32_t& size_out) {
      uint32_t offset = static_cast<uint32_t>(g_state.uniform_arena.size());
      if (!shader.has_uniform_buffer || shader.uniform_names.empty()) {
        size_out = 0;
        return offset;
      }

      for (const std::string& name : shader.uniform_names) {
        float values[4] = {};
        auto found = g_state.uniform_values.find(name);
        if (found != g_state.uniform_values.end())
          std::memcpy(values, found->second.data, sizeof(values));
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(values);
        g_state.uniform_arena.insert(g_state.uniform_arena.end(), bytes, bytes + sizeof(values));
      }

      size_out = static_cast<uint32_t>(shader.uniform_names.size()) * 16u;
      return offset;
    }

    uint32_t appendArena(std::vector<uint8_t>& arena, const uint8_t* data, uint32_t size,
                         uint32_t alignment) {
      size_t padded = (arena.size() + alignment - 1) & ~static_cast<size_t>(alignment - 1);
      arena.resize(padded);
      uint32_t offset = static_cast<uint32_t>(arena.size());
      arena.insert(arena.end(), data, data + size);
      return offset;
    }

    void ensureBuffer(SDL_GPUBuffer*& buffer, uint32_t& capacity, uint32_t needed,
                      SDL_GPUBufferUsageFlags usage) {
      if (needed <= capacity)
        return;
      if (buffer)
        SDL_ReleaseGPUBuffer(g_state.device, buffer);

      capacity = needed + needed / 2;
      SDL_GPUBufferCreateInfo info {};
      info.usage = usage;
      info.size = capacity;
      buffer = SDL_CreateGPUBuffer(g_state.device, &info);
    }

    void executeCommands();

    void uploadArena(SDL_GPUCopyPass* copy_pass, const std::vector<uint8_t>& arena,
                     SDL_GPUBuffer* buffer) {
      if (arena.empty())
        return;

      SDL_GPUTransferBufferCreateInfo transfer_info {};
      transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
      transfer_info.size = static_cast<Uint32>(arena.size());
      SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(g_state.device, &transfer_info);
      if (transfer == nullptr)
        return;

      void* mapped = SDL_MapGPUTransferBuffer(g_state.device, transfer, false);
      std::memcpy(mapped, arena.data(), arena.size());
      SDL_UnmapGPUTransferBuffer(g_state.device, transfer);

      SDL_GPUTransferBufferLocation source {};
      source.transfer_buffer = transfer;
      SDL_GPUBufferRegion destination {};
      destination.buffer = buffer;
      destination.size = static_cast<Uint32>(arena.size());
      SDL_UploadToGPUBuffer(copy_pass, &source, &destination, true);
      SDL_ReleaseGPUTransferBuffer(g_state.device, transfer);
    }
  }

  bool isValid(ShaderHandle handle) { return handle.idx != kInvalidHandle; }
  bool isValid(ProgramHandle handle) { return handle.idx != kInvalidHandle; }
  bool isValid(UniformHandle handle) { return handle.idx != kInvalidHandle; }
  bool isValid(TextureHandle handle) { return handle.idx != kInvalidHandle; }
  bool isValid(FrameBufferHandle handle) { return handle.idx != kInvalidHandle; }
  bool isValid(IndexBufferHandle handle) { return handle.idx != kInvalidHandle; }
  bool isValid(VertexBufferHandle handle) { return handle.idx != kInvalidHandle; }

  void destroy(ShaderHandle handle) {
    if (!isValid(handle))
      return;
    SDL_ReleaseGPUShader(g_state.device, g_shaders.get(handle.idx).shader);
    g_shaders.release(handle.idx);
  }

  void destroy(ProgramHandle handle) {
    if (!isValid(handle))
      return;
    ProgramResource& program = g_programs.get(handle.idx);
    for (auto& pipeline : program.pipelines)
      SDL_ReleaseGPUGraphicsPipeline(g_state.device, pipeline.second);
    program.pipelines.clear();
    g_programs.release(handle.idx);
  }

  void destroy(UniformHandle handle) {
    if (isValid(handle))
      g_uniforms.release(handle.idx);
  }

  void destroy(TextureHandle handle) {
    if (!isValid(handle))
      return;
    TextureResource& resource = g_textures.get(handle.idx);
    if (resource.sampler)
      SDL_ReleaseGPUSampler(g_state.device, resource.sampler);
    if (resource.texture)
      SDL_ReleaseGPUTexture(g_state.device, resource.texture);
    g_textures.release(handle.idx);
  }

  void destroy(FrameBufferHandle handle) {
    if (!isValid(handle))
      return;
    destroy(g_framebuffers.get(handle.idx).color_texture);
    g_framebuffers.release(handle.idx);
  }

  void destroy(IndexBufferHandle handle) {
    if (!isValid(handle))
      return;
    SDL_ReleaseGPUBuffer(g_state.device, g_index_buffers.get(handle.idx).buffer);
    g_index_buffers.release(handle.idx);
  }

  void destroy(VertexBufferHandle handle) {
    if (!isValid(handle))
      return;
    SDL_ReleaseGPUBuffer(g_state.device, g_vertex_buffers.get(handle.idx).buffer);
    g_vertex_buffers.release(handle.idx);
  }

  const Memory* copy(const void* data, uint32_t size) {
    auto memory = new Memory();
    auto owned = new uint8_t[size];
    std::memcpy(owned, data, size);
    memory->data = owned;
    memory->size = size;
    return memory;
  }

  const Memory* makeRef(const void* data, uint32_t size) {
    auto memory = new Memory();
    memory->data = static_cast<const uint8_t*>(data);
    memory->size = size;
    return memory;
  }

  namespace {
    void releaseMemory(const Memory* memory, bool owns_data) {
      if (owns_data)
        delete[] memory->data;
      delete memory;
    }
  }

  ShaderHandle createShader(const Memory* memory) {
    ParsedBlob blob = parseBlob(memory->data, memory->size, g_state.shader_formats);
    if (!blob.valid) {
      g_state.last_shader_error = "Shader blob is not a shadertool v2 container for this backend";
      VISAGE_LOG(g_state.last_shader_error.c_str());
      releaseMemory(memory, true);
      return {};
    }

    SDL_GPUShaderCreateInfo info {};
    info.code = blob.code;
    info.code_size = blob.code_size;
    info.entrypoint = blob.entrypoint();
    info.format = blob.format;
    info.stage = blob.isFragment() ? SDL_GPU_SHADERSTAGE_FRAGMENT : SDL_GPU_SHADERSTAGE_VERTEX;
    info.num_samplers = blob.num_samplers;
    info.num_uniform_buffers = blob.num_uniform_buffers;

    SDL_GPUShader* shader = SDL_CreateGPUShader(g_state.device, &info);
    releaseMemory(memory, true);
    if (shader == nullptr) {
      g_state.last_shader_error = SDL_GetError();
      VISAGE_LOG(String("SDL_CreateGPUShader failed: ") + g_state.last_shader_error.c_str());
      return {};
    }

    ShaderHandle handle;
    handle.idx = g_shaders.create({ shader, blob.isFragment(), std::move(blob.uniform_names),
                                    std::move(blob.sampler_names), blob.num_uniform_buffers > 0 });
    return handle;
  }

  void* createGpuShader(const Memory* memory) {
    ShaderHandle handle = createShader(memory);
    if (!isValid(handle))
      return nullptr;

    // The application owns the shader from here; the slot only held it so
    // createShader could stay the one place a blob turns into a stage.
    SDL_GPUShader* shader = g_shaders.get(handle.idx).shader;
    g_shaders.release(handle.idx);
    return shader;
  }

  void destroyGpuShader(void* shader) {
    if (shader)
      SDL_ReleaseGPUShader(g_state.device, static_cast<SDL_GPUShader*>(shader));
  }

  void* createComputePipeline(const Memory* memory) {
    // The parse only points into `memory`, so it has to outlive the create.
    ParsedBlob blob = parseBlob(memory->data, memory->size, g_state.shader_formats);
    if (!blob.valid || blob.stage != BlobStage::Compute) {
      g_state.last_shader_error = "Shader blob is not a compute kernel for this backend";
      VISAGE_LOG(g_state.last_shader_error.c_str());
      releaseMemory(memory, true);
      return nullptr;
    }

    SDL_GPUComputePipelineCreateInfo info {};
    info.code = blob.code;
    info.code_size = blob.code_size;
    info.entrypoint = blob.entrypoint();
    info.format = blob.format;
    info.num_samplers = blob.num_samplers;
    info.num_readonly_storage_textures = blob.num_readonly_storage_textures;
    info.num_readonly_storage_buffers = blob.num_readonly_storage_buffers;
    info.num_readwrite_storage_textures = blob.num_readwrite_storage_textures;
    info.num_readwrite_storage_buffers = blob.num_readwrite_storage_buffers;
    info.num_uniform_buffers = blob.num_uniform_buffers;
    info.threadcount_x = blob.threadcount[0];
    info.threadcount_y = blob.threadcount[1];
    info.threadcount_z = blob.threadcount[2];

    SDL_GPUComputePipeline* pipeline = SDL_CreateGPUComputePipeline(g_state.device, &info);
    releaseMemory(memory, true);
    if (pipeline == nullptr) {
      g_state.last_shader_error = SDL_GetError();
      VISAGE_LOG(String("SDL_CreateGPUComputePipeline failed: ") + g_state.last_shader_error.c_str());
    }
    return pipeline;
  }

  void destroyComputePipeline(void* pipeline) {
    if (pipeline)
      SDL_ReleaseGPUComputePipeline(g_state.device, static_cast<SDL_GPUComputePipeline*>(pipeline));
  }

  ProgramHandle createProgram(ShaderHandle vertex_shader, ShaderHandle fragment_shader,
                              bool destroy_shaders) {
    if (!isValid(vertex_shader) || !isValid(fragment_shader))
      return {};

    ProgramHandle handle;
    handle.idx = g_programs.create({ vertex_shader, fragment_shader, {} });
    // Shaders stay alive: pipelines are built lazily per target format and
    // blend state, long after the caller would have dropped them.
    (void)destroy_shaders;
    return handle;
  }

  UniformHandle createUniform(const char* name, UniformType::Enum type, uint16_t) {
    UniformHandle handle;
    handle.idx = g_uniforms.create({ name, type });
    return handle;
  }

  TextureHandle createTexture2D(uint16_t width, uint16_t height, bool, uint16_t,
                                TextureFormat::Enum format, uint64_t flags) {
    SDL_GPUTextureCreateInfo info {};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = textureFormat(format);
    info.width = width;
    info.height = height;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    // A blit destination has to be a colour target too, not just readable.
    if (flags & (BGFX_TEXTURE_RT | BGFX_TEXTURE_BLIT_DST))
      info.usage |= SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;

    SDL_GPUTexture* texture = SDL_CreateGPUTexture(g_state.device, &info);
    if (texture == nullptr) {
      VISAGE_LOG(String("SDL_CreateGPUTexture failed: ") + SDL_GetError());
      return {};
    }

    SDL_GPUSamplerCreateInfo sampler_info {};
    // 32-bit float textures are not filterable everywhere; the path atlas reads
    // them by exact texel anyway.
    SDL_GPUFilter filter = format == TextureFormat::R32F ? SDL_GPU_FILTER_NEAREST :
                                                           SDL_GPU_FILTER_LINEAR;
    sampler_info.min_filter = filter;
    sampler_info.mag_filter = filter;
    sampler_info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sampler_info.address_mode_u = (flags & BGFX_SAMPLER_U_CLAMP) ?
                                      SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE :
                                      SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    sampler_info.address_mode_v = (flags & BGFX_SAMPLER_V_CLAMP) ?
                                      SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE :
                                      SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    sampler_info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;

    TextureHandle handle;
    handle.idx = g_textures.create({ texture, SDL_CreateGPUSampler(g_state.device, &sampler_info),
                                     format, width, height });
    return handle;
  }

  void updateTexture2D(TextureHandle handle, uint16_t, uint8_t, uint16_t x, uint16_t y,
                       uint16_t width, uint16_t height, const Memory* memory, uint16_t) {
    TextureResource& resource = g_textures.get(handle.idx);
    uint32_t size = static_cast<uint32_t>(width) * height * bytesPerPixel(resource.format);

    SDL_GPUTransferBufferCreateInfo transfer_info {};
    transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transfer_info.size = size;
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(g_state.device, &transfer_info);
    if (transfer) {
      void* mapped = SDL_MapGPUTransferBuffer(g_state.device, transfer, false);
      std::memcpy(mapped, memory->data, std::min<uint32_t>(size, memory->size));
      SDL_UnmapGPUTransferBuffer(g_state.device, transfer);

      SDL_GPUCommandBuffer* command_buffer = SDL_AcquireGPUCommandBuffer(g_state.device);
      SDL_GPUCopyPass* copy_pass = SDL_BeginGPUCopyPass(command_buffer);

      SDL_GPUTextureTransferInfo source {};
      source.transfer_buffer = transfer;
      source.pixels_per_row = width;
      source.rows_per_layer = height;
      SDL_GPUTextureRegion destination {};
      destination.texture = resource.texture;
      destination.x = x;
      destination.y = y;
      destination.w = width;
      destination.h = height;
      destination.d = 1;
      SDL_UploadToGPUTexture(copy_pass, &source, &destination, false);

      SDL_EndGPUCopyPass(copy_pass);
      SDL_SubmitGPUCommandBuffer(command_buffer);
      SDL_ReleaseGPUTransferBuffer(g_state.device, transfer);
    }
    releaseMemory(memory, true);
  }

  namespace {
    BufferResource createBuffer(const Memory* memory, SDL_GPUBufferUsageFlags usage) {
      SDL_GPUBufferCreateInfo info {};
      info.usage = usage;
      info.size = memory->size;
      SDL_GPUBuffer* buffer = SDL_CreateGPUBuffer(g_state.device, &info);

      SDL_GPUTransferBufferCreateInfo transfer_info {};
      transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
      transfer_info.size = memory->size;
      SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(g_state.device, &transfer_info);
      void* mapped = SDL_MapGPUTransferBuffer(g_state.device, transfer, false);
      std::memcpy(mapped, memory->data, memory->size);
      SDL_UnmapGPUTransferBuffer(g_state.device, transfer);

      SDL_GPUCommandBuffer* command_buffer = SDL_AcquireGPUCommandBuffer(g_state.device);
      SDL_GPUCopyPass* copy_pass = SDL_BeginGPUCopyPass(command_buffer);
      SDL_GPUTransferBufferLocation source {};
      source.transfer_buffer = transfer;
      SDL_GPUBufferRegion destination {};
      destination.buffer = buffer;
      destination.size = memory->size;
      SDL_UploadToGPUBuffer(copy_pass, &source, &destination, false);
      SDL_EndGPUCopyPass(copy_pass);
      SDL_SubmitGPUCommandBuffer(command_buffer);
      SDL_ReleaseGPUTransferBuffer(g_state.device, transfer);

      return { buffer, memory->size, 0, false, {} };
    }
  }

  IndexBufferHandle createIndexBuffer(const Memory* memory, uint16_t) {
    BufferResource resource = createBuffer(memory, SDL_GPU_BUFFERUSAGE_INDEX);
    resource.count = memory->size / 2;
    IndexBufferHandle handle;
    handle.idx = g_index_buffers.create(resource);
    releaseMemory(memory, false);
    return handle;
  }

  VertexBufferHandle createVertexBuffer(const Memory* memory, const VertexLayout& layout, uint16_t) {
    BufferResource resource = createBuffer(memory, SDL_GPU_BUFFERUSAGE_VERTEX);
    resource.layout = layout;
    VertexBufferHandle handle;
    handle.idx = g_vertex_buffers.create(resource);
    releaseMemory(memory, false);
    return handle;
  }

  bool allocTransientBuffers(TransientVertexBuffer* vertex_buffer, const VertexLayout& layout,
                             uint32_t num_vertices, TransientIndexBuffer* index_buffer,
                             uint32_t num_indices, bool index32) {
    allocTransientVertexBuffer(vertex_buffer, num_vertices, layout);

    index_buffer->size = num_indices * (index32 ? 4 : 2);
    index_buffer->isIndex16 = !index32;
    auto memory = std::make_unique<uint8_t[]>(index_buffer->size);
    index_buffer->data = memory.get();
    g_state.transient_memory.push_back(std::move(memory));
    return true;
  }

  bool allocTransientVertexBuffer(TransientVertexBuffer* vertex_buffer, uint32_t num_vertices,
                                  const VertexLayout& layout) {
    vertex_buffer->size = num_vertices * layout.getStride();
    vertex_buffer->layout = &layout;
    auto memory = std::make_unique<uint8_t[]>(vertex_buffer->size);
    vertex_buffer->data = memory.get();
    g_state.transient_memory.push_back(std::move(memory));
    return true;
  }

  FrameBufferHandle createFrameBuffer(void* native_window_handle, uint16_t width, uint16_t height,
                                      TextureFormat::Enum format) {
    VISAGE_ASSERT(native_window_handle);
    FrameBufferHandle handle = createFrameBuffer(width, height, format,
                                                 BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
    if (isValid(handle))
      g_framebuffers.get(handle.idx).window = static_cast<SDL_Window*>(native_window_handle);
    return handle;
  }

  FrameBufferHandle createFrameBuffer(uint16_t width, uint16_t height, TextureFormat::Enum format,
                                      uint64_t flags) {
    TextureHandle color_texture = createTexture2D(width, height, false, 1, format,
                                                  flags | BGFX_TEXTURE_RT);
    if (!isValid(color_texture))
      return {};

    FrameBufferHandle handle;
    handle.idx = g_framebuffers.create({ color_texture, format, width, height, nullptr, false });
    return handle;
  }

  TextureHandle getTexture(FrameBufferHandle handle, uint8_t) {
    if (!isValid(handle))
      return {};
    return g_framebuffers.get(handle.idx).color_texture;
  }

  void setViewMode(uint16_t, ViewMode::Enum) { }

  void setViewRect(uint16_t, uint16_t, uint16_t, uint16_t width, uint16_t height) {
    g_state.view_width = width;
    g_state.view_height = height;
  }

  void setViewFrameBuffer(uint16_t, FrameBufferHandle handle) {
    g_state.view_framebuffer = handle;
  }

  void setState(uint64_t state) {
    g_state.draw.state = state;
  }

  void setUniform(UniformHandle handle, const void* value, uint16_t num) {
    const UniformResource& resource = g_uniforms.get(handle.idx);
    UniformValue& stored = g_state.uniform_values[resource.name];
    stored.type = resource.type;

    size_t floats = 4;
    if (resource.type == UniformType::Mat3)
      floats = 9;
    else if (resource.type == UniformType::Mat4)
      floats = 16;
    if (num > 1)
      floats = std::min<size_t>(16, floats * num);
    std::memcpy(stored.data, value, floats * sizeof(float));
  }

  void setTexture(uint8_t stage, UniformHandle sampler, TextureHandle handle) {
    VISAGE_ASSERT(stage < kMaxTextureStages);
    PendingTexture& pending = g_state.draw.textures[stage];
    pending.bound = true;
    pending.sampler_name = g_uniforms.get(sampler.idx).name;
    pending.texture = handle;
  }

  void setVertexBuffer(uint8_t, const TransientVertexBuffer* vertex_buffer) {
    DrawState& draw = g_state.draw;
    draw.has_vertex_buffer = true;
    draw.vertex_buffer = {};
    draw.vertex_layout = vertex_buffer->layout;
    draw.pending_vertex_data = vertex_buffer->data;
    draw.pending_vertex_size = vertex_buffer->size;
  }

  void setVertexBuffer(uint8_t, VertexBufferHandle handle) {
    DrawState& draw = g_state.draw;
    draw.has_vertex_buffer = true;
    draw.vertex_buffer = handle;
    draw.vertex_layout = &g_vertex_buffers.get(handle.idx).layout;
    draw.pending_vertex_data = nullptr;
  }

  void setIndexBuffer(const TransientIndexBuffer* index_buffer) {
    DrawState& draw = g_state.draw;
    draw.has_index_buffer = true;
    draw.index_buffer = {};
    draw.index32 = !index_buffer->isIndex16;
    draw.index_count = index_buffer->size / (draw.index32 ? 4 : 2);
    draw.pending_index_data = index_buffer->data;
    draw.pending_index_size = index_buffer->size;
  }

  void setIndexBuffer(IndexBufferHandle handle) {
    BufferResource& resource = g_index_buffers.get(handle.idx);
    DrawState& draw = g_state.draw;
    draw.has_index_buffer = true;
    draw.index_buffer = handle;
    draw.index32 = resource.index32;
    draw.index_count = resource.count;
    draw.pending_index_data = nullptr;
  }

  void submit(uint16_t, ProgramHandle program) {
    DrawState& draw = g_state.draw;
    if (!isValid(program) || !draw.has_vertex_buffer || !draw.has_index_buffer ||
        draw.vertex_layout == nullptr || !isValid(g_state.view_framebuffer)) {
      draw.reset();
      return;
    }

    ++g_state.stats.numDraw;

    DrawCommand command;
    command.program = program;
    command.state = draw.state;
    command.target = g_state.view_framebuffer;
    command.view_width = g_state.view_width;
    command.view_height = g_state.view_height;
    command.index32 = draw.index32;
    command.index_count = draw.index_count;

    ProgramResource& program_resource = g_programs.get(program.idx);
    ShaderResource& vertex = g_shaders.get(program_resource.vertex_shader.idx);
    ShaderResource& fragment = g_shaders.get(program_resource.fragment_shader.idx);

    if (draw.pending_vertex_data) {
      command.vertex_offset = appendArena(g_state.vertex_arena, draw.pending_vertex_data,
                                          draw.pending_vertex_size, draw.vertex_layout->getStride());
      command.layout = draw.vertex_layout;
    }
    else {
      command.vertex_buffer = draw.vertex_buffer;
      command.layout = draw.vertex_layout;
    }

    if (draw.pending_index_data) {
      command.index_offset = appendArena(g_state.index_arena, draw.pending_index_data,
                                         draw.pending_index_size, draw.index32 ? 4 : 2);
    }
    else
      command.index_buffer = draw.index_buffer;

    command.vertex_uniform_offset = packUniforms(vertex, command.vertex_uniform_size);
    command.fragment_uniform_offset = packUniforms(fragment, command.fragment_uniform_size);

    // Slot comes from the shader's own declaration order, not the stage the
    // caller passed: slot 0 is the gradient atlas for a library shader quad but
    // the effect's own texture for a third-party one.
    for (const std::string& sampler_name : fragment.sampler_names) {
      SDL_GPUTextureSamplerBinding binding {};
      for (const PendingTexture& pending : draw.textures) {
        if (!pending.bound || pending.sampler_name != sampler_name || !isValid(pending.texture))
          continue;
        TextureResource& texture = g_textures.get(pending.texture.idx);
        binding.texture = texture.texture;
        binding.sampler = texture.sampler;
        break;
      }
      command.samplers[command.num_samplers++] = binding;
    }

    g_state.commands.push_back(command);
    draw.reset();
  }

  void blit(uint16_t, TextureHandle dst, uint16_t dst_x, uint16_t dst_y, TextureHandle src,
            uint16_t src_x, uint16_t src_y, uint16_t width, uint16_t height) {
    // The source is usually a render target drawn to earlier this frame, so the
    // recorded commands have to land before the copy reads it.
    executeCommands();

    TextureResource& destination = g_textures.get(dst.idx);
    TextureResource& source = g_textures.get(src.idx);
    if (width == UINT16_MAX)
      width = source.width;
    if (height == UINT16_MAX)
      height = source.height;

    SDL_GPUBlitInfo info {};
    info.source.texture = source.texture;
    info.source.x = src_x;
    info.source.y = src_y;
    info.source.w = width;
    info.source.h = height;
    info.destination.texture = destination.texture;
    info.destination.x = dst_x;
    info.destination.y = dst_y;
    info.destination.w = width;
    info.destination.h = height;
    info.load_op = SDL_GPU_LOADOP_DONT_CARE;
    info.filter = SDL_GPU_FILTER_NEAREST;

    SDL_GPUCommandBuffer* command_buffer = SDL_AcquireGPUCommandBuffer(g_state.device);
    SDL_BlitGPUTexture(command_buffer, &info);
    SDL_SubmitGPUCommandBuffer(command_buffer);
  }

  void readTexture(TextureHandle handle, void* data, uint8_t) {
    // Draws are only recorded until something forces them out; reading a
    // texture before that would sample whatever it held last frame.
    executeCommands();

    TextureResource& resource = g_textures.get(handle.idx);
    uint32_t size = static_cast<uint32_t>(resource.width) * resource.height *
                    bytesPerPixel(resource.format);

    SDL_GPUTransferBufferCreateInfo transfer_info {};
    transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    transfer_info.size = size;
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(g_state.device, &transfer_info);
    if (transfer == nullptr)
      return;

    SDL_GPUCommandBuffer* command_buffer = SDL_AcquireGPUCommandBuffer(g_state.device);
    SDL_GPUCopyPass* copy_pass = SDL_BeginGPUCopyPass(command_buffer);
    SDL_GPUTextureRegion source {};
    source.texture = resource.texture;
    source.w = resource.width;
    source.h = resource.height;
    source.d = 1;
    SDL_GPUTextureTransferInfo destination {};
    destination.transfer_buffer = transfer;
    destination.pixels_per_row = resource.width;
    destination.rows_per_layer = resource.height;
    SDL_DownloadFromGPUTexture(copy_pass, &source, &destination);
    SDL_EndGPUCopyPass(copy_pass);

    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command_buffer);
    SDL_WaitForGPUFences(g_state.device, true, &fence, 1);
    SDL_ReleaseGPUFence(g_state.device, fence);

    void* mapped = SDL_MapGPUTransferBuffer(g_state.device, transfer, false);
    std::memcpy(data, mapped, size);
    SDL_UnmapGPUTransferBuffer(g_state.device, transfer);
    SDL_ReleaseGPUTransferBuffer(g_state.device, transfer);
  }

  void requestScreenShot(FrameBufferHandle handle, const char*) {
    const FrameBufferResource& resource = g_framebuffers.get(handle.idx);
    std::vector<uint8_t> pixels(static_cast<size_t>(resource.width) * resource.height * 4);
    readTexture(resource.color_texture, pixels.data());
    Renderer::instance().setScreenshotData(pixels.data(), resource.width, resource.height,
                                           resource.width * 4, false);
  }

  namespace {
    // Replays everything recorded so far. Called at end of frame, and again
    // whenever a readback needs the pixels to actually exist.
    void executeCommands() {
      if (g_state.commands.empty())
        return;

      ensureBuffer(g_state.transient_vertex_buffer, g_state.transient_vertex_capacity,
                 static_cast<uint32_t>(g_state.vertex_arena.size()), SDL_GPU_BUFFERUSAGE_VERTEX);
      ensureBuffer(g_state.transient_index_buffer, g_state.transient_index_capacity,
                 static_cast<uint32_t>(g_state.index_arena.size()), SDL_GPU_BUFFERUSAGE_INDEX);

      SDL_GPUCommandBuffer* command_buffer = SDL_AcquireGPUCommandBuffer(g_state.device);

      SDL_GPUCopyPass* copy_pass = SDL_BeginGPUCopyPass(command_buffer);
      uploadArena(copy_pass, g_state.vertex_arena, g_state.transient_vertex_buffer);
      uploadArena(copy_pass, g_state.index_arena, g_state.transient_index_buffer);
      SDL_EndGPUCopyPass(copy_pass);

      SDL_GPURenderPass* render_pass = nullptr;
      uint16_t current_target = kInvalidHandle;

      for (const DrawCommand& command : g_state.commands) {
      FrameBufferResource& target = g_framebuffers.get(command.target.idx);
      if (command.target.idx != current_target) {
        if (render_pass)
          SDL_EndGPURenderPass(render_pass);

        SDL_GPUColorTargetInfo target_info {};
        target_info.texture = g_textures.get(target.color_texture.idx).texture;
        // LOAD, never CLEAR: window framebuffers are persistent and dirty-region
        // redraw depends on untouched pixels surviving the frame.
        target_info.load_op = target.initialized ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_CLEAR;
        target_info.store_op = SDL_GPU_STOREOP_STORE;
        target.initialized = true;
        render_pass = SDL_BeginGPURenderPass(command_buffer, &target_info, 1, nullptr);
        current_target = command.target.idx;
      }

      SDL_GPUGraphicsPipeline* pipeline = pipelineFor(command.program, command.state, target.format,
                                                      command.layout);
      if (pipeline == nullptr)
        continue;

      SDL_GPUViewport viewport {};
      viewport.w = command.view_width;
      viewport.h = command.view_height;
      viewport.max_depth = 1.0f;
      SDL_SetGPUViewport(render_pass, &viewport);
      SDL_BindGPUGraphicsPipeline(render_pass, pipeline);

      SDL_GPUBufferBinding vertex_binding {};
      vertex_binding.buffer = isValid(command.vertex_buffer) ?
                                  g_vertex_buffers.get(command.vertex_buffer.idx).buffer :
                                  g_state.transient_vertex_buffer;
      vertex_binding.offset = isValid(command.vertex_buffer) ? 0 : command.vertex_offset;
      SDL_BindGPUVertexBuffers(render_pass, 0, &vertex_binding, 1);

      SDL_GPUBufferBinding index_binding {};
      index_binding.buffer = isValid(command.index_buffer) ?
                                 g_index_buffers.get(command.index_buffer.idx).buffer :
                                 g_state.transient_index_buffer;
      index_binding.offset = isValid(command.index_buffer) ? 0 : command.index_offset;
      SDL_BindGPUIndexBuffer(render_pass, &index_binding,
                             command.index32 ? SDL_GPU_INDEXELEMENTSIZE_32BIT :
                                               SDL_GPU_INDEXELEMENTSIZE_16BIT);

      if (command.num_samplers)
        SDL_BindGPUFragmentSamplers(render_pass, 0, command.samplers, command.num_samplers);

      if (command.vertex_uniform_size) {
        SDL_PushGPUVertexUniformData(command_buffer, 0,
                                     g_state.uniform_arena.data() + command.vertex_uniform_offset,
                                     command.vertex_uniform_size);
      }
      if (command.fragment_uniform_size) {
        SDL_PushGPUFragmentUniformData(command_buffer, 0,
                                       g_state.uniform_arena.data() + command.fragment_uniform_offset,
                                       command.fragment_uniform_size);
      }

      SDL_DrawGPUIndexedPrimitives(render_pass, command.index_count, 1, 0, 0, 0);
      }

      if (render_pass)
      SDL_EndGPURenderPass(render_pass);
      SDL_SubmitGPUCommandBuffer(command_buffer);

      g_state.commands.clear();
      g_state.vertex_arena.clear();
      g_state.index_arena.clear();
      g_state.uniform_arena.clear();
    }
  }

  uint32_t frame(bool) {
    executeCommands();
    g_state.transient_memory.clear();
    return g_state.frame_count++;
  }

  // Swapchains are claimed with vsync and recreated by SDL when the window
  // resizes, so there is no reset flag left to act on.
  void reset(uint32_t, uint32_t, uint32_t) { }

  const Caps* getCaps() {
    return &g_state.caps;
  }

  const Stats* getStats() {
    return &g_state.stats;
  }

  const char* getRendererName(RendererType::Enum) {
    const char* driver = g_state.device ? SDL_GetGPUDeviceDriver(g_state.device) : nullptr;
    return driver ? driver : "SDL_GPU";
  }

  RendererType::Enum getRendererType() {
    return g_state.caps.rendererType;
  }

  bool initBackend() {
    if (g_state.initialized)
      return true;

    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
      VISAGE_LOG(String("SDL_InitSubSystem(VIDEO) failed: ") + SDL_GetError());
      return false;
    }

    // Validation layers in debug builds: SDL_GPU misuse is otherwise a silent
    // crash inside the driver.
    bool debug = false;
    VISAGE_ASSERT((debug = true));

    // Properties rather than SDL_CreateGPUDevice(): the Pi's V3D does not
    // support depthClamp, which SDL_GPU otherwise treats as required, so it
    // passes over V3D and picks llvmpipe instead - software rasterization that
    // also cannot present to a KMSDRM display. Nothing here clamps depth, so
    // opting out of the feature costs nothing, and requiring hardware
    // acceleration turns a silent software fallback into an honest failure.
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_SPIRV_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_MSL_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_DEBUGMODE_BOOLEAN, debug);
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_FEATURE_DEPTH_CLAMPING_BOOLEAN, false);
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_VULKAN_REQUIRE_HARDWARE_ACCELERATION_BOOLEAN,
                           true);
    g_state.device = SDL_CreateGPUDeviceWithProperties(props);
    SDL_DestroyProperties(props);
    if (g_state.device == nullptr) {
      VISAGE_LOG(String("SDL_CreateGPUDevice failed: ") + SDL_GetError());
      return false;
    }

    g_state.shader_formats = SDL_GetGPUShaderFormats(g_state.device);
    g_state.caps.rendererType = RendererType::OpenGL;
    // SDL_GPU renders top-left origin on every backend, which is the same
    // convention bgfx used for D3D - visage already handles it.
    g_state.caps.originBottomLeft = false;
    g_state.caps.supported = BGFX_CAPS_INDEX32 | BGFX_CAPS_TEXTURE_BLIT |
                             BGFX_CAPS_TEXTURE_READ_BACK | BGFX_CAPS_SWAP_CHAIN;

    g_state.initialized = true;
    return true;
  }

  void* gpuDevice() {
    return g_state.device;
  }

  namespace {
    // Present pass: one quad from the window framebuffer's texture into the
    // swapchain. vs_full_screen_texture takes (x, y, u, v) packed in one vec4
    // and fs_sample does a plain texture read, so no present-only shader
    // exists; the pipeline just needs the swapchain's own format.
    struct PresentResources {
      ProgramHandle program;
      VertexLayout layout;
      SDL_GPUBuffer* quad = nullptr;
      std::map<std::pair<int, bool>, SDL_GPUGraphicsPipeline*> pipelines;
    };

    PresentResources g_present;

    // A swapchain texture is acquired against one command buffer and presented
    // when that buffer is submitted, so an application drawing underneath the
    // UI and the composite pass have to share both.
    struct PendingWindowTarget {
      SDL_Window* window = nullptr;
      SDL_GPUCommandBuffer* command_buffer = nullptr;
      SDL_GPUTexture* texture = nullptr;
      uint32_t width = 0;
      uint32_t height = 0;
    };

    PendingWindowTarget g_pending_target;

    // Acquiring blocks until the swapchain is ready, which is what paces the
    // frame loop now that there is no buffer swap to wait on.
    void acquireSwapchain(SDL_Window* window) {
      // Nothing drew into an earlier window's texture, but its command buffer
      // still has to go somewhere.
      if (g_pending_target.command_buffer)
        SDL_SubmitGPUCommandBuffer(g_pending_target.command_buffer);

      g_pending_target = {};
      g_pending_target.window = window;
      g_pending_target.command_buffer = SDL_AcquireGPUCommandBuffer(g_state.device);
      SDL_WaitAndAcquireGPUSwapchainTexture(g_pending_target.command_buffer, window,
                                            &g_pending_target.texture, &g_pending_target.width,
                                            &g_pending_target.height);
    }

    // Screen corners of a triangle strip: BL, BR, TL, TR. Texture corners are
    // a ring in the same rotational order, so a quarter turn is a ring shift.
    // V runs opposite the gl backend's table because SDL_GPU textures are
    // top-down - that makes ring i the same image corner on both, and the
    // permutation and its direction carry over unchanged.
    constexpr float kCornerX[] = { -1.0f, 1.0f, -1.0f, 1.0f };
    constexpr float kCornerY[] = { -1.0f, -1.0f, 1.0f, 1.0f };
    constexpr float kRingU[] = { 0.0f, 1.0f, 1.0f, 0.0f };
    constexpr float kRingV[] = { 1.0f, 1.0f, 0.0f, 0.0f };
    constexpr int kCornerToRing[] = { 0, 1, 3, 2 };
    constexpr int kCornersPerQuad = 4;

    bool initPresentResources() {
      if (isValid(g_present.program))
        return true;

      ShaderHandle vertex = createShader(copy(shaders::vs_full_screen_texture.data,
                                              shaders::vs_full_screen_texture.size));
      ShaderHandle fragment = createShader(copy(shaders::fs_sample.data, shaders::fs_sample.size));
      g_present.program = createProgram(vertex, fragment);
      if (!isValid(g_present.program))
        return false;

      g_present.layout.begin().add(Attrib::Position, 4, AttribType::Float).end();

      // All four rotations live in the buffer at once, so presenting is a draw
      // with no upload.
      float vertices[4 * kCornersPerQuad * 4];
      float* value = vertices;
      for (int rotation = 0; rotation < 4; ++rotation) {
        for (int corner = 0; corner < kCornersPerQuad; ++corner) {
          int ring = (kCornerToRing[corner] + rotation) & 3;
          *value++ = kCornerX[corner];
          *value++ = kCornerY[corner];
          *value++ = kRingU[ring];
          *value++ = kRingV[ring];
        }
      }

      const Memory* memory = makeRef(vertices, sizeof(vertices));
      g_present.quad = createBuffer(memory, SDL_GPU_BUFFERUSAGE_VERTEX).buffer;
      releaseMemory(memory, false);
      return g_present.quad != nullptr;
    }

    SDL_GPUGraphicsPipeline* presentPipeline(SDL_GPUTextureFormat format, bool blend) {
      std::pair<int, bool> key { static_cast<int>(format), blend };
      auto found = g_present.pipelines.find(key);
      if (found != g_present.pipelines.end())
        return found->second;

      ProgramResource& program = g_programs.get(g_present.program.idx);

      SDL_GPUVertexBufferDescription buffer_description {};
      buffer_description.pitch = g_present.layout.getStride();
      buffer_description.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

      SDL_GPUVertexAttribute attribute {};
      attribute.location = static_cast<Uint32>(Attrib::Position);
      attribute.format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;

      // Layer contents are premultiplied by BlendMode::Alpha, so compositing
      // over what the application drew is ONE / INV_SRC_ALPHA.
      SDL_GPUBlendFactor destination = blend ? SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA :
                                               SDL_GPU_BLENDFACTOR_ZERO;
      SDL_GPUColorTargetBlendState blend_state {};
      blend_state.enable_blend = true;
      blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
      blend_state.dst_color_blendfactor = destination;
      blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
      blend_state.dst_alpha_blendfactor = destination;
      blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
      blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;

      SDL_GPUColorTargetDescription color_target {};
      color_target.format = format;
      color_target.blend_state = blend_state;

      SDL_GPUGraphicsPipelineCreateInfo info {};
      info.vertex_shader = g_shaders.get(program.vertex_shader.idx).shader;
      info.fragment_shader = g_shaders.get(program.fragment_shader.idx).shader;
      info.vertex_input_state.vertex_buffer_descriptions = &buffer_description;
      info.vertex_input_state.num_vertex_buffers = 1;
      info.vertex_input_state.vertex_attributes = &attribute;
      info.vertex_input_state.num_vertex_attributes = 1;
      info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP;
      info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
      info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
      info.target_info.color_target_descriptions = &color_target;
      info.target_info.num_color_targets = 1;

      SDL_GPUGraphicsPipeline* pipeline = SDL_CreateGPUGraphicsPipeline(g_state.device, &info);
      if (pipeline == nullptr)
        VISAGE_LOG(String("Present pipeline creation failed: ") + SDL_GetError());

      g_present.pipelines[key] = pipeline;
      return pipeline;
    }
  }

  WindowTarget acquireWindowTarget(FrameBufferHandle handle) {
    if (!isValid(handle))
      return {};

    FrameBufferResource& resource = g_framebuffers.get(handle.idx);
    if (resource.window == nullptr)
      return {};

    // The UI's own draws were recorded before this call, so they belong on the
    // queue before whatever the application is about to record.
    executeCommands();

    if (g_pending_target.window != resource.window)
      acquireSwapchain(resource.window);

    WindowTarget target;
    target.command_buffer = g_pending_target.command_buffer;
    target.texture = g_pending_target.texture;
    target.width = static_cast<uint16_t>(g_pending_target.width);
    target.height = static_cast<uint16_t>(g_pending_target.height);
    return target;
  }

  void presentFrameBuffer(FrameBufferHandle handle, uint16_t dst_width, uint16_t dst_height,
                          int rotation_quarter_turns, bool blend) {
    if (!isValid(handle))
      return;

    FrameBufferResource& resource = g_framebuffers.get(handle.idx);
    if (resource.window == nullptr || !initPresentResources())
      return;

    // The layer being presented was drawn by commands that may still only be
    // recorded.
    executeCommands();

    if (g_pending_target.window != resource.window)
      acquireSwapchain(resource.window);

    // Consumed here: submitting this buffer is the present.
    SDL_GPUCommandBuffer* command_buffer = g_pending_target.command_buffer;
    SDL_GPUTexture* swapchain = g_pending_target.texture;
    g_pending_target = {};

    SDL_GPUGraphicsPipeline* pipeline = nullptr;
    if (swapchain) {
      SDL_GPUTextureFormat format = SDL_GetGPUSwapchainTextureFormat(g_state.device, resource.window);
      pipeline = presentPipeline(format, blend);
    }
    // A minimized window has no texture to draw into; the frame is dropped,
    // but the command buffer still has to be submitted rather than cancelled.
    if (pipeline == nullptr) {
      SDL_SubmitGPUCommandBuffer(command_buffer);
      return;
    }

    SDL_GPUColorTargetInfo target_info {};
    target_info.texture = swapchain;
    // The quad covers the whole swapchain, so its previous contents only
    // matter when compositing over what the application drew underneath.
    target_info.load_op = blend ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_DONT_CARE;
    target_info.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* render_pass = SDL_BeginGPURenderPass(command_buffer, &target_info, 1, nullptr);

    SDL_GPUViewport viewport {};
    viewport.w = dst_width;
    viewport.h = dst_height;
    viewport.max_depth = 1.0f;
    SDL_SetGPUViewport(render_pass, &viewport);
    SDL_BindGPUGraphicsPipeline(render_pass, pipeline);

    SDL_GPUBufferBinding vertex_binding {};
    vertex_binding.buffer = g_present.quad;
    SDL_BindGPUVertexBuffers(render_pass, 0, &vertex_binding, 1);

    TextureResource& texture = g_textures.get(resource.color_texture.idx);
    SDL_GPUTextureSamplerBinding sampler_binding {};
    sampler_binding.texture = texture.texture;
    sampler_binding.sampler = texture.sampler;
    SDL_BindGPUFragmentSamplers(render_pass, 0, &sampler_binding, 1);

    SDL_DrawGPUPrimitives(render_pass, kCornersPerQuad, 1,
                          (rotation_quarter_turns & 3) * kCornersPerQuad, 0);
    SDL_EndGPURenderPass(render_pass);
    SDL_SubmitGPUCommandBuffer(command_buffer);
  }

  const char* lastShaderError() {
    return g_state.last_shader_error.c_str();
  }
}
