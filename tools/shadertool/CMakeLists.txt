# Offline shader compiler. Not part of a normal build: the compiled blobs are
# committed, so only someone editing a .glsl needs glslang and SDL_shadercross.
#
#   cmake -B build -DVISAGE_BUILD_SHADERTOOL=ON
#   cmake --build build --target shaders

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/CPM.cmake)

CPMAddPackage(
  NAME glslang
  GITHUB_REPOSITORY KhronosGroup/glslang
  GIT_TAG 16.3.0
  EXCLUDE_FROM_ALL YES
  SYSTEM YES
  OPTIONS
  "ENABLE_OPT OFF"
  "GLSLANG_TESTS OFF"
  "ENABLE_GLSLANG_BINARIES OFF"
  "ENABLE_HLSL OFF"
  "BUILD_SHARED_LIBS OFF"
)

# Pinned to a commit, not main: CI checks the committed blobs are reproducible,
# so a moving dependency would fail the build for reasons nobody changed.
# SDL_shadercross publishes no tags. Commit is from 2026-06-26.
CPMAddPackage(
  NAME SDL3_shadercross
  GITHUB_REPOSITORY libsdl-org/SDL_shadercross
  GIT_TAG e55cf5e31ced6f3d1be5cc6d0c50e99384f9f4ba
  EXCLUDE_FROM_ALL YES
  SYSTEM YES
  OPTIONS
  "SDLSHADERCROSS_CLI OFF"
  "SDLSHADERCROSS_DXC OFF"
  "SDLSHADERCROSS_VENDORED ON"
  "SDLSHADERCROSS_SHARED OFF"
  "SDLSHADERCROSS_STATIC ON"
  "SDLSHADERCROSS_INSTALL OFF"
  "BUILD_SHARED_LIBS OFF"
)

add_executable(shadertool main.cpp)
target_compile_features(shadertool PRIVATE cxx_std_17)
target_link_libraries(shadertool
  PRIVATE
  glslang::glslang
  glslang::SPIRV
  glslang::glslang-default-resource-limits
  SDL3_shadercross::SDL3_shadercross-static
  SDL3::SDL3-static
)
set_target_properties(shadertool PROPERTIES FOLDER "visage/tools")

# SDL_shadercross links SPIRV-Cross as a shared library regardless of
# BUILD_SHARED_LIBS, so the DLL has to sit next to the tool on Windows.
if (WIN32 AND TARGET spirv-cross-c-shared)
  add_custom_command(TARGET shadertool POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_FILE:spirv-cross-c-shared> $<TARGET_FILE_DIR:shadertool>
  )
endif ()

set(VISAGE_SHADER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../visage_graphics/shaders)
set(VISAGE_SHADER_OUT ${VISAGE_SHADER_SRC}/compiled)
set(EXAMPLE_SHADER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../examples/shaders)
set(EXAMPLE_SHADER_OUT ${EXAMPLE_SHADER_SRC}/compiled)

# .vert/.frag/.comp are compiled verbatim, for an application that drives
# SDL_GPU itself and owns its own descriptor layout.
file(GLOB SHADER_SOURCES CONFIGURE_DEPENDS
  ${VISAGE_SHADER_SRC}/*.glsl
  ${EXAMPLE_SHADER_SRC}/*.glsl
  ${EXAMPLE_SHADER_SRC}/*.vert
  ${EXAMPLE_SHADER_SRC}/*.frag
  ${EXAMPLE_SHADER_SRC}/*.comp
)

add_custom_target(shaders
  COMMAND ${CMAKE_COMMAND} -E make_directory ${VISAGE_SHADER_OUT}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${EXAMPLE_SHADER_OUT}
  COMMAND shadertool ${VISAGE_SHADER_SRC} ${VISAGE_SHADER_OUT} ${VISAGE_SHADER_SRC}/shader_utils.glsl
  COMMAND shadertool ${EXAMPLE_SHADER_SRC} ${EXAMPLE_SHADER_OUT} ${VISAGE_SHADER_SRC}/shader_utils.glsl
  DEPENDS shadertool ${SHADER_SOURCES}
  COMMENT "Compiling visage GLSL -> SPIR-V + MSL"
  VERBATIM
)
