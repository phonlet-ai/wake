# Phonlet Wake engine + JNI bridge for Android. The bridge is the same
# sdk/java/native/wake_jni.c used on the JVM; the engine sources are shared
# with every other backend.
cmake_minimum_required(VERSION 3.22.1)
project(wakejni LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(root "${CMAKE_CURRENT_LIST_DIR}/../../..")
file(STRINGS "${root}/engine/sources.txt" wake_sources)
list(TRANSFORM wake_sources PREPEND "${root}/engine/")

find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(wake_generated "${CMAKE_CURRENT_BINARY_DIR}/languages.generated.h")
add_custom_command(
  OUTPUT "${wake_generated}"
  COMMAND Python3::Interpreter "${root}/engine/tools/generate_languages.py"
          "${root}/engine/inventories" "${wake_generated}"
  DEPENDS "${root}/engine/tools/generate_languages.py"
  VERBATIM)

add_library(wakejni SHARED ${wake_sources} "${wake_generated}" "${root}/sdk/java/native/wake_jni.c")
target_include_directories(wakejni PRIVATE
  "${root}/include" "${root}/engine/include" "${root}/engine/include/kws/detail"
  "${root}/engine/src/session" "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_options(wakejni PRIVATE -O3 -ffp-contract=off -fno-fast-math)
# Play's 16 KB page requirement for newer devices.
target_link_options(wakejni PRIVATE "-Wl,-z,max-page-size=16384")
