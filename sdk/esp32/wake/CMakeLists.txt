# Phonlet Wake engine as an ESP-IDF component.
# Compiles the shared engine sources (C++17, strict floating-point flags) and
# exposes the public phonlet_wake.h API. The application supplies the model
# container and enrollment JSON (e.g. via EMBED_FILES) and calls
# phonlet_wake_init_bytes / phonlet_wake_process.
cmake_minimum_required(VERSION 3.16)

file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/../../../engine/sources.txt" wake_sources)
list(TRANSFORM wake_sources PREPEND "${CMAKE_CURRENT_LIST_DIR}/../../../engine/")

find_package(Python3 REQUIRED COMPONENTS Interpreter)
# Generated at configure time: ESP-IDF re-invokes component lists in script
# mode where add_custom_command is not allowed.
set(wake_generated "${CMAKE_CURRENT_LIST_DIR}/build/languages.generated.h")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/build")
execute_process(
  COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_LIST_DIR}/../../../engine/tools/generate_languages.py"
          "${CMAKE_CURRENT_LIST_DIR}/../../../engine/inventories" "${wake_generated}"
  RESULT_VARIABLE wake_generation)
if(wake_generation)
  message(FATAL_ERROR "language table generation failed: ${wake_generation}")
endif()

idf_component_register(
  SRCS ${wake_sources} "${wake_generated}" esp_wake.c s3/s3_acoustic.cc s3/s3_kernels.c
  INCLUDE_DIRS include s3 "${CMAKE_CURRENT_LIST_DIR}/../../../include"
  PRIV_INCLUDE_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../../../engine/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../../engine/include/kws/detail"
    "${CMAKE_CURRENT_LIST_DIR}/../../../engine/src/session"
    "${CMAKE_CURRENT_LIST_DIR}/build"
  REQUIRES heap)
# The engine reports errors through C++ exceptions; ESP-IDF disables
# them by default for C++.
target_compile_options(${COMPONENT_LIB} PRIVATE -fexceptions -ffp-contract=off -fno-fast-math)
set_source_files_properties(${wake_sources} s3/s3_acoustic.cc s3/s3_kernels.c PROPERTIES COMPILE_FLAGS "-O3")
