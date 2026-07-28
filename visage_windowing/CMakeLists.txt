# SDL3 itself is fetched from the top level, because the gpu backend in
# visage_graphics needs it too and that subdirectory is configured first.

file(GLOB PLATFORM_SOURCE_FILES sdl3/*.cpp)
file(GLOB PLATFORM_HEADER_FILES sdl3/*.h)

file(GLOB HEADERS *.h)
file(GLOB SOURCE_FILES *.cpp)

amalgamate_headers(${VISAGE_INCLUDE}/visage/windowing.h "${HEADERS}")
list(APPEND HEADERS ${VISAGE_INCLUDE}/visage/windowing.h)

add_library(VisageWindowing OBJECT ${SOURCE_FILES} ${PLATFORM_SOURCE_FILES} ${HEADERS} ${PLATFORM_HEADER_FILES})
target_include_directories(VisageWindowing
  PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${VISAGE_INCLUDE_PATH}
)
target_link_libraries(VisageWindowing PUBLIC SDL3::SDL3-static)

set_target_properties(VisageWindowing PROPERTIES FOLDER "visage")
