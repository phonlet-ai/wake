# INMP441 wiring check for ESP32-S3 (N16R8). Build (ESP-IDF v5.4):
#   idf.py set-target esp32s3
#   idf.py build
# No engine or model: this firmware only proves the microphone path.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(mic_check)
