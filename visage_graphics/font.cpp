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

#include "font.h"

#include "emoji.h"
#include "visage_utils/file_system.h"
#include "visage_utils/thread_utils.h"

#include <bgfx/bgfx.h>
#include <cstdint>
#include <cstring>
#include <freetype/freetype.h>
#include <freetype/ftbitmap.h>
#include <freetype/ftsizes.h>
#include <memory>
#include <set>
#include <string>

#if VISAGE_HARFBUZZ
#include <hb-ft.h>
#include <hb.h>
#endif
#include <vector>

namespace visage {

  class FreeTypeLibrary {
  public:
    static FreeTypeLibrary& instance() {
      static FreeTypeLibrary instance;
      return instance;
    }

    static FT_Face newMemoryFace(const unsigned char* data, int data_size) {
      FT_Face face = nullptr;
      FT_New_Memory_Face(instance().library_, data, data_size, 0, &face);
      instance().faces_.insert(face);
      return face;
    }

    static void doneFace(FT_Face face) {
      VISAGE_ASSERT(instance().faces_.count(face));
      if (instance().faces_.count(face) == 0)
        return;

      FT_Done_Face(face);
      instance().faces_.erase(face);
    }

    static FT_Library library() { return instance().library_; }

    static std::string idForFont(const unsigned char* data, int data_size) {
      FT_Face face = newMemoryFace(data, data_size);
      std::string id = std::string(face->family_name) + "-" + std::string(face->style_name);
      doneFace(face);
      return id;
    }

  private:
    FreeTypeLibrary() { FT_Init_FreeType(&library_); }
    ~FreeTypeLibrary() {
      for (FT_Face face : faces_)
        FT_Done_Face(face);
      FT_Done_FreeType(library_);
    }

    std::set<FT_Face> faces_;
    FT_Library library_ = nullptr;
  };

  class TypeFace {
  public:
    TypeFace(const TypeFace&) = delete;
    TypeFace& operator=(const TypeFace&) = delete;

    TypeFace(int size, const unsigned char* data, int data_size) {
      face_ = FreeTypeLibrary::newMemoryFace(data, data_size);
      FT_Set_Pixel_Sizes(face_, 0, std::max(0, size));
    }

    ~TypeFace() { FreeTypeLibrary::doneFace(face_); }

    int numGlyphs() const { return face_->num_glyphs; }
    std::string familyName() const { return face_->family_name; }
    std::string styleName() const { return face_->style_name; }

    int glyphIndex(char32_t character) const { return FT_Get_Char_Index(face_, character); }
    int lineHeight() const { return face_->size->metrics.height >> 6; }

    FT_Face face() const { return face_; }

  private:
    FT_Face face_ = nullptr;
  };

  // The first byte of a bitmap's row y, counted from the top whichever way
  // its rows flow.
  static const unsigned char* bitmapRow(const FT_Bitmap& bitmap, int y) {
    const unsigned char* top = bitmap.buffer;
    if (bitmap.pitch < 0)
      top -= bitmap.pitch * (static_cast<int>(bitmap.rows) - 1);
    return top + y * bitmap.pitch;
  }

  // A rendered glyph into the atlas's pixels, white with the coverage as
  // alpha, whatever depth FreeType handed back: an embedded strike can be
  // 1, 2 or 4 bits deep, and a colour glyph is premultiplied BGRA.
  static void copyGlyphBitmap(const FT_Bitmap& bitmap, int width, int height, unsigned int* dest) {
    if (bitmap.pixel_mode == FT_PIXEL_MODE_BGRA) {
      int rows = std::min(height, static_cast<int>(bitmap.rows));
      int columns = std::min(width, static_cast<int>(bitmap.width));
      for (int y = 0; y < rows; ++y)
        std::memcpy(dest + y * width, bitmapRow(bitmap, y), columns * sizeof(unsigned int));
      return;
    }

    FT_Bitmap converted;
    FT_Bitmap_Init(&converted);
    const FT_Bitmap* gray = &bitmap;
    if (bitmap.pixel_mode != FT_PIXEL_MODE_GRAY) {
      if (FT_Bitmap_Convert(FreeTypeLibrary::library(), &bitmap, &converted, 1) == 0)
        gray = &converted;
      else
        gray = nullptr;
    }

    if (gray) {
      int rows = std::min(height, static_cast<int>(gray->rows));
      int columns = std::min(width, static_cast<int>(gray->width));
      int max_level = std::max(1, gray->num_grays - 1);
      for (int y = 0; y < rows; ++y) {
        const unsigned char* row = bitmapRow(*gray, y);
        for (int x = 0; x < columns; ++x) {
          unsigned int coverage = max_level == 255 ? row[x] : row[x] * 255 / max_level;
          dest[y * width + x] = (coverage << 24) + 0xffffff;
        }
      }
    }
    FT_Bitmap_Done(FreeTypeLibrary::library(), &converted);
  }

