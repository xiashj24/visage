# SDL3 provides windowing on every platform (Win32/Cocoa/KMSDRM). Pinned to a
# GitHub release source archive - not a git tag - so the version stamps as a
# real release instead of "development-version".
set(VISAGE_SDL3_VERSION "3.4.12" CACHE STRING "SDL3 release to fetch")

include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/CPM.cmake)

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)

message(STATUS "VISAGE: Downloading SDL3 ${VISAGE_SDL3_VERSION}")
CPMAddPackage(
  NAME SDL3
  URL https://github.com/libsdl-org/SDL/releases/download/release-${VISAGE_SDL3_VERSION}/SDL3-${VISAGE_SDL3_VERSION}.tar.gz
  EXCLUDE_FROM_ALL YES
)

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
