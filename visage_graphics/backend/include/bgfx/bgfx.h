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

// This is NOT the real bgfx. It is a source-compatible reimplementation of
// the bgfx API subset visage uses. Two backends implement it: gl/ on OpenGL
// 3.3 core / OpenGL ES 3.0, and gpu/ on SDL_GPU (Vulkan/Metal/D3D12).
//
// Both rely on visage's view ids increasing monotonically within a frame, so
// bgfx's view-ordered submission collapses to submission order.
//
// createShader() takes plain GLSL text on the gl backend and a compiled blob
// from tools/shadertool on the gpu backend; the build embeds whichever the
// selected backend needs, under the same symbol names.

#include <cstdint>

namespace bgfx {
  // 0 is the invalid handle; live handles are 1-based slot indices.
  inline constexpr uint16_t kInvalidHandle = 0;

  struct ShaderHandle { uint16_t idx = kInvalidHandle; };
  struct ProgramHandle { uint16_t idx = kInvalidHandle; };
  struct UniformHandle { uint16_t idx = kInvalidHandle; };
  struct TextureHandle { uint16_t idx = kInvalidHandle; };
  struct FrameBufferHandle { uint16_t idx = kInvalidHandle; };
  struct IndexBufferHandle { uint16_t idx = kInvalidHandle; };
  struct VertexBufferHandle { uint16_t idx = kInvalidHandle; };

  bool isValid(ShaderHandle handle);
  bool isValid(ProgramHandle handle);
  bool isValid(UniformHandle handle);
  bool isValid(TextureHandle handle);
  bool isValid(FrameBufferHandle handle);
  bool isValid(IndexBufferHandle handle);
  bool isValid(VertexBufferHandle handle);

  void destroy(ShaderHandle handle);
  void destroy(ProgramHandle handle);
  void destroy(UniformHandle handle);
  void destroy(TextureHandle handle);
  void destroy(FrameBufferHandle handle);
  void destroy(IndexBufferHandle handle);
  void destroy(VertexBufferHandle handle);

  struct Memory {
    const uint8_t* data = nullptr;
    uint32_t size = 0;
  };

  // copy() duplicates the data; makeRef() aliases caller-owned storage that
  // must outlive its use. Both are released by the call that consumes them.
  const Memory* copy(const void* data, uint32_t size);
  const Memory* makeRef(const void* data, uint32_t size);

  struct UniformType {
    enum Enum { Sampler, End, Vec4, Mat3, Mat4, Count };
  };

  struct TextureFormat {
    enum Enum { RGBA8, BGRA8, R16F, R32F, RGBA16F, RGB10A2, Count };
  };

  struct RendererType {
    enum Enum { Noop, OpenGL, OpenGLES, Count };
  };

  struct ViewMode {
    enum Enum { Default, Sequential };
  };

  enum class Attrib : uint8_t {
    Position, Color0, Color1, Color2, TexCoord0, TexCoord1, TexCoord2, TexCoord3, Count
  };

  enum class AttribType : uint8_t { Float };

  // A struct, matching the forward declaration in visage's graphics_utils.h.
  struct VertexLayout {
    static constexpr int kMaxAttributes = static_cast<int>(Attrib::Count);

    struct Entry {
      Attrib attrib = Attrib::Position;
      uint8_t count = 0;
      uint16_t offset = 0;
    };

    VertexLayout& begin() {
      num_entries_ = 0;
      stride_ = 0;
      return *this;
    }

    VertexLayout& add(Attrib attrib, uint8_t count, AttribType) {
      entries_[num_entries_] = { attrib, count, stride_ };
      ++num_entries_;
      stride_ += count * static_cast<uint16_t>(sizeof(float));
      return *this;
    }

    void end() { }

    int numEntries() const { return num_entries_; }
    const Entry& entry(int index) const { return entries_[index]; }
    uint16_t getStride() const { return stride_; }

  private:
    Entry entries_[kMaxAttributes];
    int num_entries_ = 0;
    uint16_t stride_ = 0;
  };

  struct TransientVertexBuffer {
    uint8_t* data = nullptr;
    uint32_t size = 0;
    const VertexLayout* layout = nullptr;
  };

  struct TransientIndexBuffer {
    uint8_t* data = nullptr;
    uint32_t size = 0;
    bool isIndex16 = true;
  };