  // The faces tried in order for a character a font lacks, before the emoji
  // face. Opened from their files on first need, so a chain costs nothing
  // until a character is missing, and read by FreeType on demand rather than
  // held in memory. Fonts share one chain; each renders from it at its own
  // size through an FT_Size of its own.
  class FallbackChain {
  public:
    explicit FallbackChain(std::vector<FallbackFace> files) :
        files_(std::move(files)), faces_(files_.size(), nullptr), opened_(files_.size(), false) { }

    ~FallbackChain() {
      for (FT_Face face : faces_) {
        if (face)
          FT_Done_Face(face);
      }
    }

    // The first face that has the character, or -1.
    int faceFor(char32_t character) {
      auto found = coverage_.find(character);
      if (found != coverage_.end())
        return found->second;

      int covering = -1;
      for (int i = 0; i < static_cast<int>(files_.size()) && covering < 0; ++i) {
        FT_Face chain_face = face(i);
        if (chain_face && FT_Get_Char_Index(chain_face, character))
          covering = i;
      }
      coverage_[character] = covering;
      return covering;
    }

    FT_Face face(int index) {
      if (!opened_[index]) {
        opened_[index] = true;
        const FallbackFace& file = files_[index];
        if (FT_New_Face(FreeTypeLibrary::library(), file.path.c_str(), file.index, &faces_[index]))
          faces_[index] = nullptr;
      }
      return faces_[index];
    }

  private:
    std::vector<FallbackFace> files_;
    std::vector<FT_Face> faces_;
    std::vector<bool> opened_;
    std::map<char32_t, int> coverage_;
  };

  static std::shared_ptr<FallbackChain>& fallbackChain() {
    FreeTypeLibrary::instance();
    static std::shared_ptr<FallbackChain> chain;
    return chain;
  }

  // Each setting of the chain, so fonts made after it keep their own atlases.
  static int& fallbackGeneration() {
    static int generation = 0;
    return generation;
  }

  static bool& shapingState() {
    static bool shaping = false;
    return shaping;
  }

#if VISAGE_HARFBUZZ
  // One buffer serves every shaping call, all on the main thread.
  static hb_buffer_t* shapingBuffer() {
    static hb_buffer_t* buffer = hb_buffer_create();
    return buffer;
  }

  // A joiner or selector that belongs to the run before it, whatever face
  // covers it: HarfBuzz hides it or uses it there.
  static bool isDefaultIgnorable(char32_t character) {
    return character == 0x200c || character == 0x200d || (character >= 0xfe00 && character <= 0xfe0f) ||
           (character >= 0xe0100 && character <= 0xe01ef);
  }

  static bool isMark(char32_t character) {
    switch (hb_unicode_general_category(hb_unicode_funcs_get_default(), character)) {
    case HB_UNICODE_GENERAL_CATEGORY_NON_SPACING_MARK:
    case HB_UNICODE_GENERAL_CATEGORY_SPACING_MARK:
    case HB_UNICODE_GENERAL_CATEGORY_ENCLOSING_MARK: return true;
    default: return false;
    }
  }
#endif

  class PackedFont {
  public:
    static constexpr int kChannels = 4;

    // A string laid out once by shaping: each glyph's origin from the start
    // of the string's baseline, and the whole string's advance.
    struct ShapedGlyph {
      const PackedGlyph* glyph = nullptr;
      float x = 0.0f;
      float y = 0.0f;
    };
    struct ShapedRun {
      std::vector<ShapedGlyph> glyphs;
      float width = 0.0f;
    };
    // Strings shaped and kept, so text drawn every frame is shaped once;
    // the cache empties when full.
    static constexpr size_t kMaxShapedRuns = 1024;

    // A glyph is its face and its index there: 0 the font's own face, 1 and
    // on the chain's, and the emoji face with the codepoint as its index.
    using GlyphKey = uint64_t;
    static constexpr uint32_t kEmojiFace = 0xffffffff;
    static GlyphKey glyphKey(uint32_t face, uint32_t index) {
      return (static_cast<GlyphKey>(face) << 32) | index;
    }
    static uint32_t keyFace(GlyphKey key) { return key >> 32; }
    static uint32_t keyIndex(GlyphKey key) { return key & 0xffffffff; }

