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
  add_executable(hg_core_tests tests/main.cpp tests/test_basics.cpp tests/test_app.cpp tests/test_timer.cpp tests/test_clock_face.cpp tests/test_qr.cpp tests/test_i18n.cpp tests/test_power.cpp tests/test_ws185.cpp tests/test_speaker_pa.cpp tests/test_speaker_queue.cpp tests/test_speaker_underruns.cpp tests/test_band_flush.cpp tests/test_frame_stats.cpp tests/test_shared_reply.cpp tests/test_tag_scanner.cpp tests/test_battery_curve.cpp tests/test_mic_power.cpp tests/test_wifi_retry.cpp tests/test_wifi_power.cpp tests/test_wifi_pick.cpp tests/test_wifi_fast.cpp tests/test_wifi_networks.cpp tests/test_ws_link.cpp tests/test_tailnet_watch.cpp tests/test_i2c_scan.cpp tests/test_qmi8658.cpp tests/test_bmi270.cpp tests/test_pcf85063.cpp drivers/axp2101.cpp drivers/cores3.cpp drivers/sticks3.cpp drivers/stc8_battery.cpp drivers/qmi8658.cpp drivers/bmi270.cpp drivers/pcf85063.cpp)
  target_include_directories(hg_core_tests PRIVATE drivers)
  target_link_libraries(hg_core_tests PRIVATE hg_core)
  if(MSVC)
    target_compile_options(hg_core_tests PRIVATE /utf-8)
  endif()
  add_test(NAME hg_core_tests COMMAND hg_core_tests)

  # TailTiny's Noise channel AEAD (esp32/components/TAILTINY.md) against RFC 8439. The vendored
  # C uses GCC builtins, so not on MSVC.
  if(NOT MSVC)
    enable_language(C)
    set(HG_WG_REFC esp32/components/wireguard_lwip/src/crypto/refc)
    add_executable(hg_tailtiny_tests tests/main.cpp tests/test_tailtiny_aead.cpp
      ${HG_WG_REFC}/chacha20poly1305.c ${HG_WG_REFC}/chacha20.c ${HG_WG_REFC}/poly1305-donna.c
      esp32/components/wireguard_lwip/src/crypto.c)
    target_include_directories(hg_tailtiny_tests PRIVATE ${HG_WG_REFC} esp32/components/tailtiny/src)
    add_test(NAME hg_tailtiny_tests COMMAND hg_tailtiny_tests)
  endif()

  # TailTiny's relay TLS check against a local TLS server, on the Mbed TLS ESP-IDF 6.1 ships. Off by
  # default because it downloads and builds Mbed TLS; CI turns it on.
  option(HG_TLS_TESTS "Build the TLS test (downloads Mbed TLS 4.1.0)" OFF)
  if(HG_TLS_TESTS AND NOT WIN32)
    include(FetchContent)
    set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
    set(TF_PSA_CRYPTO_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(mbedtls
      URL https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-4.1.0/mbedtls-4.1.0.tar.bz2
      URL_HASH SHA256=377a09cf8eb81b5fb2707045e5522d5489d3309fed5006c9874e60558fc81d10)
    FetchContent_MakeAvailable(mbedtls)
    add_executable(hg_tailtiny_tls_tests tests/main.cpp tests/test_tailtiny_tls.cpp)
    target_include_directories(hg_tailtiny_tls_tests PRIVATE esp32/components/tailtiny/src)
    target_link_libraries(hg_tailtiny_tls_tests PRIVATE mbedtls mbedx509 tfpsacrypto)
    add_test(NAME hg_tailtiny_tls_tests COMMAND hg_tailtiny_tls_tests)
  endif()
endif()