  bool allocTransientBuffers(TransientVertexBuffer* vertex_buffer, const VertexLayout& layout,
                             uint32_t num_vertices, TransientIndexBuffer* index_buffer,
                             uint32_t num_indices, bool index32 = false);
  bool allocTransientVertexBuffer(TransientVertexBuffer* vertex_buffer, uint32_t num_vertices,
                                  const VertexLayout& layout);

  // `memory` must hold plain GLSL text. The profile #version line, default
  // precision and the shared helper prelude are prepended here; vertex vs
  // fragment stage is detected from the source (vertex shaders reference
  // gl_Position). Returns an invalid handle on compile error - see
  // lastShaderError().
  ShaderHandle createShader(const Memory* memory);
  ProgramHandle createProgram(ShaderHandle vertex_shader, ShaderHandle fragment_shader,
                              bool destroy_shaders = false);
  UniformHandle createUniform(const char* name, UniformType::Enum type, uint16_t num = 1);

  TextureHandle createTexture2D(uint16_t width, uint16_t height, bool has_mips, uint16_t num_layers,
                                TextureFormat::Enum format, uint64_t flags = 0);
  void updateTexture2D(TextureHandle handle, uint16_t layer, uint8_t mip, uint16_t x, uint16_t y,
                       uint16_t width, uint16_t height, const Memory* memory,
                       uint16_t pitch = UINT16_MAX);

  IndexBufferHandle createIndexBuffer(const Memory* memory, uint16_t flags = 0);
  VertexBufferHandle createVertexBuffer(const Memory* memory, const VertexLayout& layout,
                                        uint16_t flags = 0);

  // Window-attached framebuffer. Backed by a persistent offscreen texture
  // (not the default framebuffer), so its contents survive buffer swaps and
  // dirty-region redraw stays valid; presentFrameBuffer() puts it on screen.
  // native_window_handle is never dereferenced but must be non-null.
  FrameBufferHandle createFrameBuffer(void* native_window_handle, uint16_t width, uint16_t height,
                                      TextureFormat::Enum format);
  FrameBufferHandle createFrameBuffer(uint16_t width, uint16_t height, TextureFormat::Enum format,
                                      uint64_t flags = 0);
  TextureHandle getTexture(FrameBufferHandle handle, uint8_t attachment = 0);

  void setViewMode(uint16_t view_id, ViewMode::Enum mode);
  void setViewRect(uint16_t view_id, uint16_t x, uint16_t y, uint16_t width, uint16_t height);
  void setViewFrameBuffer(uint16_t view_id, FrameBufferHandle handle);

  void setState(uint64_t state);
  void setUniform(UniformHandle handle, const void* value, uint16_t num = 1);
  void setTexture(uint8_t stage, UniformHandle sampler, TextureHandle handle);
  void setVertexBuffer(uint8_t stream, const TransientVertexBuffer* vertex_buffer);
  void setVertexBuffer(uint8_t stream, VertexBufferHandle handle);
  void setIndexBuffer(const TransientIndexBuffer* index_buffer);
  void setIndexBuffer(IndexBufferHandle handle);
  void submit(uint16_t view_id, ProgramHandle program);

  void blit(uint16_t view_id, TextureHandle dst, uint16_t dst_x, uint16_t dst_y, TextureHandle src,
            uint16_t src_x = 0, uint16_t src_y = 0, uint16_t width = UINT16_MAX,
            uint16_t height = UINT16_MAX);
  void readTexture(TextureHandle handle, void* data, uint8_t mip = 0);
  void requestScreenShot(FrameBufferHandle handle, const char* file_path);

  uint32_t frame(bool capture = false);
  void reset(uint32_t width, uint32_t height, uint32_t flags = 0);

  struct Caps {
    RendererType::Enum rendererType = RendererType::OpenGL;
    uint64_t supported = 0;
    bool originBottomLeft = true;
  };

  struct Stats {
    double waitRender = 0.0;
    double waitSubmit = 0.0;
    uint32_t numDraw = 0;
    uint16_t numViews = 0;
  };

  const Caps* getCaps();
  const Stats* getStats();
  const char* getRendererName(RendererType::Enum type);
  RendererType::Enum getRendererType();

  // ---- Extensions beyond the bgfx API ----

  // One-time backend setup. The gl backend requires loadGlApi() to have
  // succeeded on the thread whose context is current; the gpu backend creates
  // its own device here.
  // Whether the rendering context is gone. On the gl backend the driver behind
  // it is unloaded with the last window, so while this is set every destroy()
  // is dropped rather than dispatched into nothing. It clears again when a
  // window brings a live context back.
  void setContextLost(bool lost);