    // data is the font cache's copy of the file, shared by every size.
    PackedFont(const std::string& id, int size, const unsigned char* data, int data_size,
               FT_Int32 load_target, std::shared_ptr<FallbackChain> chain) :
        id_(id), size_(size), data_(data), data_size_(data_size), load_target_(load_target),
        chain_(std::move(chain)) {
      type_face_ = std::make_unique<TypeFace>(size, data_, data_size);
      characters_['\n'] = &null_glyph_;
    }

    ~PackedFont() {
      if (bgfx::isValid(texture_handle_))
        bgfx::destroy(texture_handle_);
#if VISAGE_HARFBUZZ
      for (auto& hb_font : hb_fonts_)
        hb_font_destroy(hb_font.second);
#endif
      for (auto& fallback_size : fallback_sizes_)
        FT_Done_Size(fallback_size.second);
      type_face_ = nullptr;
    }

    void resize() {
      if (bgfx::isValid(texture_handle_)) {
        bgfx::destroy(texture_handle_);
        texture_handle_ = BGFX_INVALID_HANDLE;
      }

      atlas_map_.pack();
      for (auto& glyph : glyphs_) {
        if (glyph.second.width == 0)
          continue;

        const PackedRect& rect = atlas_map_.rectForId(glyph.first);
        glyph.second.atlas_left = rect.x;
        glyph.second.atlas_top = rect.y;
      }
    }

    void rasterizeGlyph(GlyphKey key, const PackedGlyph* packed_glyph) {
      int size = packed_glyph->width * packed_glyph->height;
      if (size == 0)
        return;

      std::unique_ptr<unsigned int[]> texture = std::make_unique<unsigned int[]>(size);
      if (keyFace(key) == kEmojiFace) {
        EmojiRasterizer::instance().drawIntoBuffer(keyIndex(key), size_, packed_glyph->width,
                                                   texture.get(), packed_glyph->width, 0, 0);
      }
      else if (FT_Face face = activeFace(keyFace(key))) {
        if (FT_Load_Glyph(face, keyIndex(key), FT_LOAD_RENDER | load_target_) == 0)
          copyGlyphBitmap(face->glyph->bitmap, packed_glyph->width, packed_glyph->height, texture.get());
      }

      bgfx::updateTexture2D(texture_handle_, 0, 0, packed_glyph->atlas_left,
                            packed_glyph->atlas_top, packed_glyph->width, packed_glyph->height,
                            bgfx::copy(texture.get(), size * kChannels));
    }

    PackedGlyph* packFaceGlyph(uint32_t face_number, uint32_t glyph_index) {
      static constexpr float kAdvanceMult = 1.0f / (1 << 6);

      GlyphKey key = glyphKey(face_number, glyph_index);
      auto found = glyphs_.find(key);
      if (found != glyphs_.end())
        return &found->second;

      PackedGlyph* packed_glyph = &glyphs_[key];
      FT_Face face = activeFace(face_number);
      if (face == nullptr || FT_Load_Glyph(face, glyph_index, load_target_)) {
        *packed_glyph = Font::kNullPackedGlyph;
        return packed_glyph;
      }

      FT_GlyphSlot glyph = face->glyph;
      packed_glyph->width = glyph->bitmap.width;
      packed_glyph->height = glyph->bitmap.rows;
      packed_glyph->x_offset = glyph->bitmap_left;
      packed_glyph->y_offset = glyph->bitmap_top;
      packed_glyph->x_advance = glyph->advance.x * kAdvanceMult;

      packGlyph(packed_glyph, key);
      return packed_glyph;
    }

    PackedGlyph* packEmojiGlyph(char32_t emoji) {
      GlyphKey key = glyphKey(kEmojiFace, emoji);
      auto found = glyphs_.find(key);
      if (found != glyphs_.end())
        return &found->second;

      PackedGlyph* packed_glyph = &glyphs_[key];
      int raster_width = lineHeight();
      packed_glyph->width = raster_width;
      packed_glyph->height = raster_width;
      packed_glyph->x_offset = 0;
      packed_glyph->y_offset = size_;
      packed_glyph->x_advance = raster_width;

      packGlyph(packed_glyph, key);
      return packed_glyph;
    }

