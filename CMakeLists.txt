cmake_minimum_required(VERSION 3.20)
project(tower_of_heaven CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(SDL3 REQUIRED CONFIG)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# The game's code and assets come from your own copy of the game: run tools/extract.sh first.
set(PROJECT_DIR ${CMAKE_SOURCE_DIR}/extracted/project CACHE PATH "Extracted (or instrumented) game project")
set(GEN_DIR ${CMAKE_BINARY_DIR}/gen)
file(GLOB_RECURSE GML_SOURCES CONFIGURE_DEPENDS ${PROJECT_DIR}/*.gml ${PROJECT_DIR}/*.json)

add_custom_command(
  OUTPUT ${GEN_DIR}/gml_code.cpp ${GEN_DIR}/gml_ids.h ${GEN_DIR}/gml_scripts.h
  COMMAND Python3::Interpreter -I ${CMAKE_SOURCE_DIR}/tools/transpiler/gml2cpp.py ${PROJECT_DIR} ${GEN_DIR}
  DEPENDS ${CMAKE_SOURCE_DIR}/tools/transpiler/gml2cpp.py ${CMAKE_SOURCE_DIR}/tools/transpiler/gm_builtins.json ${GML_SOURCES}
  COMMENT "Transpiling GML to C++")

set(PACK_PYTHON ${CMAKE_SOURCE_DIR}/tools/.venv/bin/python)
add_custom_command(
  OUTPUT ${CMAKE_BINARY_DIR}/toh.pak
  COMMAND ${PACK_PYTHON} -I ${CMAKE_SOURCE_DIR}/tools/pack/pack.py ${PROJECT_DIR} ${CMAKE_BINARY_DIR}/toh.pak
  DEPENDS ${CMAKE_SOURCE_DIR}/tools/pack/pack.py ${GML_SOURCES}
  COMMENT "Packing assets")
add_custom_target(pak ALL DEPENDS ${CMAKE_BINARY_DIR}/toh.pak)

add_executable(toh
  runtime/main.cpp runtime/game.cpp runtime/render.cpp runtime/builtins.cpp runtime/value.cpp
  ${GEN_DIR}/gml_code.cpp)
target_include_directories(toh PRIVATE runtime ${GEN_DIR})
target_link_libraries(toh PRIVATE SDL3::SDL3)
target_compile_options(toh PRIVATE -Wall -Wextra -Wno-unused-parameter)
set_source_files_properties(${GEN_DIR}/gml_code.cpp PROPERTIES COMPILE_OPTIONS "-Wno-unused-label;-Wno-unused-variable")
add_dependencies(toh pak)
