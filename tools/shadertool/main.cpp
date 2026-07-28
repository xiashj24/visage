// shadertool - offline compiler turning visage's plain GLSL into the SPIR-V and
// MSL blobs the SDL_GPU backend consumes.
//
// Usage: shadertool <src_dir> <out_dir>
//
// The .glsl sources stay plain GLSL 330/300es so the OpenGL backend can keep
// compiling them verbatim at runtime. Everything Vulkan requires - explicit
// locations, descriptor sets, a std140 uniform block - is injected here.
//
// .vert / .frag / .comp are compiled with none of that: they are already
// Vulkan GLSL and declare their own sets and bindings. That is for an
// application driving SDL_GPU itself, which owns its descriptor layout and
// would only be fighting the injection.

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <SPIRV/GlslangToSpv.h>

#include <SDL3/SDL.h>
#include <SDL3_shadercross/SDL_shadercross.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

  // Vertex attribute locations are the bgfx Attrib enum values; the GL backend
  // binds the same slots by name, so the two backends stay in agreement.
  const std::vector<std::string> kAttribNames = { "a_position",  "a_color0",    "a_color1",
                                                  "a_color2",    "a_texcoord0", "a_texcoord1",
                                                  "a_texcoord2", "a_texcoord3" };

  // Varying locations must agree across every vertex/fragment pair that can be
  // linked - including a live-edited shader linking against a library stage -
  // so the table is fixed here rather than derived per directory. Adding a
  // varying to a shader means adding it here; the tool errors out otherwise.
  const std::vector<std::string> kVaryingNames = {
    "v_coordinates",   "v_dimensions",      "v_gradient_pos",  "v_gradient_pos2",
    "v_gradient_texture_pos", "v_position", "v_shader_values", "v_shader_values1",
    "v_texture_uv",
  };

  // SDL_GPU's SPIR-V binding model: vertex uses set 0 (textures) / 1 (uniforms),
  // fragment uses set 2 / 3.
  constexpr int kVertexUniformSet = 1;
  constexpr int kFragmentSamplerSet = 2;
  constexpr int kFragmentUniformSet = 3;

  constexpr uint32_t kBlobMagic = 0x53475356;  // "VSGS"
  constexpr uint32_t kBlobVersion = 3;

  enum class Stage : uint32_t { Vertex, Fragment, Compute };

  // Effect shaders are authored to Shadertoy's convention - the one with a
  // corpus large enough that a third party needs no visage documentation.
  // Library shaders write main() and declare their own varyings.
  //
  // Only iResolution, iTime and iChannel0 are provided. Anything else Shadertoy
  // defines (iMouse, iFrame, iTimeDelta, iChannel1-3) fails to compile with an
  // undeclared-identifier error, which is the honest answer: nothing populates
  // them. Declaring them would hand the shader garbage instead.
  constexpr const char* kEffectPreamble = R"GLSL(
in vec2 v_coordinates;
in vec2 v_dimensions;
in vec2 v_position;

out vec4 fragColor;

uniform vec4 u_time;
)GLSL";

  // iChannel0 is a 512x2 texture - spectrum on row 0, waveform on row 1 -
  // which is both Shadertoy's audio input layout and xlib's u_tex0 layout.
  constexpr const char* kAudioPreamble = R"GLSL(
uniform sampler2D s_texture;
#define iChannel0 s_texture
#define u_tex0 s_texture

float spectrum01(float x) { return texture(s_texture, vec2(x, 0.25)).x; }
float waveform(float x)   { return texture(s_texture, vec2(x, 0.75)).x; }
vec2 spectrum01LR(float x) { return texture(s_texture, vec2(x, 0.25)).xy; }
vec2 waveformLR(float x)   { return texture(s_texture, vec2(x, 0.75)).xy; }
)GLSL";

  // Shadertoy names alias the fixed vec4 block rather than joining it, so the
  // block stays vec4-only and the blob's "offset = index * 16" manifest holds.
  constexpr const char* kShadertoyPreamble = R"GLSL(
#define iResolution vec3(v_dimensions, 1.0)
#define iTime (u_time.x)
)GLSL";

  // fragCoord is y-up with a bottom-left origin, as Shadertoy defines it;
  // v_coordinates is -1..1 with y down.
  constexpr const char* kShadertoyEpilogue = R"GLSL(