    const PackedGlyph* packedGlyph(char32_t character) {
      auto found = characters_.find(character);
      if (found != characters_.end())
        return found->second;

      PackedGlyph* packed_glyph = nullptr;
      if (int index = type_face_->glyphIndex(character))
        packed_glyph = packFaceGlyph(0, index);
      else if (int chain_face = chain_ ? chain_->faceFor(character) : -1; chain_face >= 0)
        packed_glyph = packFaceGlyph(chain_face + 1, FT_Get_Char_Index(chain_->face(chain_face), character));
      else
        packed_glyph = packEmojiGlyph(character);

      characters_[character] = packed_glyph;
      return packed_glyph;
    }

#if VISAGE_HARFBUZZ
    // The string shaped: split into runs by the face that covers each
    // character, as packedGlyph chooses, a mark or joiner staying in the run
    // before it where that face has it; each run shaped left to right, the
    // emoji face's characters drawn one by one.
    const ShapedRun& shapedRun(const char32_t* text, int length) {
      std::u32string key(text, length);
      auto found = runs_.find(key);
      if (found != runs_.end())
        return found->second;

      if (runs_.size() >= kMaxShapedRuns)
        runs_.clear();

      ShapedRun& run = runs_[key];
      float pen = 0.0f;
      int start = 0;
      while (start < length) {
        char32_t character = text[start];
        if (Font::isNewLine(character) || Font::isIgnored(character) || isDefaultIgnorable(character)) {
          ++start;
          continue;
        }

        uint32_t face_number = faceNumberFor(character);
        if (face_number == kEmojiFace) {
          const PackedGlyph* glyph = packEmojiGlyph(character);
          run.glyphs.push_back({ glyph, pen + glyph->x_offset, -glyph->y_offset });
          pen += glyph->x_advance;
          ++start;
          continue;
        }

        int end = start + 1;
        while (end < length && continuesRun(text[end], face_number))
          ++end;
        shapeRun(run, text, length, start, end - start, face_number, pen);
        start = end;
      }
      run.width = pen;
      return run;
    }
#endif

    void checkInit() {
      if (!bgfx::isValid(texture_handle_)) {
        texture_handle_ = bgfx::createTexture2D(atlas_map_.width(), atlas_map_.height(), false, 1,
                                                bgfx::TextureFormat::BGRA8);
        int width = atlas_map_.width();
        int height = atlas_map_.height();
        std::unique_ptr<unsigned int[]> clear = std::make_unique<unsigned int[]>(width * height);
        bgfx::updateTexture2D(texture_handle_, 0, 0, 0, 0, width, height,
                              bgfx::copy(clear.get(), width * height * kChannels));

        for (auto& glyph : glyphs_)
          rasterizeGlyph(glyph.first, &glyph.second);
      }
    }

    int atlasWidth() const { return atlas_map_.width(); }
    int atlasHeight() const { return atlas_map_.height(); }
    bgfx::TextureHandle& textureHandle() { return texture_handle_; }
    int lineHeight() const { return type_face_->lineHeight(); }
    int size() const { return size_; }
    const unsigned char* data() const { return data_; }
    int dataSize() const { return data_size_; }
    const std::string& id() const { return id_; }

  private:
    // The face packedGlyph draws a character from: 0 the font's own, 1 and on
    // the chain's, or the emoji face.
    uint32_t faceNumberFor(char32_t character) {
      if (type_face_->glyphIndex(character))
        return 0;
      int chain_face = chain_ ? chain_->faceFor(character) : -1;
      return chain_face >= 0 ? chain_face + 1 : kEmojiFace;
    }

#if VISAGE_HARFBUZZ
    bool faceHas(uint32_t face_number, char32_t character) {
      if (face_number == 0)
        return type_face_->glyphIndex(character);
      FT_Face face = chain_ ? chain_->face(face_number - 1) : nullptr;
      return face && FT_Get_Char_Index(face, character);
    }

    bool continuesRun(char32_t character, uint32_t face_number) {
      if (Font::isNewLine(character) || character == '\r')
        return false;
      if (isDefaultIgnorable(character) || (isMark(character) && faceHas(face_number, character)))
        return true;
      return faceNumberFor(character) == face_number;
    }

    // HarfBuzz's font for a face at this font's size, through hb-ft so its
    // advances are FreeType's, hinted as the glyphs are.
    hb_font_t* hbFont(uint32_t face_number) {
      FT_Face face = activeFace(face_number);
      if (face == nullptr)
        return nullptr;

      auto found = hb_fonts_.find(face_number);
      if (found != hb_fonts_.end())
        return found->second;

      hb_font_t* font = hb_ft_font_create_referenced(face);
      hb_ft_font_set_load_flags(font, load_target_);
      hb_fonts_[face_number] = font;
      return font;
    }

