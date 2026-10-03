# ESP-IDF application for Hermes Gadget devices (ESP-IDF 5.3+; compile-checked with 6.1).
#   idf.py set-target esp32s3
#   idf.py menuconfig      # "Hermes Gadget" menu: board, pins, Wi-Fi, server
#   idf.py build flash monitor
cmake_minimum_required(VERSION 3.16)

# The portable device core is shared with the desktop simulator.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../core")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(hermes_gadget)
