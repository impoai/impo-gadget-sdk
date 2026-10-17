cmake_minimum_required(VERSION 3.13)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT DEFINED BOARD)
    set(BOARD "arcs_mini_v2_1")
endif()

find_package(listenai-cmake REQUIRED HINTS $ENV{ARCS_BASE})

project(impo-gadget)

get_filename_component(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/.. ABSOLUTE)
# The repository root: ARCS builds the shared link and noise core straight from esp32/.
get_filename_component(SDK_ROOT ${REPO_ROOT}/.. ABSOLUTE)
set(UPSTREAM_MAIN ${SDK_ROOT}/esp32/main)
set(UPSTREAM_IMPO ${SDK_ROOT}/esp32/components/impo)
set(UPSTREAM_NOISE_CORE ${SDK_ROOT}/esp32/components/noise_core)

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