    void shapeRun(ShapedRun& run, const char32_t* text, int length, int start, int count,
                  uint32_t face_number, float& pen) {
      static constexpr float kPositionMult = 1.0f / (1 << 6);

      hb_font_t* font = hbFont(face_number);
      if (font == nullptr)
        return;

      hb_buffer_t* buffer = shapingBuffer();
      hb_buffer_clear_contents(buffer);
      hb_buffer_add_utf32(buffer, reinterpret_cast<const uint32_t*>(text), length, start, count);
      hb_buffer_guess_segment_properties(buffer);
      // Left to right only: the letters of a right-to-left script stay in
      // their logical order.
      hb_buffer_set_direction(buffer, HB_DIRECTION_LTR);
      hb_shape(font, buffer, nullptr, 0);

      unsigned int glyph_count = 0;
      const hb_glyph_info_t* infos = hb_buffer_get_glyph_infos(buffer, &glyph_count);
      const hb_glyph_position_t* positions = hb_buffer_get_glyph_positions(buffer, &glyph_count);
      for (unsigned int i = 0; i < glyph_count; ++i) {
        // A character no face covers comes through as glyph 0: the emoji
        // face has the last word, as unshaped.
        const PackedGlyph* glyph = infos[i].codepoint ? packFaceGlyph(face_number, infos[i].codepoint)
                                                      : packEmojiGlyph(text[infos[i].cluster]);
        run.glyphs.push_back({ glyph, pen + positions[i].x_offset * kPositionMult + glyph->x_offset,
                               -positions[i].y_offset * kPositionMult - glyph->y_offset });
        pen += positions[i].x_advance * kPositionMult;
      }
    }
#endif

    // A face ready to load glyphs at this font's size: its own, or a chain
    // face with this font's size made active on it.
    FT_Face activeFace(uint32_t face_number) {
      if (face_number == 0)
        return type_face_->face();

      int chain_index = face_number - 1;
      FT_Face face = chain_ ? chain_->face(chain_index) : nullptr;
      if (face == nullptr)
        return nullptr;

      auto found = fallback_sizes_.find(chain_index);
      if (found != fallback_sizes_.end()) {
        FT_Activate_Size(found->second);
        return face;
      }

      FT_Size size = nullptr;
      if (FT_New_Size(face, &size))
        return nullptr;
      FT_Activate_Size(size);
      FT_Set_Pixel_Sizes(face, 0, std::max(0, size_));
      fallback_sizes_[chain_index] = size;
      return face;
    }

    void packGlyph(PackedGlyph* packed_glyph, GlyphKey key) {
      if (!atlas_map_.addRect(key, packed_glyph->width, packed_glyph->height))
        resize();

      const PackedRect& rect = atlas_map_.rectForId(key);
      packed_glyph->atlas_left = rect.x;
      packed_glyph->atlas_top = rect.y;

      if (bgfx::isValid(texture_handle_))
        rasterizeGlyph(key, packed_glyph);
    }

    PackedAtlasMap<GlyphKey> atlas_map_;
    std::unique_ptr<TypeFace> type_face_;
    std::string id_;
    int size_ = 0;
    const unsigned char* data_ = nullptr;
    int data_size_ = 0;
    FT_Int32 load_target_ = FT_LOAD_TARGET_NORMAL;
    std::shared_ptr<FallbackChain> chain_;
    std::map<int, FT_Size> fallback_sizes_;

    std::map<GlyphKey, PackedGlyph> glyphs_;
    std::map<char32_t, PackedGlyph*> characters_;
    PackedGlyph null_glyph_ = Font::kNullPackedGlyph;
#if VISAGE_HARFBUZZ
    std::map<uint32_t, hb_font_t*> hb_fonts_;
    std::map<std::u32string, ShapedRun> runs_;
#endif
    bgfx::TextureHandle texture_handle_ = { bgfx::kInvalidHandle };
  };

  static TextRendering& textRendering() {
    static TextRendering rendering;
    return rendering;
  }

  void Font::setRendering(const TextRendering& rendering) {
    textRendering() = rendering;
  }

  const TextRendering& Font::rendering() {
    return textRendering();
  }

  void Font::setShaping(bool shaping) {
    shapingState() = shaping && shapingAvailable();
  }

  bool Font::shaping() {
    return shapingState();
  }

  bool Font::shapingAvailable() {
#if VISAGE_HARFBUZZ
    return true;
#else
    return false;
#endif
  }

  void Font::setFallbackFaces(std::vector<FallbackFace> faces) {
    fallbackChain() = faces.empty() ? nullptr : std::make_shared<FallbackChain>(std::move(faces));
    ++fallbackGeneration();
  }

  // The cache's id for a face at a size under the current rendering: a
  // hinting and a fallback chain each rasterize or cover differently, so
  // each keeps its own atlas; visage's defaults keep the plain id.
  static std::string cacheId(const std::string& face_id) {
    std::string id = face_id;
    if (Font::rendering().hinting == TextRendering::Hinting::Light)
      id += " - light";
    if (fallbackChain())
      id += " - fallback " + std::to_string(fallbackGeneration());
    return id;
  }

