# ESP32-S3 wake-word example. Build (ESP-IDF v5.x):
#   idf.py set-target esp32s3
#   idf.py build flash monitor
cmake_minimum_required(VERSION 3.16)
# The engine component comes from the repository checkout.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../sdk/esp32/wake")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(wake_demo)