void main() {
  vec2 fragCoord = vec2(v_coordinates.x * 0.5 + 0.5, 0.5 - v_coordinates.y * 0.5) * v_dimensions;
  mainImage(fragColor, fragCoord);
}
)GLSL";

  struct Shader {
    Stage stage = Stage::Fragment;
    std::string body;                    // source with declarations stripped
    std::vector<std::string> uniforms;   // std140 block order
    std::vector<std::string> samplers;   // binding order

    bool isFragment() const { return stage == Stage::Fragment; }
  };

  // What SDL_GPUComputePipelineCreateInfo needs, all reflected rather than
  // declared - including the workgroup size, which the dispatch has to match.
  struct ComputeInfo {
    uint32_t num_samplers = 0;
    uint32_t num_readonly_storage_textures = 0;
    uint32_t num_readonly_storage_buffers = 0;
    uint32_t num_readwrite_storage_textures = 0;
    uint32_t num_readwrite_storage_buffers = 0;
    uint32_t num_uniform_buffers = 0;
    uint32_t threadcount[3] = { 1, 1, 1 };
  };

  std::string readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream stream;
    stream << in.rdbuf();
    return stream.str();
  }

  int indexOf(const std::vector<std::string>& table, const std::string& name) {
    auto it = std::find(table.begin(), table.end(), name);
    return it == table.end() ? -1 : static_cast<int>(it - table.begin());
  }

  std::string trimmed(const std::string& line) {
    size_t begin = line.find_first_not_of(" \t\r");
    if (begin == std::string::npos)
      return {};
    size_t end = line.find_last_not_of(" \t\r");
    return line.substr(begin, end - begin + 1);
  }

  // Matches "<qualifier> <type> <name>;" and pulls out type and name.
  bool matchDeclaration(const std::string& line, const std::string& qualifier,
                        std::string& type, std::string& name) {
    std::istringstream stream(line);
    std::string first;
    if (!(stream >> first) || first != qualifier)
      return false;
    if (!(stream >> type) || !(stream >> name))
      return false;
    if (name.empty() || name.back() != ';')
      return false;
    name.pop_back();
    std::string extra;
    return !(stream >> extra);
  }

  // Rewrites one source line, collecting uniforms/samplers as it goes. Returns
  // the replacement text, or the line unchanged when it is not a declaration.
  bool rewriteLine(const std::string& raw, Shader& shader, std::string& out, std::string& error) {
    const std::string line = trimmed(raw);
    std::string type, name;

    if (matchDeclaration(line, "uniform", type, name)) {
      if (type == "sampler2D") {
        if (!shader.isFragment()) {
          error = "sampler in a vertex shader is not supported: " + name;
          return false;
        }
        int binding = static_cast<int>(shader.samplers.size());
        shader.samplers.push_back(name);
        out = "layout(set = " + std::to_string(kFragmentSamplerSet) + ", binding = " +
              std::to_string(binding) + ") uniform sampler2D " + name + ";";
        return true;
      }
      // Scalars/vectors move into the std140 block emitted in the preamble.
      shader.uniforms.push_back(name);
      out = "";
      return true;
    }

    if (matchDeclaration(line, "in", type, name)) {
      const bool is_attribute = name.rfind("a_", 0) == 0;
      const std::vector<std::string>& table = is_attribute ? kAttribNames : kVaryingNames;
      int location = indexOf(table, name);
      if (location < 0) {
        error = "unknown " + std::string(is_attribute ? "attribute" : "varying") + " '" + name +
                "' - add it to " + (is_attribute ? "kAttribNames" : "kVaryingNames");
        return false;
      }
      out = "layout(location = " + std::to_string(location) + ") in " + type + " " + name + ";";
      return true;
    }

    if (matchDeclaration(line, "out", type, name)) {
      if (name.rfind("v_", 0) == 0) {
        int location = indexOf(kVaryingNames, name);
        if (location < 0) {
          error = "unknown varying '" + name + "' - add it to kVaryingNames";
          return false;
        }
        out = "layout(location = " + std::to_string(location) + ") out " + type + " " + name + ";";
        return true;
      }
      // The lone non-varying output of a fragment shader is the colour target.
      out = "layout(location = 0) out " + type + " " + name + ";";
      return true;
    }

    out = raw;
    return true;
  }

  bool transform(const std::string& source, Shader& shader, std::string& error) {
    std::istringstream stream(source);
    std::string line;
    std::ostringstream body;
    while (std::getline(stream, line)) {
      std::string rewritten;
      if (!rewriteLine(line, shader, rewritten, error))
        return false;
      body << rewritten << "\n";
    }
    shader.body = body.str();
    return true;
  }

  std::string uniformBlock(const Shader& shader) {
    if (shader.uniforms.empty())
      return {};

    int set = shader.isFragment() ? kFragmentUniformSet : kVertexUniformSet;
    std::ostringstream block;
    block << "layout(set = " << set << ", binding = 0, std140) uniform VisageUniforms {\n";
    for (const std::string& name : shader.uniforms)
      block << "  vec4 " << name << ";\n";
    block << "};\n";
    return block.str();
  }

  // mainImage is unambiguous: no library shader defines it, so a single
  // substring check replaces the heuristics an author could trip over.
  bool isEffectShader(const std::string& source) {
    return source.find("mainImage") != std::string::npos;
  }

  // Resource ceilings the shader is checked against. glslang's defaults are
  // desktop-permissive, so a third-party shader that overruns what v3d offers
  // compiles clean here and only fails on the Pi.
  //
  // TODO(phase-0): replace the marked fields with the real v3d maxima. Capture
  // them on the Pi with vulkaninfo, or SDL_GetGPUDeviceProperties once the
  // SDL_GPU backend runs there, then drop VISAGE_SHADER_TARGET_DESKTOP.
  //
  // This bounds resource counts only - it says nothing about instruction-store
  // size or how fast the shader runs, which stay on-device measurements.
  TBuiltInResource targetLimits() {
    TBuiltInResource limits = *GetDefaultResources();
#ifndef VISAGE_SHADER_TARGET_DESKTOP
    // limits.maxFragmentUniformVectors = <v3d>;
    // limits.maxVaryingVectors = <v3d>;
    // limits.maxTextureImageUnits = <v3d>;
    // limits.maxCombinedTextureImageUnits = <v3d>;
    // limits.maxDrawBuffers = <v3d>;
#endif
    return limits;
  }

  bool compileToSpirv(EShLanguage stage, const std::string& source, std::vector<uint32_t>& spirv,
                      std::string& log) {
    glslang::TShader shader(stage);
    const char* strings[] = { source.c_str() };
    shader.setStrings(strings, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);

    const TBuiltInResource limits = targetLimits();
    if (!shader.parse(&limits, 100, false, EShMsgDefault)) {
      log = std::string(shader.getInfoLog()) + "\n" + shader.getInfoDebugLog();
      return false;
    }

    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(EShMsgDefault)) {
      log = std::string(program.getInfoLog()) + "\n" + program.getInfoDebugLog();
      return false;
    }

    glslang::GlslangToSpv(*program.getIntermediate(stage), spirv);
    return true;
  }

  bool transpileToMsl(const std::vector<uint32_t>& spirv, Stage stage, std::string& msl) {
    SDL_ShaderCross_SPIRV_Info info;
    SDL_zero(info);
    info.bytecode = reinterpret_cast<const Uint8*>(spirv.data());
    info.bytecode_size = spirv.size() * sizeof(uint32_t);
    info.entrypoint = "main";
    info.shader_stage = stage == Stage::Compute  ? SDL_SHADERCROSS_SHADERSTAGE_COMPUTE :
                        stage == Stage::Fragment ? SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT :
                                                   SDL_SHADERCROSS_SHADERSTAGE_VERTEX;

    void* out = SDL_ShaderCross_TranspileMSLFromSPIRV(&info);
    if (out == nullptr)
      return false;
    msl = static_cast<const char*>(out);
    SDL_free(out);
    return true;
  }

  void appendUint(std::string& blob, uint32_t value) {
    blob.append(reinterpret_cast<const char*>(&value), sizeof(value));
  }

  std::string packNames(const std::vector<std::string>& names) {
    std::string packed;
    for (const std::string& name : names) {
      packed.push_back(static_cast<char>(name.size()));
      packed.append(name);
    }
    return packed;
  }

  // One blob per shader keeps the embedded symbol names (vs_color, fs_color)
  // that ShaderCache and ProgramCache already key on.
  //
  // Sampler names ride along because binding index alone is ambiguous: slot 0
  // is the gradient atlas for a library shader quad and the audio texture for a
  // third-party effect. The backend binds by name, not by position.
  std::string packBlob(const Shader& shader, const std::vector<uint32_t>& spirv,
                       const std::string& msl, uint32_t num_samplers, uint32_t num_uniform_buffers,
                       const ComputeInfo& compute) {
    const std::string uniform_names = packNames(shader.uniforms);
    const std::string sampler_names = packNames(shader.samplers);

    const uint32_t spirv_bytes = static_cast<uint32_t>(spirv.size() * sizeof(uint32_t));
    std::string blob;
    appendUint(blob, kBlobMagic);
    appendUint(blob, kBlobVersion);
    appendUint(blob, static_cast<uint32_t>(shader.stage));
    appendUint(blob, num_samplers);
    appendUint(blob, num_uniform_buffers);
    appendUint(blob, static_cast<uint32_t>(shader.uniforms.size()));
    appendUint(blob, static_cast<uint32_t>(uniform_names.size()));
    appendUint(blob, static_cast<uint32_t>(shader.samplers.size()));
    appendUint(blob, static_cast<uint32_t>(sampler_names.size()));
    appendUint(blob, spirv_bytes);
    appendUint(blob, static_cast<uint32_t>(msl.size()));
    // Zero for a graphics stage, so the header is one fixed size.
    appendUint(blob, compute.num_readonly_storage_textures);
    appendUint(blob, compute.num_readonly_storage_buffers);
    appendUint(blob, compute.num_readwrite_storage_textures);
    appendUint(blob, compute.num_readwrite_storage_buffers);
    appendUint(blob, compute.threadcount[0]);
    appendUint(blob, compute.threadcount[1]);
    appendUint(blob, compute.threadcount[2]);
    blob.append(uniform_names);
    blob.append(sampler_names);
    while (blob.size() % 4 != 0)  // SPIR-V must stay 4-byte aligned for the loader.
      blob.push_back('\0');
    blob.append(reinterpret_cast<const char*>(spirv.data()), spirv_bytes);
    blob.append(msl);
    return blob;
  }

  bool writeFile(const fs::path& path, const std::string& data) {
    std::ofstream out(path, std::ios::binary);
    if (!out)
      return false;
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    return out.good();
  }

  bool compileOne(const fs::path& input, const fs::path& out_dir, const std::string& utils) {
    const std::string stem = input.stem().string();
    if (stem == "shader_utils")
      return true;  // Injected into every shader, never compiled alone.

    const std::string extension = input.extension().string();
    const bool verbatim = extension != ".glsl";

    Shader shader;
    if (extension == ".comp")
      shader.stage = Stage::Compute;
    else if (extension == ".vert")
      shader.stage = Stage::Vertex;
    else if (extension == ".frag")
      shader.stage = Stage::Fragment;
    else {
      // A vs_ prefix is the only way to get a vertex shader; third-party
      // effects are fragment-only and need no prefix.
      shader.stage = stem.rfind("vs_", 0) == 0 ? Stage::Vertex : Stage::Fragment;
    }

    const std::string body = readFile(input);
    const bool is_effect = !verbatim && shader.isFragment() && isEffectShader(body);

    std::string source;
    if (verbatim)
      source = body;
    else {
      std::string preamble, epilogue;
      if (is_effect) {
        preamble = kEffectPreamble;
        // Only declare the audio sampler when the shader reaches for it - an
        // unbound sampler is a validation error.
        if (body.find("iChannel0") != std::string::npos ||
            body.find("spectrum") != std::string::npos ||
            body.find("waveform") != std::string::npos)
          preamble += kAudioPreamble;
        preamble += kShadertoyPreamble;
        epilogue = kShadertoyEpilogue;
      }

      // #line 1 puts the body back at line 1 so compile errors match what a
      // live shader editor shows.
      std::string error;
      if (!transform(utils + preamble + "\n#line 1\n" + body + epilogue, shader, error)) {
        std::fprintf(stderr, "[shadertool] %s: %s\n", stem.c_str(), error.c_str());
        return false;
      }
      source = "#version 450\n" + uniformBlock(shader) + shader.body;
    }

    const EShLanguage language = shader.stage == Stage::Compute  ? EShLangCompute :
                                 shader.stage == Stage::Fragment ? EShLangFragment :
                                                                   EShLangVertex;
    std::vector<uint32_t> spirv;
    std::string log;
    if (!compileToSpirv(language, source, spirv, log)) {
      std::fprintf(stderr, "[shadertool] %s: GLSL->SPIR-V failed:\n%s\n", stem.c_str(), log.c_str());
      return false;
    }

    // Reflect rather than assume: glslang drops a uniform block nothing reads,
    // and a dispatch has to match the workgroup size the source declared.
    uint32_t num_samplers = 0;
    uint32_t num_uniform_buffers = 0;
    ComputeInfo compute;
    if (shader.stage == Stage::Compute) {
      SDL_ShaderCross_ComputePipelineMetadata* metadata =
          SDL_ShaderCross_ReflectComputeSPIRV(reinterpret_cast<const Uint8*>(spirv.data()),
                                              spirv.size() * sizeof(uint32_t), 0);
      if (metadata == nullptr) {
        std::fprintf(stderr, "[shadertool] %s: reflection failed: %s\n", stem.c_str(), SDL_GetError());
        return false;
      }
      num_samplers = metadata->num_samplers;
      num_uniform_buffers = metadata->num_uniform_buffers;
      compute.num_readonly_storage_textures = metadata->num_readonly_storage_textures;
      compute.num_readonly_storage_buffers = metadata->num_readonly_storage_buffers;
      compute.num_readwrite_storage_textures = metadata->num_readwrite_storage_textures;
      compute.num_readwrite_storage_buffers = metadata->num_readwrite_storage_buffers;
      compute.threadcount[0] = metadata->threadcount_x;
      compute.threadcount[1] = metadata->threadcount_y;
      compute.threadcount[2] = metadata->threadcount_z;
      SDL_free(metadata);
    }
    else {
      SDL_ShaderCross_GraphicsShaderMetadata* metadata =
          SDL_ShaderCross_ReflectGraphicsSPIRV(reinterpret_cast<const Uint8*>(spirv.data()),
                                               spirv.size() * sizeof(uint32_t), 0);
      if (metadata == nullptr) {
        std::fprintf(stderr, "[shadertool] %s: reflection failed: %s\n", stem.c_str(), SDL_GetError());
        return false;
      }
      num_samplers = metadata->resource_info.num_samplers;
      num_uniform_buffers = metadata->resource_info.num_uniform_buffers;
      SDL_free(metadata);
    }

    // A declared-but-unused sampler gets dropped, leaving the survivors on
    // non-contiguous bindings that SDL_GPU cannot express. Fail loudly here
    // rather than mis-bind at runtime. Verbatim sources declare their own
    // bindings, so there is nothing to cross-check.
    if (!verbatim && num_samplers != shader.samplers.size()) {
      std::fprintf(stderr,
                   "[shadertool] %s: declares %zu sampler(s) but %u survive - remove the unused "
                   "one, bindings must stay contiguous from 0\n",
                   stem.c_str(), shader.samplers.size(), num_samplers);
      return false;
    }

    std::string msl;
    if (!transpileToMsl(spirv, shader.stage, msl)) {
      std::fprintf(stderr, "[shadertool] %s: SPIR-V->MSL failed: %s\n", stem.c_str(), SDL_GetError());
      return false;
    }

    if (!writeFile(out_dir / stem,
                   packBlob(shader, spirv, msl, num_samplers, num_uniform_buffers, compute))) {
      std::fprintf(stderr, "[shadertool] %s: failed to write blob\n", stem.c_str());
      return false;
    }

    const char* kind = verbatim ? (shader.stage == Stage::Compute ? "compute" : "verbatim") :
                                  (is_effect ? "effect" : "library");
    std::printf("[shadertool] %-28s %-9s %u uniform(s), %u sampler(s), %zu B SPIR-V, %zu B MSL\n",
                stem.c_str(), kind, static_cast<unsigned>(shader.uniforms.size()), num_samplers,
                spirv.size() * sizeof(uint32_t), msl.size());
    return true;
  }

}  // namespace

