# The gl/ backend headers are internal - kept out of the amalgamated public
# visage/graphics.h.
file(GLOB HEADERS *.h)
file(GLOB SOURCE_FILES *.cpp)
list(APPEND SOURCE_FILES gl/gl_api.cpp gl/bgfx_gl.cpp)
set(GL_BACKEND_HEADERS gl/gl_api.h gl/include/bgfx/bgfx.h)

if (WIN32)
  list(APPEND SOURCE_FILES win32/emoji_win32.cpp)
  set(WIN_LIBS d2d1 dwrite Windowscodecs)
elseif (APPLE)
  list(APPEND SOURCE_FILES macos/emoji_macos.cpp)
elseif (UNIX)
  list(APPEND SOURCE_FILES linux/emoji_linux.cpp)
endif ()

if (VISAGE_AMALGAMATED_BUILD)
  amalgamate_sources(AMALGAMATED ${CMAKE_CURRENT_BINARY_DIR}/visage_graphics_amalgamated "${SOURCE_FILES}")
  list(APPEND SOURCE_FILES ${AMALGAMATED})
endif ()

amalgamate_headers(${VISAGE_INCLUDE}/visage/graphics.h "${HEADERS}")
list(APPEND HEADERS ${VISAGE_INCLUDE}/visage/graphics.h)

add_library(VisageGraphics OBJECT ${SOURCE_FILES} ${PLATFORM_FILES} ${HEADERS} ${GL_BACKEND_HEADERS})

if (UNIX AND NOT APPLE)
  add_definitions(-D_LARGEFILE64_SOURCE)
endif ()

set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)

if (VISAGE_ENABLE_GRAPHICS_DEBUG_LOGGING)
  target_compile_definitions(VisageGraphics PUBLIC VISAGE_GRAPHICS_DEBUG_LOGGING=1)
endif ()

message(STATUS "VISAGE: Downloading graphics dependencies")
set(CMAKE_MESSAGE_LOG_LEVEL "ERROR")
FetchContent_Declare(freetype GIT_REPOSITORY https://gitlab.freedesktop.org/freetype/freetype.git
  GIT_SHALLOW TRUE
  GIT_TAG VER-2-14-1
  EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(freetype)
set(CMAKE_MESSAGE_LOG_LEVEL "NOTICE")

if (MSVC)
  set(DISABLE_WARNINGS /W0)
else ()
  set(DISABLE_WARNINGS -w)
endif ()

target_compile_options(freetype PRIVATE ${DISABLE_WARNINGS})

target_link_libraries(
  VisageGraphics
  PRIVATE
  freetype
  ${WIN_LIBS}
  ${FONTCONFIG_LIBRARIES}
)

include(embedded.cmake)

target_include_directories(VisageGraphics
  PRIVATE
  ${VISAGE_INCLUDE_PATH}
  ${freetype_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/gl/include
)

if(WIN32)
  target_compile_definitions(VisageGraphics PRIVATE
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
    NTDDI_VERSION=0x0A000000
  )
endif()

target_include_directories(VisageGraphics
  SYSTEM PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/
)

set_target_properties(VisageGraphics PROPERTIES FOLDER "visage")

visage_add_test_target(
  TARGET VisageGraphicsTests
  TEST_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/tests
)
set_target_properties(freetype PROPERTIES FOLDER "visage/third_party")

if (APPLE)
  target_compile_options(VisageGraphics PRIVATE -fobjc-arc)
endif ()
