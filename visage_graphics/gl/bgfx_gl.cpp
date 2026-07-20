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
#include "gl_api.h"
#include "renderer.h"
#include "visage_utils/defines.h"
#include "visage_utils/string_utils.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
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

    struct ShaderResource {
      GLuint gl_shader = 0;
      bool is_fragment = false;
    };

    struct ProgramResource {
      GLuint gl_program = 0;
      std::unordered_map<std::string, GLint> uniform_locations;
    };

    struct UniformResource {
      std::string name;
      UniformType::Enum type = UniformType::Vec4;
    };

    struct TextureResource {
      GLuint gl_texture = 0;
      TextureFormat::Enum format = TextureFormat::RGBA8;
      uint16_t width = 0;
      uint16_t height = 0;
    };

    struct FrameBufferResource {
      GLuint gl_framebuffer = 0;
      TextureHandle color_texture;
      TextureFormat::Enum format = TextureFormat::RGBA8;
      uint16_t width = 0;
      uint16_t height = 0;
      bool is_window = false;
    };

    struct IndexBufferResource {
      GLuint gl_buffer = 0;
      uint32_t num_indices = 0;
      bool index32 = false;
    };

    struct VertexBufferResource {
      GLuint gl_buffer = 0;
      VertexLayout layout;
    };

    struct PendingTextureBinding {
      bool bound = false;
      GLuint gl_texture = 0;
      std::string sampler_name;
    };

    struct UniformValue {
      UniformType::Enum type = UniformType::Vec4;
      float data[16] = {};
    };

    constexpr uint64_t kDefaultState = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A |
                                       BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_ZERO);
    constexpr int kMaxTextureStages = 4;

    // Per-draw state, reset after each submit() like bgfx's encoder state.
    struct DrawState {
      uint64_t state = kDefaultState;
      PendingTextureBinding textures[kMaxTextureStages];

      bool has_vertex_buffer = false;
      GLuint vertex_gl_buffer = 0;
      const VertexLayout* vertex_layout = nullptr;
      const uint8_t* pending_vertex_data = nullptr;
      uint32_t pending_vertex_size = 0;

      bool has_index_buffer = false;
      GLuint index_gl_buffer = 0;
      bool index32 = false;
      uint32_t index_count = 0;
      const uint8_t* pending_index_data = nullptr;
      uint32_t pending_index_size = 0;

      void reset() {
        state = kDefaultState;
        for (auto& texture : textures)
          texture.bound = false;
        has_vertex_buffer = false;
        pending_vertex_data = nullptr;
        has_index_buffer = false;
        pending_index_data = nullptr;
      }
    };

    struct BackendState {
      bool initialized = false;
      GLuint vao = 0;
      GLuint streaming_vbo = 0;
      GLuint streaming_ibo = 0;
      GLuint scratch_read_fbo = 0;
      GLuint scratch_draw_fbo = 0;
      bool r16f_renderable = true;
      uint32_t enabled_attribs = 0;

      Caps caps;
      Stats stats;
      uint32_t frame_count = 0;

      // Current view target; "last set wins" is correct because execution is
      // immediate and view ids only increase.
      FrameBufferHandle view_framebuffer;
      uint16_t view_width = 0;
      uint16_t view_height = 0;

      DrawState draw;
      // Uniform values persist across submits within and across frames,
      // matching bgfx semantics that post-effect passes rely on.
      std::unordered_map<std::string, UniformValue> uniform_values;

      // Transient buffer memory lives until the end of the frame.
      std::vector<std::unique_ptr<uint8_t[]>> transient_memory;

      std::string last_shader_error;

      GLuint present_program = 0;
      GLuint present_vbo = 0;
      GLint present_sampler_location = -1;
    };

    BackendState g_state;
    SlotTable<ShaderResource> g_shaders;
    SlotTable<ProgramResource> g_programs;
    SlotTable<UniformResource> g_uniforms;
    SlotTable<TextureResource> g_textures;
    SlotTable<FrameBufferResource> g_framebuffers;
    SlotTable<IndexBufferResource> g_index_buffers;
    SlotTable<VertexBufferResource> g_vertex_buffers;

    struct TextureFormatInfo {
      GLenum internal_format;
      GLenum format;
      GLenum type;
    };

    TextureFormatInfo formatInfo(TextureFormat::Enum format) {
      switch (format) {
      case TextureFormat::RGBA8:
      case TextureFormat::BGRA8: return { GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE };
      case TextureFormat::R32F: return { GL_R32F, GL_RED, GL_FLOAT };
      case TextureFormat::RGBA16F: return { GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT };
      case TextureFormat::RGB10A2: return { GL_RGB10_A2, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV };
      case TextureFormat::R16F:
        // GLES 3.0 can't render to R16F without EXT_color_buffer_(half_)float;
        // the path atlas then falls back to R8.
        if (g_state.r16f_renderable)
          return { GL_R16F, GL_RED, GL_HALF_FLOAT };
        return { GL_R8, GL_RED, GL_UNSIGNED_BYTE };
      default: VISAGE_ASSERT(false); return { GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE };
      }
    }

    GLenum blendFactorToGl(uint64_t factor) {
      switch (factor) {
      case BGFX_STATE_BLEND_ZERO: return GL_ZERO;
      case BGFX_STATE_BLEND_ONE: return GL_ONE;
      case BGFX_STATE_BLEND_SRC_ALPHA: return GL_SRC_ALPHA;
      case BGFX_STATE_BLEND_INV_SRC_ALPHA: return GL_ONE_MINUS_SRC_ALPHA;
      case BGFX_STATE_BLEND_DST_COLOR: return GL_DST_COLOR;
      default: return GL_ONE;
      }
    }

    void applyDrawStateBits(uint64_t state) {
      gl.colorMask((state & BGFX_STATE_WRITE_R) != 0, (state & BGFX_STATE_WRITE_G) != 0,
                   (state & BGFX_STATE_WRITE_B) != 0, (state & BGFX_STATE_WRITE_A) != 0);

      uint64_t src_rgb = (state >> 8) & 0x7;
      uint64_t dst_rgb = (state >> 11) & 0x7;
      uint64_t src_a = (state >> 14) & 0x7;
      uint64_t dst_a = (state >> 17) & 0x7;
      gl.blendFuncSeparate(blendFactorToGl(src_rgb), blendFactorToGl(dst_rgb),
                           blendFactorToGl(src_a), blendFactorToGl(dst_a));

      uint64_t eq_rgb = (state >> 20) & 0x1;
      uint64_t eq_a = (state >> 21) & 0x1;
      gl.blendEquationSeparate(eq_rgb ? GL_FUNC_REVERSE_SUBTRACT : GL_FUNC_ADD,
                               eq_a ? GL_FUNC_REVERSE_SUBTRACT : GL_FUNC_ADD);
    }

    void bindFramebufferForView(FrameBufferHandle handle, GLenum target) {
      if (!isValid(handle)) {
        gl.bindFramebuffer(target, 0);
        return;
      }
      gl.bindFramebuffer(target, g_framebuffers.get(handle.idx).gl_framebuffer);
    }

    // Attribute slots are fixed: the slot index is the Attrib enum value and
    // programs bind matching a_* names pre-link.
    const char* attribName(int slot) {
      static const char* kNames[] = { "a_position",  "a_color0",    "a_color1",    "a_color2",
                                      "a_texcoord0", "a_texcoord1", "a_texcoord2", "a_texcoord3" };
      return kNames[slot];
    }

    void applyVertexLayout(const VertexLayout& layout) {
      uint32_t needed_attribs = 0;
      for (int i = 0; i < layout.numEntries(); ++i) {
        const VertexLayout::Entry& entry = layout.entry(i);
        GLuint slot = static_cast<GLuint>(entry.attrib);
        needed_attribs |= 1u << slot;
        gl.enableVertexAttribArray(slot);
        gl.vertexAttribPointer(slot, entry.count, GL_FLOAT, GL_FALSE, layout.getStride(),
                               reinterpret_cast<const void*>(static_cast<uintptr_t>(entry.offset)));
      }

      // Stale enabled arrays would read through old buffer bindings.
      uint32_t stale = g_state.enabled_attribs & ~needed_attribs;
      for (GLuint slot = 0; stale != 0; ++slot, stale >>= 1) {
        if (stale & 1)
          gl.disableVertexAttribArray(slot);
      }
      g_state.enabled_attribs = needed_attribs;
    }

    GLint uniformLocation(ProgramResource& program, const std::string& name) {
      auto cached = program.uniform_locations.find(name);
      if (cached != program.uniform_locations.end())
        return cached->second;

      GLint location = gl.getUniformLocation(program.gl_program, name.c_str());
      program.uniform_locations[name] = location;
      return location;
    }

    // Rows from glReadPixels are bottom-up; visage expects top-down.
    void readPixelsTopDown(GLint width, GLint height, uint8_t* destination) {
      size_t row_bytes = static_cast<size_t>(width) * 4;
      std::vector<uint8_t> pixels(row_bytes * height);
      gl.pixelStorei(GL_PACK_ALIGNMENT, 1);
      gl.readPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
      for (GLint y = 0; y < height; ++y)
        std::memcpy(destination + row_bytes * y, pixels.data() + row_bytes * (height - 1 - y), row_bytes);
    }

    std::string shaderProfileHeader() {
      if (glContextIsEs())
        return "#version 300 es\nprecision highp float;\n";
      return "#version 330 core\nprecision highp float;\n";
    }

    std::string sharedShaderPrelude() {
      const EmbeddedFile& utils = visage::shaders::shader_utils;
      return std::string(reinterpret_cast<const char*>(utils.data), utils.size);
    }

    GLuint compileGlShader(GLenum type, const std::string& source, std::string& error) {
      GLuint shader = gl.createShader(type);
      const char* source_pointer = source.c_str();
      GLint length = static_cast<GLint>(source.size());
      gl.shaderSource(shader, 1, &source_pointer, &length);
      gl.compileShader(shader);

      GLint compiled = 0;
      gl.getShaderiv(shader, GL_COMPILE_STATUS, &compiled);
      if (compiled)
        return shader;

      char log[4096] = {};
      GLsizei log_length = 0;
      gl.getShaderInfoLog(shader, sizeof(log) - 1, &log_length, log);
      error = log;
      gl.deleteShader(shader);
      return 0;
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
    gl.deleteShader(g_shaders.get(handle.idx).gl_shader);
    g_shaders.release(handle.idx);
  }

  void destroy(ProgramHandle handle) {
    if (!isValid(handle))
      return;
    gl.deleteProgram(g_programs.get(handle.idx).gl_program);
    g_programs.release(handle.idx);
  }

  void destroy(UniformHandle handle) {
    if (!isValid(handle))
      return;
    g_uniforms.release(handle.idx);
  }

  void destroy(TextureHandle handle) {
    if (!isValid(handle))
      return;
    GLuint texture = g_textures.get(handle.idx).gl_texture;
    gl.deleteTextures(1, &texture);
    g_textures.release(handle.idx);
  }

  void destroy(FrameBufferHandle handle) {
    if (!isValid(handle))
      return;
    FrameBufferResource& resource = g_framebuffers.get(handle.idx);
    gl.deleteFramebuffers(1, &resource.gl_framebuffer);
    destroy(resource.color_texture);
    g_framebuffers.release(handle.idx);
  }

  void destroy(IndexBufferHandle handle) {
    if (!isValid(handle))
      return;
    GLuint buffer = g_index_buffers.get(handle.idx).gl_buffer;
    gl.deleteBuffers(1, &buffer);
    g_index_buffers.release(handle.idx);
  }

  void destroy(VertexBufferHandle handle) {
    if (!isValid(handle))
      return;
    GLuint buffer = g_vertex_buffers.get(handle.idx).gl_buffer;
    gl.deleteBuffers(1, &buffer);
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
    // copy() owns its bytes; makeRef() aliases caller storage.
    void releaseMemory(const Memory* memory, bool owns_data) {
      if (owns_data)
        delete[] memory->data;
      delete memory;
    }
  }

  ShaderHandle createShader(const Memory* memory) {
    std::string body(reinterpret_cast<const char*>(memory->data), memory->size);
    releaseMemory(memory, true);  // Always created via copy() in this codebase.

    // Only vertex shaders write gl_Position; fragment sources never mention it.
    bool is_fragment = body.find("gl_Position") == std::string::npos;

    // #line 1 keeps driver error messages aligned with the body text, which
    // is what live shader editors display.
    std::string source = shaderProfileHeader() + sharedShaderPrelude() + "\n#line 1\n" + body;

    std::string error;
    GLuint gl_shader = compileGlShader(is_fragment ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER, source, error);
    if (gl_shader == 0) {
      g_state.last_shader_error = error;
      VISAGE_LOG(String("GLSL compile error: ") + error.c_str());
      return {};
    }

    ShaderHandle handle;
    handle.idx = g_shaders.create({ gl_shader, is_fragment });
    return handle;
  }

  ProgramHandle createProgram(ShaderHandle vertex_shader, ShaderHandle fragment_shader, bool destroy_shaders) {
    if (!isValid(vertex_shader) || !isValid(fragment_shader))
      return {};

    GLuint program = gl.createProgram();
    gl.attachShader(program, g_shaders.get(vertex_shader.idx).gl_shader);
    gl.attachShader(program, g_shaders.get(fragment_shader.idx).gl_shader);
    for (int slot = 0; slot < VertexLayout::kMaxAttributes; ++slot)
      gl.bindAttribLocation(program, slot, attribName(slot));
    gl.linkProgram(program);

    GLint linked = 0;
    gl.getProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
      char log[4096] = {};
      GLsizei log_length = 0;
      gl.getProgramInfoLog(program, sizeof(log) - 1, &log_length, log);
      g_state.last_shader_error = log;
      VISAGE_LOG(String("GLSL link error: ") + log);
      gl.deleteProgram(program);
      return {};
    }

    ProgramHandle handle;
    handle.idx = g_programs.create({ program, {} });
    if (destroy_shaders) {
      destroy(vertex_shader);
      destroy(fragment_shader);
    }
    return handle;
  }

  UniformHandle createUniform(const char* name, UniformType::Enum type, uint16_t) {
    UniformHandle handle;
    handle.idx = g_uniforms.create({ name, type });
    return handle;
  }

  TextureHandle createTexture2D(uint16_t width, uint16_t height, bool, uint16_t,
                                TextureFormat::Enum format, uint64_t flags) {
    GLuint texture = 0;
    gl.genTextures(1, &texture);
    gl.bindTexture(GL_TEXTURE_2D, texture);

    TextureFormatInfo info = formatInfo(format);
    gl.texImage2D(GL_TEXTURE_2D, 0, info.internal_format, width, height, 0, info.format, info.type, nullptr);
    if (GLenum error = gl.getError()) {
      VISAGE_LOG(String("createTexture2D failed: format ") + static_cast<int>(format) + " " +
                 width + "x" + height + " glGetError " + static_cast<int>(error));
    }

    // 32-bit float textures aren't filterable in GLES 3.0.
    GLenum filter = format == TextureFormat::R32F ? GL_NEAREST : GL_LINEAR;
    gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                     (flags & BGFX_SAMPLER_U_CLAMP) ? GL_CLAMP_TO_EDGE : GL_REPEAT);
    gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                     (flags & BGFX_SAMPLER_V_CLAMP) ? GL_CLAMP_TO_EDGE : GL_REPEAT);

    // GLES has no GL_BGRA upload format; data stays BGRA in memory and the
    // red/blue swap happens at sample time via swizzle (core in both profiles).
    if (format == TextureFormat::BGRA8) {
      gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_BLUE);
      gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
    }

    TextureHandle handle;
    handle.idx = g_textures.create({ texture, format, width, height });
    return handle;
  }

  void updateTexture2D(TextureHandle handle, uint16_t, uint8_t, uint16_t x, uint16_t y,
                       uint16_t width, uint16_t height, const Memory* memory, uint16_t) {
    const TextureResource& resource = g_textures.get(handle.idx);
    TextureFormatInfo info = formatInfo(resource.format);

    gl.pixelStorei(GL_UNPACK_ALIGNMENT, 1);
    gl.bindTexture(GL_TEXTURE_2D, resource.gl_texture);
    gl.texSubImage2D(GL_TEXTURE_2D, 0, x, y, width, height, info.format, info.type, memory->data);
    if (GLenum error = gl.getError()) {
      VISAGE_LOG(String("updateTexture2D failed: format ") + static_cast<int>(resource.format) +
                 " " + width + "x" + height + " at " + x + "," + y + " glGetError " +
                 static_cast<int>(error));
    }
    releaseMemory(memory, true);
  }

  IndexBufferHandle createIndexBuffer(const Memory* memory, uint16_t) {
    GLuint buffer = 0;
    gl.genBuffers(1, &buffer);
    gl.bindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffer);
    gl.bufferData(GL_ELEMENT_ARRAY_BUFFER, memory->size, memory->data, GL_STATIC_DRAW);

    IndexBufferHandle handle;
    handle.idx = g_index_buffers.create({ buffer, memory->size / 2, false });
    releaseMemory(memory, false);
    return handle;
  }

  VertexBufferHandle createVertexBuffer(const Memory* memory, const VertexLayout& layout, uint16_t) {
    GLuint buffer = 0;
    gl.genBuffers(1, &buffer);
    gl.bindBuffer(GL_ARRAY_BUFFER, buffer);
    gl.bufferData(GL_ARRAY_BUFFER, memory->size, memory->data, GL_STATIC_DRAW);

    VertexBufferHandle handle;
    handle.idx = g_vertex_buffers.create({ buffer, layout });
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
    // The window framebuffer is a persistent offscreen texture, NOT the
    // default framebuffer: a raw double-buffered swap doesn't preserve
    // untouched pixels, which would break dirty-region redraw. The texture
    // reaches the screen through presentFrameBuffer() each frame.
    VISAGE_ASSERT(native_window_handle);
    FrameBufferHandle handle = createFrameBuffer(width, height, format,
                                                 BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
    if (isValid(handle))
      g_framebuffers.get(handle.idx).is_window = true;
    return handle;
  }

  FrameBufferHandle createFrameBuffer(uint16_t width, uint16_t height, TextureFormat::Enum format,
                                      uint64_t flags) {
    TextureHandle color_texture = createTexture2D(width, height, false, 1, format, flags);

    GLuint framebuffer = 0;
    gl.genFramebuffers(1, &framebuffer);
    gl.bindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    gl.framebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                            g_textures.get(color_texture.idx).gl_texture, 0);

    GLenum status = gl.checkFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE)
      VISAGE_LOG(String("Framebuffer incomplete: status ") + static_cast<int>(status) +
                 " format " + static_cast<int>(format));

    FrameBufferHandle handle;
    handle.idx = g_framebuffers.create({ framebuffer, color_texture, format, width, height, false });
    return handle;
  }

  TextureHandle getTexture(FrameBufferHandle handle, uint8_t) {
    if (!isValid(handle))
      return {};
    return g_framebuffers.get(handle.idx).color_texture;
  }

  void setViewMode(uint16_t, ViewMode::Enum) {
    // Immediate-mode execution is always sequential.
  }

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
    PendingTextureBinding& binding = g_state.draw.textures[stage];
    binding.bound = true;
    binding.gl_texture = g_textures.get(handle.idx).gl_texture;
    binding.sampler_name = g_uniforms.get(sampler.idx).name;
  }

  void setVertexBuffer(uint8_t, const TransientVertexBuffer* vertex_buffer) {
    // The caller fills vertex_buffer->data after this returns; the GL upload
    // is deferred to submit() when the data is complete.
    DrawState& draw = g_state.draw;
    draw.has_vertex_buffer = true;
    draw.vertex_gl_buffer = g_state.streaming_vbo;
    draw.vertex_layout = vertex_buffer->layout;
    draw.pending_vertex_data = vertex_buffer->data;
    draw.pending_vertex_size = vertex_buffer->size;
  }

  void setVertexBuffer(uint8_t, VertexBufferHandle handle) {
    VertexBufferResource& resource = g_vertex_buffers.get(handle.idx);
    DrawState& draw = g_state.draw;
    draw.has_vertex_buffer = true;
    draw.vertex_gl_buffer = resource.gl_buffer;
    draw.vertex_layout = &resource.layout;
    draw.pending_vertex_data = nullptr;
  }

  void setIndexBuffer(const TransientIndexBuffer* index_buffer) {
    DrawState& draw = g_state.draw;
    draw.has_index_buffer = true;
    draw.index_gl_buffer = g_state.streaming_ibo;
    draw.index32 = !index_buffer->isIndex16;
    draw.index_count = index_buffer->size / (draw.index32 ? 4 : 2);
    draw.pending_index_data = index_buffer->data;
    draw.pending_index_size = index_buffer->size;
  }

  void setIndexBuffer(IndexBufferHandle handle) {
    IndexBufferResource& resource = g_index_buffers.get(handle.idx);
    DrawState& draw = g_state.draw;
    draw.has_index_buffer = true;
    draw.index_gl_buffer = resource.gl_buffer;
    draw.index32 = resource.index32;
    draw.index_count = resource.num_indices;
    draw.pending_index_data = nullptr;
  }

  void submit(uint16_t, ProgramHandle program) {
    DrawState& draw = g_state.draw;
    if (!isValid(program) || !draw.has_vertex_buffer || !draw.has_index_buffer) {
      draw.reset();
      return;
    }

    ++g_state.stats.numDraw;

    bindFramebufferForView(g_state.view_framebuffer, GL_FRAMEBUFFER);
    gl.viewport(0, 0, g_state.view_width, g_state.view_height);
    applyDrawStateBits(draw.state);

    ProgramResource& program_resource = g_programs.get(program.idx);
    gl.useProgram(program_resource.gl_program);

    for (const auto& [name, value] : g_state.uniform_values) {
      GLint location = uniformLocation(program_resource, name);
      if (location < 0)
        continue;
      if (value.type == UniformType::Mat3)
        gl.uniformMatrix3fv(location, 1, GL_FALSE, value.data);
      else if (value.type == UniformType::Mat4)
        gl.uniformMatrix4fv(location, 1, GL_FALSE, value.data);
      else
        gl.uniform4fv(location, 1, value.data);
    }

    for (int stage = 0; stage < kMaxTextureStages; ++stage) {
      const PendingTextureBinding& binding = draw.textures[stage];
      if (!binding.bound)
        continue;
      gl.activeTexture(GL_TEXTURE0 + stage);
      gl.bindTexture(GL_TEXTURE_2D, binding.gl_texture);
      GLint location = uniformLocation(program_resource, binding.sampler_name);
      if (location >= 0)
        gl.uniform1i(location, stage);
    }
    gl.activeTexture(GL_TEXTURE0);

    // Deferred transient uploads happen here - the caller finished writing
    // its vertex/index data only after setVertexBuffer/setIndexBuffer.
    gl.bindVertexArray(g_state.vao);
    gl.bindBuffer(GL_ARRAY_BUFFER, draw.vertex_gl_buffer);
    if (draw.pending_vertex_data)
      gl.bufferData(GL_ARRAY_BUFFER, draw.pending_vertex_size, draw.pending_vertex_data, GL_STREAM_DRAW);
    applyVertexLayout(*draw.vertex_layout);

    gl.bindBuffer(GL_ELEMENT_ARRAY_BUFFER, draw.index_gl_buffer);
    if (draw.pending_index_data)
      gl.bufferData(GL_ELEMENT_ARRAY_BUFFER, draw.pending_index_size, draw.pending_index_data,
                    GL_STREAM_DRAW);

    gl.drawElements(GL_TRIANGLES, static_cast<GLsizei>(draw.index_count),
                    draw.index32 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT, nullptr);

    draw.reset();
  }

  void blit(uint16_t, TextureHandle dst, uint16_t dst_x, uint16_t dst_y, TextureHandle src,
            uint16_t src_x, uint16_t src_y, uint16_t width, uint16_t height) {
    const TextureResource& dst_resource = g_textures.get(dst.idx);
    const TextureResource& src_resource = g_textures.get(src.idx);
    if (width == UINT16_MAX)
      width = src_resource.width;
    if (height == UINT16_MAX)
      height = src_resource.height;

    gl.bindFramebuffer(GL_READ_FRAMEBUFFER, g_state.scratch_read_fbo);
    gl.framebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                            src_resource.gl_texture, 0);
    gl.bindFramebuffer(GL_DRAW_FRAMEBUFFER, g_state.scratch_draw_fbo);
    gl.framebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                            dst_resource.gl_texture, 0);
    gl.blitFramebuffer(src_x, src_y, src_x + width, src_y + height, dst_x, dst_y, dst_x + width,
                       dst_y + height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
  }

  void readTexture(TextureHandle handle, void* data, uint8_t) {
    const TextureResource& resource = g_textures.get(handle.idx);
    gl.bindFramebuffer(GL_READ_FRAMEBUFFER, g_state.scratch_read_fbo);
    gl.framebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                            resource.gl_texture, 0);
    readPixelsTopDown(resource.width, resource.height, static_cast<uint8_t*>(data));
  }

  void requestScreenShot(FrameBufferHandle handle, const char*) {
    const FrameBufferResource& resource = g_framebuffers.get(handle.idx);
    std::vector<uint8_t> pixels(static_cast<size_t>(resource.width) * resource.height * 4);
    gl.bindFramebuffer(GL_READ_FRAMEBUFFER, resource.gl_framebuffer);
    readPixelsTopDown(resource.width, resource.height, pixels.data());
    Renderer::instance().setScreenshotData(pixels.data(), resource.width, resource.height,
                                           resource.width * 4, false);
  }

  uint32_t frame(bool) {
    g_state.transient_memory.clear();
    return g_state.frame_count++;
  }

  void reset(uint32_t, uint32_t, uint32_t) {
    // The application owns the swap chain; nothing to do.
  }

  const Caps* getCaps() {
    return &g_state.caps;
  }

  const Stats* getStats() {
    return &g_state.stats;
  }

  const char* getRendererName(RendererType::Enum type) {
    return type == RendererType::OpenGLES ? "OpenGL ES" : "OpenGL";
  }

  RendererType::Enum getRendererType() {
    return g_state.caps.rendererType;
  }

  bool initGlBackend() {
    if (g_state.initialized)
      return true;
    if (!glApiLoaded())
      return false;

    gl.enable(GL_BLEND);
    gl.disable(GL_SCISSOR_TEST);
    gl.disable(GL_DEPTH_TEST);
    gl.disable(GL_CULL_FACE);
    // bgfx treats clockwise as front-facing (D3D convention) and the path
    // fill shader's coverage sign depends on gl_FrontFacing matching that.
    gl.frontFace(GL_CW);

    gl.genVertexArrays(1, &g_state.vao);
    gl.bindVertexArray(g_state.vao);
    gl.genBuffers(1, &g_state.streaming_vbo);
    gl.genBuffers(1, &g_state.streaming_ibo);
    gl.genFramebuffers(1, &g_state.scratch_read_fbo);
    gl.genFramebuffers(1, &g_state.scratch_draw_fbo);

    g_state.r16f_renderable = !glContextIsEs() ||
                              glContextHasExtension("GL_EXT_color_buffer_float") ||
                              glContextHasExtension("GL_EXT_color_buffer_half_float");
    if (!g_state.r16f_renderable)
      VISAGE_LOG("No color-renderable half-float support; path atlas falls back to R8");

    g_state.caps.rendererType = glContextIsEs() ? RendererType::OpenGLES : RendererType::OpenGL;
    g_state.caps.originBottomLeft = true;
    g_state.caps.supported = BGFX_CAPS_TEXTURE_BLIT | BGFX_CAPS_TEXTURE_READ_BACK |
                             BGFX_CAPS_INDEX32 | BGFX_CAPS_SWAP_CHAIN;

    g_state.initialized = true;
    return true;
  }

  namespace {
    // Present pass: one quad from the window framebuffer's texture to the
    // default framebuffer, rotated by permuting which texture corner each
    // vertex reads - no CPU pixel work, no per-fragment math.
    constexpr const char* kPresentVertex = R"(
in vec2 a_position;
in vec2 a_texcoord0;
out vec2 v_texture_uv;
void main() {
  v_texture_uv = a_texcoord0;
  gl_Position = vec4(a_position, 0.0, 1.0);
}
)";

    constexpr const char* kPresentFragment = R"(