int main(int argc, char** argv) {
  if (argc != 4) {
    std::fprintf(stderr, "usage: %s <src_dir> <out_dir> <shader_utils.glsl>\n", argv[0]);
    return 2;
  }
  const fs::path src_dir = argv[1];
  const fs::path out_dir = argv[2];
  const fs::path utils_path = argv[3];

  if (!SDL_ShaderCross_Init()) {
    std::fprintf(stderr, "[shadertool] SDL_ShaderCross_Init failed: %s\n", SDL_GetError());
    return 1;
  }
  glslang::InitializeProcess();

  // Example shaders link against the same helpers as library shaders, so the
  // prelude is passed in rather than found next to the sources.
  const std::string utils = readFile(utils_path);

  std::vector<fs::path> inputs;
  for (const auto& entry : fs::directory_iterator(src_dir)) {
    const std::string extension = entry.path().extension().string();
    if (entry.is_regular_file() &&
        (extension == ".glsl" || extension == ".vert" || extension == ".frag" || extension == ".comp"))
      inputs.push_back(entry.path());
  }
  std::sort(inputs.begin(), inputs.end());  // Deterministic output ordering.

  bool ok = true;
  for (const fs::path& input : inputs)
    ok = compileOne(input, out_dir, utils) && ok;

  glslang::FinalizeProcess();
  SDL_ShaderCross_Quit();
  return ok ? 0 : 1;
}