  bool Font::hasNewLine(const char32_t* string, int length) {
    for (int i = 0; i < length; ++i) {
      if (isNewLine(string[i]))
        return true;
    }
    return false;
  }

  Font::Font(float size, const unsigned char* font_data, int data_size, float dpi_scale) :
      size_(size), dpi_scale_(dpi_scale) {
    native_size_ = std::round(size * (dpi_scale ? dpi_scale : 1.0f));
    packed_font_ = FontCache::loadPackedFont(native_size_, font_data, data_size);
  }

  Font::Font(float size, const EmbeddedFile& file, float dpi_scale) :
      size_(size), dpi_scale_(dpi_scale) {
    native_size_ = std::round(size * (dpi_scale ? dpi_scale : 1.0f));
    packed_font_ = FontCache::loadPackedFont(native_size_, file);
  }

  Font::Font(float size, const std::string& file_path, float dpi_scale) :
      size_(size), dpi_scale_(dpi_scale) {
    native_size_ = std::round(size * (dpi_scale ? dpi_scale : 1.0f));
    packed_font_ = FontCache::loadPackedFont(native_size_, file_path);
  }

  Font::Font(const Font& other) {
    size_ = other.size_;
    native_size_ = other.native_size_;
    dpi_scale_ = other.dpi_scale_;
    packed_font_ = FontCache::loadPackedFont(other.packed_font_);
  }

  Font& Font::operator=(const Font& other) {
    Font copy(other);
    std::swap(size_, copy.size_);
    std::swap(native_size_, copy.native_size_);
    std::swap(dpi_scale_, copy.dpi_scale_);
    std::swap(packed_font_, copy.packed_font_);
    return *this;
  }

  Font::~Font() {
    if (packed_font_)
      FontCache::returnPackedFont(packed_font_);
  }

  Font Font::withDpiScale(float dpi_scale) const {
    if (packed_font_ == nullptr)
      return { size_, nullptr, 0, dpi_scale };
    return { size_, packed_font_->data(), packed_font_->dataSize(), dpi_scale };
  }

  Font Font::withSize(float size) const {
    if (packed_font_ == nullptr)
      return { size, nullptr, 0, dpi_scale_ };
    return { size, packed_font_->data(), packed_font_->dataSize(), dpi_scale_ };
  }

  int Font::nativeWidthOverflowIndex(const char32_t* string, int string_length, float width,
                                     bool round, int character_override) const {
    float string_width = 0;
    for (int i = 0; i < string_length; ++i) {
      char32_t character = string[i];
      if (character_override)
        character = character_override;
      const PackedGlyph* packed_char = &kNullPackedGlyph;
      if (!isIgnored(character))
        packed_char = packed_font_->packedGlyph(character);

      float advance = packed_char->x_advance;
      float break_point = advance;
      if (round)
        break_point = advance * 0.5f;

      if (string_width + break_point > width)
        return i;

      string_width += advance;
    }

    return string_length;
  }

  float Font::nativeStringWidth(const char32_t* string, int length, int character_override) const {
    if (length <= 0)
      return 0.0f;

    if (character_override) {
      float advance = packed_font_->packedGlyph(character_override)->x_advance;
      return advance * length;
    }

#if VISAGE_HARFBUZZ
    if (shapingState())
      return packed_font_->shapedRun(string, length).width;
#endif

    float width = 0.0f;
    for (int i = 0; i < length; ++i) {
      if (!isNewLine(string[i]) && !isIgnored(string[i]))
        width += packed_font_->packedGlyph(string[i])->x_advance;
    }

    return width;
  }

  void Font::setVertexPositions(FontAtlasQuad* quads, const char32_t* text, int length, float x,
                                float y, float width, float height, Justification justification,
                                int character_override) const {
    if (length <= 0)
      return;

    float string_width = nativeStringWidth(text, length, character_override);
    float pen_x = x + (width - string_width) * 0.5f;
    float pen_y = y + static_cast<int>((height + nativeCapitalHeight()) * 0.5f);

    if (justification & kLeft)
      pen_x = x;
    else if (justification & kRight)
      pen_x = x + width - string_width;

    if (justification & kTop)
      pen_y = y + static_cast<int>((nativeCapitalHeight() + nativeLineHeight()) * 0.5f);
    else if (justification & kBottom)
      pen_y = y + static_cast<int>(height);

    for (int i = 0; i < length; ++i) {
      char32_t character = character_override ? character_override : text[i];
      const PackedGlyph* packed_glyph = packed_font_->packedGlyph(character);

      quads[i].packed_glyph = packed_glyph;
      quads[i].x = pen_x + packed_glyph->x_offset;
      quads[i].y = pen_y - packed_glyph->y_offset;
      quads[i].width = packed_glyph->width;
      quads[i].height = packed_glyph->height;

      pen_x += packed_glyph->x_advance;
    }
  }