in vec2 v_texture_uv;
uniform sampler2D s_texture;
out vec4 frag_color;
void main() {
  frag_color = texture(s_texture, v_texture_uv);
}
)";

    bool initPresentResources() {
      if (g_state.present_program)
        return true;

      std::string header = shaderProfileHeader();
      std::string error;
      GLuint vertex = compileGlShader(GL_VERTEX_SHADER, header + kPresentVertex, error);
      if (vertex == 0) {
        VISAGE_LOG(String("Present vertex shader error: ") + error.c_str());
        return false;
      }
      GLuint fragment = compileGlShader(GL_FRAGMENT_SHADER, header + kPresentFragment, error);
      if (fragment == 0) {
        VISAGE_LOG(String("Present fragment shader error: ") + error.c_str());
        gl.deleteShader(vertex);
        return false;
      }

      GLuint program = gl.createProgram();
      gl.attachShader(program, vertex);
      gl.attachShader(program, fragment);
      gl.bindAttribLocation(program, 0, "a_position");
      gl.bindAttribLocation(program, 1, "a_texcoord0");
      gl.linkProgram(program);
      gl.deleteShader(vertex);
      gl.deleteShader(fragment);

      GLint linked = 0;
      gl.getProgramiv(program, GL_LINK_STATUS, &linked);
      if (!linked) {
        VISAGE_LOG("Present shader failed to link");
        gl.deleteProgram(program);
        return false;
      }

      g_state.present_program = program;
      g_state.present_sampler_location = gl.getUniformLocation(program, "s_texture");
      gl.genBuffers(1, &g_state.present_vbo);
      return true;
    }
  }

  void presentFrameBuffer(FrameBufferHandle handle, uint16_t dst_width, uint16_t dst_height,
                          int rotation_quarter_turns) {
    if (!isValid(handle) || !initPresentResources())
      return;

    const FrameBufferResource& resource = g_framebuffers.get(handle.idx);

    // Triangle-strip corners BL, BR, TL, TR; texture corners as a clockwise
    // ring so rotation is a ring shift.
    static constexpr float kCornerX[] = { -1.0f, 1.0f, -1.0f, 1.0f };
    static constexpr float kCornerY[] = { -1.0f, -1.0f, 1.0f, 1.0f };
    static constexpr float kRingU[] = { 0.0f, 1.0f, 1.0f, 0.0f };
    static constexpr float kRingV[] = { 0.0f, 0.0f, 1.0f, 1.0f };
    static constexpr int kCornerToRing[] = { 0, 1, 3, 2 };

    float vertices[16];
    for (int corner = 0; corner < 4; ++corner) {
      int ring = (kCornerToRing[corner] + rotation_quarter_turns) & 3;
      vertices[corner * 4 + 0] = kCornerX[corner];
      vertices[corner * 4 + 1] = kCornerY[corner];
      vertices[corner * 4 + 2] = kRingU[ring];
      vertices[corner * 4 + 3] = kRingV[ring];
    }

    gl.bindFramebuffer(GL_FRAMEBUFFER, 0);
    gl.viewport(0, 0, dst_width, dst_height);
    gl.colorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    gl.blendFuncSeparate(GL_ONE, GL_ZERO, GL_ONE, GL_ZERO);
    gl.blendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);

    gl.useProgram(g_state.present_program);
    gl.activeTexture(GL_TEXTURE0);
    gl.bindTexture(GL_TEXTURE_2D, g_textures.get(resource.color_texture.idx).gl_texture);
    if (g_state.present_sampler_location >= 0)
      gl.uniform1i(g_state.present_sampler_location, 0);

    gl.bindVertexArray(g_state.vao);
    gl.bindBuffer(GL_ARRAY_BUFFER, g_state.present_vbo);
    gl.bufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STREAM_DRAW);

    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
    gl.enableVertexAttribArray(1);
    gl.vertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                           reinterpret_cast<const void*>(static_cast<uintptr_t>(2 * sizeof(float))));
    uint32_t stale = g_state.enabled_attribs & ~3u;
    for (GLuint slot = 0; stale != 0; ++slot, stale >>= 1) {
      if (stale & 1)
        gl.disableVertexAttribArray(slot);
    }
    g_state.enabled_attribs = 3u;

    gl.drawArrays(GL_TRIANGLE_STRIP, 0, 4);
  }

  const char* lastShaderError() {
    return g_state.last_shader_error.c_str();
  }
}
