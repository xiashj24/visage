
# Shaders are plain GLSL text embedded as-is - no offline compiler. Each
# .glsl file is staged to an extension-less copy so the embedded symbol name
# matches the shader name (vs_color, fs_color, shader_utils, ...).
function(visage_embed_shaders project include_filename namespace original_shaders)
  set(GLSL_FOLDER ${CMAKE_CURRENT_BINARY_DIR}/shaders/glsl)
  file(MAKE_DIRECTORY ${GLSL_FOLDER})

  set(SHADER_FILES)
  foreach (SHADER ${original_shaders})
    get_filename_component(SHADER_NAME ${SHADER} NAME_WE)
    set(SHADER_COPY ${GLSL_FOLDER}/${SHADER_NAME})
    add_custom_command(
      OUTPUT ${SHADER_COPY}
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SHADER} ${SHADER_COPY}
      DEPENDS ${SHADER}
    )
    list(APPEND SHADER_FILES ${SHADER_COPY})
  endforeach ()

  add_embedded_resources(${project} ${include_filename} ${namespace} "${SHADER_FILES}")
  target_sources(${project} PRIVATE ${original_shaders})
  source_group("Shaders" FILES ${original_shaders})
endfunction()

file(GLOB LIBRARY_SHADERS shaders/*.glsl)
file(GLOB FONT_TTF_FILES fonts/*.ttf)
file(GLOB ICON_FILES icons/*.svg)

if (UNIX AND NOT APPLE)
  file(GLOB LINUX_TTF_FILES fonts/linux/*.ttf)
  list(APPEND FONT_TTF_FILES ${LINUX_TTF_FILES})
endif ()

visage_embed_shaders(VisageEmbeddedShaders "shaders.h" "visage::shaders" "${LIBRARY_SHADERS}")
add_embedded_resources(VisageEmbeddedFonts "fonts.h" "visage::fonts" "${FONT_TTF_FILES}")
add_embedded_resources(VisageEmbeddedIcons "icons.h" "visage::icons" "${ICON_FILES}")

add_library(VisageGraphicsEmbeds INTERFACE)

target_link_libraries(VisageGraphicsEmbeds
  INTERFACE
  VisageEmbeddedShaders
  VisageEmbeddedFonts
  VisageEmbeddedIcons
)
target_link_libraries(VisageGraphics PRIVATE VisageGraphicsEmbeds)

set_target_properties(VisageEmbeddedShaders PROPERTIES FOLDER "visage")
set_target_properties(VisageEmbeddedFonts PROPERTIES FOLDER "visage")
set_target_properties(VisageEmbeddedIcons PROPERTIES FOLDER "visage")
