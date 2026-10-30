# Portable device core. Builds as an ESP-IDF component or a plain CMake library.
set(HG_CORE_SOURCES
    src/app.cpp
    src/app_settings.cpp
    src/app_power.cpp
    src/app_perf.cpp
    src/app_phone.cpp
    src/app_timer.cpp
    src/canvas.cpp
    src/crypto.cpp
    src/font5x7.cpp
    src/font_cjk.cpp
    src/font_cjk_data.cpp
    src/i18n.cpp
    src/i18n_es.cpp
    src/frame_stats.cpp
    src/json.cpp
    src/mascot.cpp
    src/mascot_data.cpp
    src/protocol.cpp
    src/qr.cpp
    src/setup.cpp
    src/touch.cpp
    src/ui.cpp
    src/vad.cpp
    src/wifi_networks.cpp
    # Vendored QR encoder (MIT, Richard Moore / Project Nayuki). Adapted for this
    # project; see NOTICE. Committed as .cpp because the project is CXX-only.
    third_party/qrcode/qrcode.cpp
)

# The 12x12 CJK font and the taller text lines it needs (docs/cjk.md). Everything that
# includes hg/font.hpp must see the same value, so it travels with the core as a PUBLIC definition.
option(HG_CJK_FONT "Build in the 12x12 CJK font and its taller text lines" OFF)

if(ESP_PLATFORM)
  idf_component_register(SRCS ${HG_CORE_SOURCES} INCLUDE_DIRS include third_party/qrcode)
  if(HG_CJK_FONT)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC HG_CJK_FONT=1)
  endif()
  return()
endif()

# hg_core follows HG_CJK_FONT. The host tests also build hg_core_cjk, with the font always in,
# so the CJK code is tested on every build (firmware/CMakeLists.txt).
list(TRANSFORM HG_CORE_SOURCES PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")
set(HG_CORE_SOURCES "${HG_CORE_SOURCES}" CACHE INTERNAL "")
set(HG_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}" CACHE INTERNAL "")
function(hg_core_library name cjk)
  add_library(${name} STATIC ${HG_CORE_SOURCES})
  target_include_directories(${name} PUBLIC ${HG_CORE_DIR}/include)
  if(cjk)
    target_compile_definitions(${name} PUBLIC HG_CJK_FONT=1)
  endif()
  target_include_directories(${name} PRIVATE ${HG_CORE_DIR}/third_party/qrcode)
  target_compile_features(${name} PUBLIC cxx_std_17)
  set_target_properties(${name} PROPERTIES POSITION_INDEPENDENT_CODE ON)
  if(MSVC)
    target_compile_options(${name} PRIVATE /W4 /utf-8)
  else()
    target_compile_options(${name} PRIVATE -Wall -Wextra -fno-exceptions -fno-rtti)
  endif()
endfunction()
hg_core_library(hg_core ${HG_CJK_FONT})
