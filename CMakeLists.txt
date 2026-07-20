cmake_minimum_required(VERSION 3.24)
project(dustfall C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---- SDL2 (static) ----------------------------------------------------------
include(FetchContent)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
set(SDL2_DISABLE_UNINSTALL ON CACHE BOOL "" FORCE)
FetchContent_Declare(SDL2
  URL https://github.com/libsdl-org/SDL/releases/download/release-2.30.11/SDL2-2.30.11.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(SDL2)

# ---- Game -------------------------------------------------------------------
add_executable(dustfall
  src/platform/app.cpp
  src/engine/palette.cpp
  src/engine/assets.cpp
  src/engine/render.cpp
  src/engine/iso.cpp
  src/engine/audio.cpp
  src/engine/wav.cpp
  src/engine/save.cpp
  src/engine/stb_image_impl.cpp
  src/game/game.cpp
  src/game/map.cpp
  src/game/ui.cpp
)

target_include_directories(dustfall PRIVATE src third_party)
target_compile_options(dustfall PRIVATE
  -Wall -Wextra
  $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>)
set_source_files_properties(src/engine/stb_image_impl.cpp
  PROPERTIES COMPILE_OPTIONS "-Wno-unused-function")

target_link_libraries(dustfall PRIVATE SDL2::SDL2main SDL2::SDL2-static)

if(MINGW)
  # Fully static: no libgcc/libstdc++/winpthread DLL dependencies.
  target_link_options(dustfall PRIVATE -static -static-libgcc -static-libstdc++)
endif()

# Release builds are GUI-subsystem (no console window); Debug keeps the console
# for stdout logging.
set_target_properties(dustfall PROPERTIES WIN32_EXECUTABLE $<CONFIG:Release>)

# ---- Data + size report -----------------------------------------------------
add_custom_command(TARGET dustfall POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_SOURCE_DIR}/assets $<TARGET_FILE_DIR:dustfall>/data
  COMMAND bash ${CMAKE_SOURCE_DIR}/scripts/size_report.sh
          $<TARGET_FILE:dustfall> $<TARGET_FILE_DIR:dustfall>/data
  COMMENT "Deploying data/ and running size report"
)