  std::vector<int> Font::nativeLineBreaks(const char32_t* string, int length, float width) const {
    std::vector<int> line_breaks;
    int break_index = 0;
    while (break_index < length) {
      int overflow_index = nativeWidthOverflowIndex(string + break_index, length - break_index, width) +
                           break_index;
      if (overflow_index == length && !hasNewLine(string + break_index, overflow_index - break_index))
        break;

      int next_break_index = overflow_index;
      while (next_break_index < length && next_break_index > break_index &&
             isPrintable(string[next_break_index - 1])) {
        next_break_index--;
      }

      if (next_break_index == break_index)
        next_break_index = overflow_index;

      for (int i = break_index; i < next_break_index; ++i) {
        if (isNewLine(string[i]))
          next_break_index = i + 1;
      }

      next_break_index = std::max(next_break_index, break_index + 1);
      line_breaks.push_back(next_break_index);
      break_index = next_break_index;
    }

    return line_breaks;
  }

  void Font::layoutQuads(std::vector<FontAtlasQuad>& quads, const char32_t* text, int length, float x,
                         float y, float width, float height, Justification justification,
                         int character_override, bool multi_line) const {
#if VISAGE_HARFBUZZ
    if (shapingState() && !character_override) {
      quads.clear();
      if (!multi_line) {
        appendShapedLine(quads, text, length, x, y, width, height, justification);
        return;
      }

      // Lines break where the unshaped widths say, then each is shaped.
      int line_height = nativeLineHeight();
      std::vector<int> line_breaks = nativeLineBreaks(text, length, width);
      line_breaks.push_back(length);

      Justification line_justification = kTop;
      if (justification & kLeft)
        line_justification = kTopLeft;
      else if (justification & kRight)
        line_justification = kTopRight;

      int text_height = line_height * line_breaks.size();
      int line_y = y + 0.5 * (height - text_height);
      if (justification & kTop)
        line_y = y;
      else if (justification & kBottom)
        line_y = y + height - text_height;

      int last_break = 0;
      for (int line_break : line_breaks) {
        appendShapedLine(quads, text + last_break, line_break - last_break, x, line_y, width, height,
                         line_justification);
        last_break = line_break;
        line_y += line_height;
      }
      return;
    }
#endif

    quads.resize(std::max(0, length));
    if (multi_line)
      setMultiLineVertexPositions(quads.data(), text, length, x, y, width, height, justification);
    else
      setVertexPositions(quads.data(), text, length, x, y, width, height, justification, character_override);
  }

  void Font::appendShapedLine(std::vector<FontAtlasQuad>& quads, const char32_t* text, int length,
                              float x, float y, float width, float height,
                              Justification justification) const {
#if VISAGE_HARFBUZZ
    if (length <= 0)
      return;

    const PackedFont::ShapedRun& run = packed_font_->shapedRun(text, length);
    float pen_x = x + (width - run.width) * 0.5f;
    float pen_y = y + static_cast<int>((height + nativeCapitalHeight()) * 0.5f);

    if (justification & kLeft)
      pen_x = x;
    else if (justification & kRight)
      pen_x = x + width - run.width;

    if (justification & kTop)
      pen_y = y + static_cast<int>((nativeCapitalHeight() + nativeLineHeight()) * 0.5f);
    else if (justification & kBottom)
      pen_y = y + static_cast<int>(height);

    for (const PackedFont::ShapedGlyph& shaped : run.glyphs) {
      quads.push_back({ shaped.glyph, pen_x + shaped.x, pen_y + shaped.y,
                        static_cast<float>(shaped.glyph->width), static_cast<float>(shaped.glyph->height) });
    }
#endif
  }

  void Font::setMultiLineVertexPositions(FontAtlasQuad* quads, const char32_t* text, int length,
                                         float x, float y, float width, float height,
                                         Justification justification) const {
    int line_height = nativeLineHeight();
    std::vector<int> line_breaks = nativeLineBreaks(text, length, width);
    line_breaks.push_back(length);

    Justification line_justification = kTop;
    if (justification & kLeft)
      line_justification = kTopLeft;
    else if (justification & kRight)
      line_justification = kTopRight;

    int text_height = line_height * line_breaks.size();
    int line_y = y + 0.5 * (height - text_height);
    if (justification & kTop)
      line_y = y;
    else if (justification & kBottom)
      line_y = y + height - text_height;

    int last_break = 0;
    for (int line_break : line_breaks) {
      int line_length = line_break - last_break;
      setVertexPositions(quads + last_break, text + last_break, line_length, x, line_y, width,
                         height, line_justification);
      last_break = line_break;
      line_y += line_height;
    }
  }

