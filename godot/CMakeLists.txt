cmake_minimum_required(VERSION 3.24)
# Godot 4 GDExtension for the DUSTFALL spike. Compiles the (SDL-free) sim straight in and links
# godot-cpp. Configure with the repo's MinGW toolchain for the Windows .dll, or natively for the
# Linux .so used by the headless determinism test.
#   Linux:   cmake -S godot -B godot/build-ext && cmake --build godot/build-ext -j
#   Windows: cmake -S godot -B godot/build-ext-win --toolchain cmake/toolchain-mingw.cmake && cmake --build ...
project(dustfall_gdext CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(godot-cpp)

# The sim: every game/ + engine/ TU (NONE include SDL — SDL lives only in platform/app.cpp).
file(GLOB SIM_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/game/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/engine/*.cpp)

add_library(dustfall SHARED
  src/dustfall_sim.cpp
  src/register_types.cpp
  ${SIM_SOURCES})

target_include_directories(dustfall PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../src
  ${CMAKE_CURRENT_SOURCE_DIR}/../third_party)

target_link_libraries(dustfall PRIVATE godot-cpp)

if(MINGW)
  # Statically fold in libgcc/libstdc++/winpthread so the .dll loads on a Windows box with no
  # MinGW runtime present (matches the SDL build's zero-DLL policy).
  target_link_options(dustfall PRIVATE -static -static-libgcc -static-libstdc++)
endif()

# -> godot/bin/libdustfall.so (Linux) or dustfall.dll (Windows), per dustfall.gdextension.
set_target_properties(dustfall PROPERTIES
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/bin
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/bin
  PREFIX "lib" OUTPUT_NAME "dustfall")
