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

add_library(hg_core STATIC ${HG_CORE_SOURCES})
target_include_directories(hg_core PUBLIC include)
if(HG_CJK_FONT)
  target_compile_definitions(hg_core PUBLIC HG_CJK_FONT=1)
endif()
target_include_directories(hg_core PRIVATE third_party/qrcode)
target_compile_features(hg_core PUBLIC cxx_std_17)
set_target_properties(hg_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
  target_compile_options(hg_core PRIVATE /W4 /utf-8)
else()
  target_compile_options(hg_core PRIVATE -Wall -Wextra -fno-exceptions -fno-rtti)
endif()