  bool initBackend();

  // The gpu backend's SDL_GPUDevice, so the windowing layer can claim windows
  // against it and an application can render into the same device. Null on the
  // gl backend, where the application owns the context instead.
  void* gpuDevice();

  // Where an application draws content the UI is composited over. On the gpu
  // backend, acquiring the window's swapchain texture is the only way to draw
  // into it, and it can only happen once per frame - so presentFrameBuffer()
  // reuses whatever this handed out. Everything recorded so far is flushed
  // first, which puts the application's pass after the UI's own draws.
  //
  // All null on the gl backend: there the application binds the default
  // framebuffer itself.
  struct WindowTarget {
    void* command_buffer = nullptr;  // SDL_GPUCommandBuffer*
    void* texture = nullptr;         // SDL_GPUTexture*
    uint16_t width = 0;
    uint16_t height = 0;
  };
  WindowTarget acquireWindowTarget(FrameBufferHandle handle);

  // Shader objects for an application drawing into gpuDevice() with pipelines
  // of its own: an SDL_GPUShader for a graphics stage and an
  // SDL_GPUComputePipeline for a kernel, both from a shadertool blob. Null on
  // the gl backend, where the application compiles GLSL itself.
  void* createGpuShader(const Memory* memory);
  void destroyGpuShader(void* shader);
  void* createComputePipeline(const Memory* memory);
  void destroyComputePipeline(void* pipeline);

  // Draws a window framebuffer's texture into the default framebuffer as one
  // full-screen quad, rotated by `rotation_quarter_turns` (0-3, clockwise).
  // Call once per frame right before the buffer swap; the destination size is
  // the physical drawable size in pixels. With `blend`, the quad composites
  // over whatever is already in the default framebuffer instead of replacing
  // it, so an application can draw its own content underneath.
  void presentFrameBuffer(FrameBufferHandle handle, uint16_t dst_width, uint16_t dst_height,
                          int rotation_quarter_turns, bool blend = false);

  // Compile log of the most recent failed createShader(), for live editing.
  const char* lastShaderError();
}

#define BGFX_INVALID_HANDLE \
  { ::bgfx::kInvalidHandle }

// Draw-state bits. The bit layout is internal to this backend - only the
// macro/constant names are bgfx-compatible.
inline constexpr uint64_t BGFX_STATE_WRITE_R = 1ull << 0;
inline constexpr uint64_t BGFX_STATE_WRITE_G = 1ull << 1;
inline constexpr uint64_t BGFX_STATE_WRITE_B = 1ull << 2;
inline constexpr uint64_t BGFX_STATE_WRITE_A = 1ull << 3;
inline constexpr uint64_t BGFX_STATE_WRITE_RGB = BGFX_STATE_WRITE_R | BGFX_STATE_WRITE_G |
                                                 BGFX_STATE_WRITE_B;
inline constexpr uint64_t BGFX_STATE_CONSERVATIVE_RASTER = 1ull << 4;

// Blend factors (3 bits per slot).
inline constexpr uint64_t BGFX_STATE_BLEND_ZERO = 1ull;
inline constexpr uint64_t BGFX_STATE_BLEND_ONE = 2ull;
inline constexpr uint64_t BGFX_STATE_BLEND_SRC_ALPHA = 3ull;
inline constexpr uint64_t BGFX_STATE_BLEND_INV_SRC_ALPHA = 4ull;
inline constexpr uint64_t BGFX_STATE_BLEND_DST_COLOR = 5ull;

inline constexpr uint64_t BGFX_STATE_BLEND_EQUATION_ADD = 0ull;
inline constexpr uint64_t BGFX_STATE_BLEND_EQUATION_REVSUB = 1ull;

#define BGFX_STATE_BLEND_FUNC_SEPARATE(src_rgb, dst_rgb, src_a, dst_a) \
  (((src_rgb) << 8) | ((dst_rgb) << 11) | ((src_a) << 14) | ((dst_a) << 17))
