# ESP-IDF application for Hermes Gadget devices (ESP-IDF 5.3+; built with 6.1).
#   idf.py set-target esp32s3
#   idf.py menuconfig      # "Hermes Gadget" menu: board, pins, Wi-Fi, server
#   idf.py build flash monitor
cmake_minimum_required(VERSION 3.16)

# The portable device core is shared with the desktop simulator.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../core")

# ESP32-P4 boards bring Wi-Fi through the ESP32-C6 companion via
# esp_wifi_remote, whose Kconfig does `orsource ./idf_v$ESP_IDF_VERSION/...`.
# If ESP_IDF_VERSION is unset the slave-target choice never parses and the
# build dies with `#error "Unknown Slave Target"`. idf.py/export.sh do not
# always export it, so derive major.minor from IDF_PATH/version.txt before
# Kconfig runs. The regex below accepts the leading `v` in tags such as v6.1.0.
if(NOT DEFINED ENV{ESP_IDF_VERSION} AND DEFINED ENV{IDF_PATH})
    set(_hg_idf_ver "")
    foreach(_hg_vf "$ENV{IDF_PATH}/version.txt")
        if(EXISTS "${_hg_vf}")
            file(READ "${_hg_vf}" _hg_vtxt)
            string(REGEX MATCH "v([0-9]+\\.[0-9]+)" _hg_m "${_hg_vtxt}")
            if(_hg_m)
                set(_hg_idf_ver "${CMAKE_MATCH_1}")
            endif()
        endif()
    endforeach()
    if(_hg_idf_ver)
        set(ENV{ESP_IDF_VERSION} "${_hg_idf_ver}")
    endif()
endif()

# ESP-IDF 5.x does not define this 6.x-only mbedTLS Kconfig symbol. IDF warns
# and ignores it, but the strict defaults check would otherwise reject the build.
# Filter only this known-incompatible line in build-directory copies; keep the
# repository defaults intact for IDF 6.x, where the option is supported.
if(DEFINED SDKCONFIG_DEFAULTS AND DEFINED ENV{ESP_IDF_VERSION} AND "$ENV{ESP_IDF_VERSION}" VERSION_LESS "6.0")
    set(_hg_filtered_defaults)
    set(_hg_defaults_changed FALSE)
    set(_hg_default_index 0)
    foreach(_hg_default IN LISTS SDKCONFIG_DEFAULTS)
        if(IS_ABSOLUTE "${_hg_default}")
            set(_hg_default_path "${_hg_default}")
        else()
            set(_hg_default_path "${CMAKE_CURRENT_LIST_DIR}/${_hg_default}")
        endif()
        if(EXISTS "${_hg_default_path}")
            file(READ "${_hg_default_path}" _hg_defaults_text)
            if(_hg_defaults_text MATCHES "CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY=y")
                string(REPLACE "CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY=y" "" _hg_defaults_text "${_hg_defaults_text}")
                set(_hg_idf_version "$ENV{ESP_IDF_VERSION}")
                set(_hg_filtered "${CMAKE_BINARY_DIR}/sdkconfig.defaults.idf${_hg_idf_version}.${_hg_default_index}")
                file(WRITE "${_hg_filtered}" "${_hg_defaults_text}")
                list(APPEND _hg_filtered_defaults "${_hg_filtered}")
                set(_hg_defaults_changed TRUE)
            else()
                list(APPEND _hg_filtered_defaults "${_hg_default}")
            endif()
        else()
            list(APPEND _hg_filtered_defaults "${_hg_default}")
        endif()
        math(EXPR _hg_default_index "${_hg_default_index} + 1")
    endforeach()
    if(_hg_defaults_changed)
        set(SDKCONFIG_DEFAULTS "${_hg_filtered_defaults}" CACHE STRING "Filtered defaults for this ESP-IDF version" FORCE)
    endif()
endif()

# The firmware version: reported to Hermes and written into the image, where
# `hermes gadget update` reads it. Override with -DPROJECT_VER=...
if(NOT DEFINED PROJECT_VER)
    set(PROJECT_VER "0.2.0")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(hermes_gadget)

# Stop when a defaults line didn't reach the generated sdkconfig (a typo, an
# option for another chip, an unmet dependency, or an sdkconfig left over from
# an older checkout). PlatformIO runs this configure step too.
idf_build_get_property(hg_python PYTHON)
idf_build_get_property(hg_sdkconfig SDKCONFIG)
idf_build_get_property(hg_defaults SDKCONFIG_DEFAULTS)
execute_process(
    COMMAND ${hg_python} ${CMAKE_CURRENT_LIST_DIR}/tools/check_config.py
            --sdkconfig ${hg_sdkconfig} --defaults "${hg_defaults}" --root ${CMAKE_CURRENT_LIST_DIR}
    RESULT_VARIABLE hg_config_check)
if(NOT hg_config_check EQUAL 0)
    message(FATAL_ERROR "The generated sdkconfig doesn't match its defaults (see above).")
endif()
