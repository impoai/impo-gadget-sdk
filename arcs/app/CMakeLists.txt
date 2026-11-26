cmake_minimum_required(VERSION 3.13)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT DEFINED BOARD)
    set(BOARD "arcs_mini_v2_1")
endif()

if(BOARD STREQUAL "arcs_mini_voice")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(IMPO_PRODUCT_FILE "${CMAKE_CURRENT_SOURCE_DIR}/../products/arcs_mini.json" CACHE FILEPATH "ARCS Mini product JSON")
    set(IMPO_PRODUCT_FONT "" CACHE FILEPATH "Optional pinned Noto Sans SC source")
    set(IMPO_APP_DIR "" CACHE PATH "Optional application implementing impo_arcs_mini_application")
    set(IMPO_PRODUCT_DIR "${CMAKE_CURRENT_BINARY_DIR}/impo_product")
    set(product_args --product "${IMPO_PRODUCT_FILE}" --output "${IMPO_PRODUCT_DIR}")
    if(IMPO_PRODUCT_FONT)
        list(APPEND product_args --font "${IMPO_PRODUCT_FONT}")
    endif()
    execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/product.py"
        ${product_args} RESULT_VARIABLE product_result)
    if(NOT product_result EQUAL 0)
        message(FATAL_ERROR "Invalid ARCS Mini product configuration")
    endif()
    include("${IMPO_PRODUCT_DIR}/inputs.cmake")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${IMPO_PRODUCT_INPUTS}
        "${CMAKE_CURRENT_SOURCE_DIR}/../tools/product.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/../tools/gen_screen_font.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/../components/impo_arcs_mini/include/impo_arcs_mini.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/../components/impo_arcs_mini/assets/status_font.h")
    # Explicit CONFIG_FILES keep the normal final override priority.
    set(CONFIG_FILES "${IMPO_PRODUCT_DIR}/product.conf" ${CONFIG_FILES})
endif()

find_package(listenai-cmake REQUIRED HINTS $ENV{ARCS_BASE})

project(impo-gadget)

get_filename_component(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/.. ABSOLUTE)
# The repository root: ARCS builds the shared link and noise core straight from esp32/.
get_filename_component(SDK_ROOT ${REPO_ROOT}/.. ABSOLUTE)
set(UPSTREAM_MAIN ${SDK_ROOT}/esp32/main)
set(UPSTREAM_IMPO ${SDK_ROOT}/esp32/components/impo)
set(UPSTREAM_NOISE_CORE ${SDK_ROOT}/esp32/components/noise_core)
# Shared route policy; only its implementation sees ESP-IDF compatibility headers.
listenai_include_directories(${SDK_ROOT}/esp32/components/impo_cloud_route/include)

# memap.h is read by the SDK linker scripts from the application directory.
listenai_include_directories(${CMAKE_CURRENT_SOURCE_DIR})

add_subdirectory(${REPO_ROOT}/platform/arcs ${CMAKE_BINARY_DIR}/platform)
add_subdirectory(${REPO_ROOT}/platform/arcs/noise_core ${CMAKE_BINARY_DIR}/noise_core)
add_subdirectory(${REPO_ROOT}/platform/arcs/esp_compat ${CMAKE_BINARY_DIR}/impo_gadget_sdk)
add_subdirectory(${REPO_ROOT}/gadget ${CMAKE_BINARY_DIR}/gadget)
add_subdirectory(${REPO_ROOT}/boards/${BOARD}/gadget ${CMAKE_BINARY_DIR}/board_gadget)
add_subdirectory(${REPO_ROOT}/link ${CMAKE_BINARY_DIR}/link)
add_subdirectory(${REPO_ROOT}/features/pairing ${CMAKE_BINARY_DIR}/pairing)

listenai_add_executable(${PROJECT_NAME})

target_sources(${PROJECT_NAME} PRIVATE main.c)
