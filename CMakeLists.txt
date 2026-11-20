cmake_minimum_required(VERSION 3.20)
project(tower_of_heaven C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(SDL3 REQUIRED CONFIG)
find_package(ZLIB REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# The engine contains nothing from the game: its code and assets are read from toh.pak, made from
# your own copy of the game (tools/extract.sh, then the pak target below).
set(PROJECT_DIR ${CMAKE_SOURCE_DIR}/extracted/project CACHE PATH "Extracted (or instrumented) game project")
set(GEN_DIR ${CMAKE_BINARY_DIR}/gen)
file(GLOB_RECURSE GML_SOURCES CONFIGURE_DEPENDS ${PROJECT_DIR}/*.gml ${PROJECT_DIR}/*.json)
file(GLOB RUNTIME_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/runtime/*.cpp)

# name tables for the interpreter: the runtime's builtins, GM's builtin variables and constants
add_custom_command(
  OUTPUT ${GEN_DIR}/gml_functions.h ${GEN_DIR}/gml_names.inc
  COMMAND Python3::Interpreter -I ${CMAKE_SOURCE_DIR}/tools/gen_tables.py ${CMAKE_SOURCE_DIR}/runtime
          ${CMAKE_SOURCE_DIR}/tools/gm_builtins.json ${GEN_DIR}
  DEPENDS ${CMAKE_SOURCE_DIR}/tools/gen_tables.py ${CMAKE_SOURCE_DIR}/tools/gm_builtins.json ${RUNTIME_SOURCES}
  COMMENT "Generating interpreter name tables")

# development only: pack build/toh.pak from extracted/project (or an instrumented copy) when there is one
if(EXISTS ${PROJECT_DIR}/game.json)
  set(PACK_PYTHON ${CMAKE_SOURCE_DIR}/tools/.venv/bin/python)
  add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/toh.pak
    COMMAND ${PACK_PYTHON} -I ${CMAKE_SOURCE_DIR}/tools/pack/pack.py ${PROJECT_DIR} ${CMAKE_BINARY_DIR}/toh.pak
    DEPENDS ${CMAKE_SOURCE_DIR}/tools/pack/pack.py ${GML_SOURCES}
    COMMENT "Packing assets")
  add_custom_target(pak ALL DEPENDS ${CMAKE_BINARY_DIR}/toh.pak)
endif()
option(TOH_RELEASE "Portable release build (static libstdc++)" OFF)

add_executable(toh
  runtime/main.cpp runtime/game.cpp runtime/render.cpp runtime/builtins.cpp runtime/value.cpp
  runtime/interp.cpp runtime/extract.cpp runtime/settings.cpp ${GEN_DIR}/gml_functions.h ${GEN_DIR}/gml_names.inc)

# OGG decoding (third_party/stb_vorbis, public domain / MIT)
add_library(stb_vorbis STATIC third_party/stb_vorbis/stb_vorbis.c)
set_target_properties(stb_vorbis PROPERTIES LINKER_LANGUAGE C)
target_compile_options(stb_vorbis PRIVATE -w)
target_include_directories(toh PRIVATE runtime ${GEN_DIR})
target_link_libraries(toh PRIVATE SDL3::SDL3 ZLIB::ZLIB stb_vorbis)
target_compile_options(toh PRIVATE -Wall -Wextra -Wno-unused-parameter)
if(TARGET pak)
  add_dependencies(toh pak)
endif()
if(TOH_RELEASE)
  target_link_options(toh PRIVATE -static-libstdc++ -static-libgcc)
endif()
