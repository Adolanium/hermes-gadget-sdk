# Host build: the portable core, the simulator library and the unit tests.
# The ESP32 firmware has its own ESP-IDF project in esp32/.
cmake_minimum_required(VERSION 3.16)
project(hermes_gadget_firmware CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(core)

# Shared library loaded by the Python simulator host (hermes-gadget sim).
add_library(hgsim SHARED sim/src/hgsim.cpp)
target_include_directories(hgsim PUBLIC sim/include)
target_link_libraries(hgsim PRIVATE hg_core)
set_target_properties(hgsim PROPERTIES CXX_VISIBILITY_PRESET hidden)
if(MSVC)
  target_compile_options(hgsim PRIVATE /utf-8)
  # Keep the DLL next to the build root regardless of configuration.
  foreach(cfg Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER ${cfg} CFG)
    set_target_properties(hgsim PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${CFG} ${CMAKE_BINARY_DIR}/bin)
  endforeach()
endif()

option(HG_BUILD_TESTS "Build the core unit tests" ON)
if(HG_BUILD_TESTS)
  enable_testing()
  add_executable(hg_core_tests tests/main.cpp tests/test_basics.cpp tests/test_app.cpp tests/test_qr.cpp tests/test_power.cpp tests/test_ws185.cpp tests/test_speaker_pa.cpp tests/test_speaker_queue.cpp tests/test_band_flush.cpp tests/test_shared_reply.cpp tests/test_tag_scanner.cpp tests/test_wifi_retry.cpp tests/test_ws_link.cpp tests/test_i2c_scan.cpp drivers/axp2101.cpp drivers/cores3.cpp)
  target_include_directories(hg_core_tests PRIVATE drivers)
  target_link_libraries(hg_core_tests PRIVATE hg_core)
  if(MSVC)
    target_compile_options(hg_core_tests PRIVATE /utf-8)
  endif()
  add_test(NAME hg_core_tests COMMAND hg_core_tests)
endif()
