file(GLOB EXAMPLES RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} */)

file(GLOB_RECURSE EXAMPLE_SHADERS shaders/*.glsl)
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
foreach (EXAMPLE ${EXAMPLES})
  file(GLOB SOURCE_FILES ${EXAMPLE}/*.cpp)
  file(GLOB HEADER_FILES ${EXAMPLE}/*.h)

  if (IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/${EXAMPLE}" AND SOURCE_FILES AND NOT EXAMPLE STREQUAL "ClapPlugin")
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
  endif ()
endforeach ()