  int Font::nativeLineHeight() const {
    return packed_font_->lineHeight();
  }

  float Font::nativeCapitalHeight() const {
    return packed_font_->packedGlyph('T')->y_offset;
  }

  float Font::nativeLowerDipHeight() const {
    const PackedGlyph* glyph = packed_font_->packedGlyph('y');
    return glyph->y_offset + glyph->height;
  }

  int Font::atlasWidth() const {
    return packed_font_->atlasWidth();
  }

  int Font::atlasHeight() const {
    return packed_font_->atlasHeight();
  }

  const bgfx::TextureHandle& Font::textureHandle() const {
    packed_font_->checkInit();
    return packed_font_->textureHandle();
  }

  FontCache::FontCache() {
    FreeTypeLibrary::instance();
  }

  FontCache::~FontCache() = default;

  PackedFont* FontCache::loadPackedFont(int size, const std::string& file_path) {
    std::string id = "file: " + file_path + " - " + std::to_string(size);
    if (instance()->cache_.count(cacheId(id)))
      return instance()->incrementPackedFont(cacheId(id));

    File file(file_path);
    size_t file_size = 0;
    std::unique_ptr<unsigned char[]> data = loadFileData(file, file_size);
    return instance()->createOrLoadPackedFont(id, size, data.get(), file_size);
  }

  PackedFont* FontCache::loadPackedFont(const PackedFont* packed_font) {
    if (packed_font == nullptr)
      return nullptr;
    return instance()->incrementPackedFont(packed_font->id());
  }

  PackedFont* FontCache::loadPackedFont(int size, const unsigned char* font_data, int data_size) {
    if (font_data == nullptr)
      return nullptr;
    std::string id = FreeTypeLibrary::idForFont(font_data, data_size) + " - " + std::to_string(size);
    return instance()->createOrLoadPackedFont(id, size, font_data, data_size);
  }

  PackedFont* FontCache::incrementPackedFont(const std::string& id) {
    ref_count_[cache_[id].get()]++;
    return cache_[id].get();
  }

  PackedFont* FontCache::createOrLoadPackedFont(const std::string& face_id, int size,
                                                const unsigned char* font_data, int data_size) {
    VISAGE_ASSERT(Thread::isMainThread());

    bool light = Font::rendering().hinting == TextRendering::Hinting::Light;
    std::string id = cacheId(face_id);
    if (cache_.count(id) == 0) {
      TypeFaceData type_face_data(font_data, data_size);
      if (type_face_data_lookup_.count(type_face_data) == 0) {
        auto saved_data = std::make_unique<unsigned char[]>(data_size);
        std::memcpy(saved_data.get(), font_data, data_size);
        type_face_data.data = saved_data.get();
        type_face_data_lookup_[type_face_data] = std::move(saved_data);
      }

      type_face_data.data = type_face_data_lookup_[type_face_data].get();
      type_face_data_ref_count_[type_face_data]++;
      cache_[id] = std::make_unique<PackedFont>(id, size, type_face_data.data, data_size,
                                                light ? FT_LOAD_TARGET_LIGHT : FT_LOAD_TARGET_NORMAL,
                                                fallbackChain());
    }

    return incrementPackedFont(id);
  }

  void FontCache::decrementPackedFont(PackedFont* packed_font) {
    VISAGE_ASSERT(Thread::isMainThread());
    ref_count_[packed_font]--;
    int count = ref_count_[packed_font];
    has_stale_fonts_ = has_stale_fonts_ || count == 0;
    VISAGE_ASSERT(ref_count_[packed_font] >= 0);
  }

  void FontCache::removeStaleFonts() {
    for (auto it = ref_count_.begin(); it != ref_count_.end();) {
      if (it->second)
        ++it;
      else {
        // The font goes before the file data its face reads from.
        TypeFaceData type_face_data(it->first->data(), it->first->dataSize());
        cache_.erase(it->first->id());
        it = ref_count_.erase(it);

        type_face_data_ref_count_[type_face_data]--;
        if (type_face_data_ref_count_[type_face_data] == 0) {
          type_face_data_ref_count_.erase(type_face_data);
          type_face_data_lookup_.erase(type_face_data);
        }
      }
    }
    has_stale_fonts_ = false;
  }
}