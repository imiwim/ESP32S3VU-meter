# ESP32 VU Meter — ESP-IDF port of ESP32VU-meter_241230 (Arduino/PlatformIO).
#
# Generates PROJECT_BUILD_NUMBER before project() so esp_app_desc.version embeds the
# same YYMMDD.HHMMSS label as project_build_info.h (visible in OTA UI and logs).

cmake_minimum_required(VERSION 3.22)

if(NOT DEFINED PYTHON)
    set(PYTHON python3)
endif()

set(_LMT_EXTRA --no-stamp)
if(DEFINED ENV{VU_BUILD_LABEL} AND NOT "$ENV{VU_BUILD_LABEL}" STREQUAL "")
    list(APPEND _LMT_EXTRA --build "$ENV{VU_BUILD_LABEL}")
elseif(DEFINED ENV{PROJECT_BUILD_LABEL} AND NOT "$ENV{PROJECT_BUILD_LABEL}" STREQUAL "")
    list(APPEND _LMT_EXTRA --build "$ENV{PROJECT_BUILD_LABEL}")
elseif(DEFINED ENV{BUILD_LABEL_WALL_CLOCK} AND NOT "$ENV{BUILD_LABEL_WALL_CLOCK}" STREQUAL "")
    list(APPEND _LMT_EXTRA --wall-clock)
endif()

# Label for project(VERSION …) only — do not write project_build_info.h here (that skips
# source stamping when the build custom target sees an up-to-date header from configure).
execute_process(
    COMMAND ${PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/tools/latest_mtime.py"
            "${CMAKE_CURRENT_SOURCE_DIR}"
            ${_LMT_EXTRA}
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    RESULT_VARIABLE _LMT_RC
    OUTPUT_VARIABLE PROJECT_VER
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_VARIABLE _LMT_ERR
)
# CMake MATCHES has no {n} quantifiers — spell out six digits each side.
if(NOT _LMT_RC EQUAL 0 OR NOT PROJECT_VER MATCHES "^[0-9][0-9][0-9][0-9][0-9][0-9]\\.[0-9][0-9][0-9][0-9][0-9][0-9]$")
    message(WARNING "latest_mtime.py at configure failed (${_LMT_RC}): ${_LMT_ERR}; got '${PROJECT_VER}', using PROJECT_VER=0.0.0")
    set(PROJECT_VER "0.0.0")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ESP32VU-meter VERSION ${PROJECT_VER})
