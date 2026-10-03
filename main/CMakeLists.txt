# Before each link, runs tools/latest_mtime.py to emit build/generated/project_build_info.h
# (PROJECT_BUILD_NUMBER) and stamp /* BUILD_NUMBER: … */ on sources. When VU_BUILD_LABEL /
# BUILD_LABEL_WALL_CLOCK is set (all.sh), the label uses wall-clock time instead of newest
# source mtime so every full build gets a fresh identifier.

idf_component_register(
    SRCS
        "app_main.cpp"
        "vu_meter.cpp"
        "vu_leds.c"
        "wifi_sta.c"
        "web_ui.c"
        "adc_input.c"
    INCLUDE_DIRS
        "."
        ".."
    REQUIRES
        nvs_flash
        esp_wifi
        esp_event
        esp_netif
        esp_http_server
        esp_timer
        esp_adc
        esp_driver_gpio
        app_update
        esp_app_format
        led_strip
        freertos
        log
)

set(BUILD_TIMESTAMP_SCRIPT "${CMAKE_SOURCE_DIR}/tools/latest_mtime.py")
if(NOT DEFINED PYTHON)
    set(PYTHON python3)
endif()
set(BUILD_INFO_DIR "${CMAKE_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${BUILD_INFO_DIR}")
set(BUILD_INFO_HEADER "${BUILD_INFO_DIR}/project_build_info.h")

set(BUILD_TIMESTAMP_EXTRA_ARGS "")
if(DEFINED ENV{VU_BUILD_LABEL} AND NOT "$ENV{VU_BUILD_LABEL}" STREQUAL "")
    list(APPEND BUILD_TIMESTAMP_EXTRA_ARGS --build "$ENV{VU_BUILD_LABEL}")
elseif(DEFINED ENV{PROJECT_BUILD_LABEL} AND NOT "$ENV{PROJECT_BUILD_LABEL}" STREQUAL "")
    list(APPEND BUILD_TIMESTAMP_EXTRA_ARGS --build "$ENV{PROJECT_BUILD_LABEL}")
elseif(DEFINED ENV{BUILD_LABEL_WALL_CLOCK} AND NOT "$ENV{BUILD_LABEL_WALL_CLOCK}" STREQUAL "")
    list(APPEND BUILD_TIMESTAMP_EXTRA_ARGS --wall-clock)
endif()

file(GLOB_RECURSE BUILD_INFO_WATCH_FILES
    LIST_DIRECTORIES false
    "${CMAKE_SOURCE_DIR}/main/*")
set(BUILD_INFO_SOURCE_DEPS "")
foreach(path IN LISTS BUILD_INFO_WATCH_FILES)
    if(NOT path MATCHES ".*/build/.*")
        list(APPEND BUILD_INFO_SOURCE_DEPS "${path}")
    endif()
endforeach()

# Run on every build (ALL) so /* BUILD_NUMBER: … */ is always synced with project_build_info.h.
add_custom_target(build_info_header ALL
    COMMAND ${PYTHON} "${BUILD_TIMESTAMP_SCRIPT}" "${CMAKE_SOURCE_DIR}"
            --header "${BUILD_INFO_HEADER}" ${BUILD_TIMESTAMP_EXTRA_ARGS}
    DEPENDS "${BUILD_TIMESTAMP_SCRIPT}" ${BUILD_INFO_SOURCE_DEPS}
    BYPRODUCTS "${BUILD_INFO_HEADER}"
    COMMENT "Generating build info header and stamping sources"
    VERBATIM
)
add_dependencies(${COMPONENT_LIB} build_info_header)
target_include_directories(${COMPONENT_LIB} PRIVATE "${BUILD_INFO_DIR}")
