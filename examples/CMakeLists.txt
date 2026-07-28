file(GLOB EXAMPLES RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} */)

# Whichever form the selected backend consumes, matching visage_graphics.
# Either way thirdparty/ stays out: it is Shadertoy source that only means
# anything after the offline tool injects its preamble, so handing it to the
# GL backend, which compiles this text verbatim at runtime, cannot work.
if (VISAGE_SDL_GPU)
  file(GLOB EXAMPLE_SHADERS shaders/compiled/*)
  if (NOT EXAMPLE_SHADERS)
    message(FATAL_ERROR
      "VISAGE_SDL_GPU needs compiled shaders in examples/shaders/compiled. "
      "Build them with -DVISAGE_BUILD_SHADERTOOL=ON --target shaders.")
  endif ()
else ()
  file(GLOB EXAMPLE_SHADERS shaders/*.glsl)
endif ()
add_compile_definitions(SHADERS_FOLDER=\"${CMAKE_CURRENT_SOURCE_DIR}/shaders\")

visage_embed_shaders(EmbeddedShaderResources "example_shaders.h" "resources::shaders" "${EXAMPLE_SHADERS}")
file(GLOB_RECURSE FONT_TTF_FILES fonts/*.ttf)
file(GLOB_RECURSE ICON_FILES icons/*.svg)
file(GLOB_RECURSE IMAGE_FILES images/*.png)

add_embedded_resources(EmbeddedFontResources "example_fonts.h" "resources::fonts" "${FONT_TTF_FILES}")
add_embedded_resources(EmbeddedIconResources "example_icons.h" "resources::icons" "${ICON_FILES}")
add_embedded_resources(EmbeddedImageResources "example_images.h" "resources::images" "${IMAGE_FILES}")

# ClapPlugin is deliberately not built: CLAP/VST-style plugin-GUI hosting has
# no use case in this fork and the SDL3 backend has no plugin-window path.
# Benchmark is built below instead: it needs a console and its own main().
set(SKIPPED_EXAMPLES ClapPlugin Benchmark)

if (VISAGE_SDL_GPU)
  # LiveShaderEditing recompiles GLSL at runtime, which only the gl backend
  # can do. AudioVisualizer drives GL compute directly; it is ported in phase 4.
  list(APPEND SKIPPED_EXAMPLES LiveShaderEditing AudioVisualizer)
endif ()

foreach (EXAMPLE ${EXAMPLES})
  file(GLOB SOURCE_FILES ${EXAMPLE}/*.cpp)
  file(GLOB HEADER_FILES ${EXAMPLE}/*.h)

  if (IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/${EXAMPLE}" AND SOURCE_FILES AND NOT EXAMPLE IN_LIST SKIPPED_EXAMPLES)
    add_executable(Example${EXAMPLE} main.cpp ${SOURCE_FILES} ${HEADER_FILES})

    if (WIN32)
      set_target_properties(Example${EXAMPLE} PROPERTIES WIN32_EXECUTABLE YES)
    elseif (APPLE)
      set(EXAMPLE_NAME Example${EXAMPLE})
      configure_file(Info.plist.in ${CMAKE_CURRENT_BINARY_DIR}/${EXAMPLE}/ExampleInfo.plist)
      set_target_properties(Example${EXAMPLE} PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_BINARY_DIR}/${EXAMPLE}/ExampleInfo.plist
      )
    endif ()

    target_link_libraries(Example${EXAMPLE} PRIVATE
      visage
      EmbeddedShaderResources
      EmbeddedFontResources
      EmbeddedIconResources
      EmbeddedImageResources
    )

    # AudioVisualizer drives its own GL and audio, so it needs SDL3 headers
    # directly rather than through visage.
    if (EXAMPLE STREQUAL "AudioVisualizer")
      target_link_libraries(Example${EXAMPLE} PRIVATE SDL3::SDL3-static)
    endif ()
  endif ()
endforeach ()

# Console, not WIN32_EXECUTABLE: it reports to stdout and is driven over SSH on
# the Pi. Lives here rather than in tests/ because it reuses the example fonts.
add_executable(VisageBenchmark Benchmark/benchmark.cpp)
target_link_libraries(VisageBenchmark PRIVATE visage EmbeddedFontResources)
set_target_properties(VisageBenchmark PROPERTIES FOLDER "visage/examples")