#define BGFX_STATE_BLEND_FUNC(src, dst) BGFX_STATE_BLEND_FUNC_SEPARATE(src, dst, src, dst)
#define BGFX_STATE_BLEND_EQUATION_SEPARATE(eq_rgb, eq_a) (((eq_rgb) << 20) | ((eq_a) << 21))
#define BGFX_STATE_BLEND_EQUATION(eq) BGFX_STATE_BLEND_EQUATION_SEPARATE(eq, eq)
#define BGFX_STATE_BLEND_MULTIPLY \
  BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_DST_COLOR, BGFX_STATE_BLEND_ZERO)

// Texture/sampler creation flags.
inline constexpr uint64_t BGFX_TEXTURE_RT = 1ull << 0;
inline constexpr uint64_t BGFX_TEXTURE_BLIT_DST = 1ull << 1;
inline constexpr uint64_t BGFX_TEXTURE_READ_BACK = 1ull << 2;
inline constexpr uint64_t BGFX_SAMPLER_U_CLAMP = 1ull << 3;
inline constexpr uint64_t BGFX_SAMPLER_V_CLAMP = 1ull << 4;

// Accepted for source compatibility; the application owns the swap chain so
// reset() ignores them.
inline constexpr uint32_t BGFX_RESET_VSYNC = 1u << 0;
inline constexpr uint32_t BGFX_RESET_FLIP_AFTER_RENDER = 1u << 1;
inline constexpr uint32_t BGFX_RESET_FLUSH_AFTER_RENDER = 1u << 2;

// Capability bits. Only TEXTURE_BLIT, TEXTURE_READ_BACK, INDEX32, SWAP_CHAIN
// and CONSERVATIVE_RASTER are meaningfully queried; the rest exist so debug
// listings still compile.
inline constexpr uint64_t BGFX_CAPS_ALPHA_TO_COVERAGE = 1ull << 0;
inline constexpr uint64_t BGFX_CAPS_BLEND_INDEPENDENT = 1ull << 1;
inline constexpr uint64_t BGFX_CAPS_COMPUTE = 1ull << 2;
inline constexpr uint64_t BGFX_CAPS_CONSERVATIVE_RASTER = 1ull << 3;
inline constexpr uint64_t BGFX_CAPS_DRAW_INDIRECT = 1ull << 4;
inline constexpr uint64_t BGFX_CAPS_FRAGMENT_DEPTH = 1ull << 5;
inline constexpr uint64_t BGFX_CAPS_FRAGMENT_ORDERING = 1ull << 6;
inline constexpr uint64_t BGFX_CAPS_GRAPHICS_DEBUGGER = 1ull << 7;
inline constexpr uint64_t BGFX_CAPS_HDR10 = 1ull << 8;
inline constexpr uint64_t BGFX_CAPS_HIDPI = 1ull << 9;
inline constexpr uint64_t BGFX_CAPS_IMAGE_RW = 1ull << 10;
inline constexpr uint64_t BGFX_CAPS_INDEX32 = 1ull << 11;
inline constexpr uint64_t BGFX_CAPS_INSTANCING = 1ull << 12;
inline constexpr uint64_t BGFX_CAPS_OCCLUSION_QUERY = 1ull << 13;
inline constexpr uint64_t BGFX_CAPS_RENDERER_MULTITHREADED = 1ull << 14;
inline constexpr uint64_t BGFX_CAPS_SWAP_CHAIN = 1ull << 15;
inline constexpr uint64_t BGFX_CAPS_TEXTURE_2D_ARRAY = 1ull << 16;
inline constexpr uint64_t BGFX_CAPS_TEXTURE_3D = 1ull << 17;
inline constexpr uint64_t BGFX_CAPS_TEXTURE_BLIT = 1ull << 18;
inline constexpr uint64_t BGFX_CAPS_TEXTURE_COMPARE_LEQUAL = 1ull << 19;
inline constexpr uint64_t BGFX_CAPS_TEXTURE_CUBE_ARRAY = 1ull << 20;
inline constexpr uint64_t BGFX_CAPS_TEXTURE_DIRECT_ACCESS = 1ull << 21;
inline constexpr uint64_t BGFX_CAPS_TEXTURE_READ_BACK = 1ull << 22;
inline constexpr uint64_t BGFX_CAPS_VERTEX_ATTRIB_HALF = 1ull << 23;
inline constexpr uint64_t BGFX_CAPS_VERTEX_ATTRIB_UINT10 = 1ull << 24;
inline constexpr uint64_t BGFX_CAPS_VERTEX_ID = 1ull << 25;
inline constexpr uint64_t BGFX_CAPS_VIEWPORT_LAYER_ARRAY = 1ull << 26;
