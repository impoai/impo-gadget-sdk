# The Impo link that every gadget runs, built from the ESP32 sources unchanged
# (esp32/main: noise_control.cpp, vm_api.c, stack_monitor.c), and the ESP-IDF
# compatibility layer it is compiled against. The ESP-IDF headers in include/
# are seen by this library only; the rest of the firmware keeps the ARCS SDK's.
listenai_library_named(impo_gadget_sdk)

listenai_library_sources(
    esp_compat.c
    esp_compat_stubs.c
    esp_compat_url.c
    esp_http_client.c
    esp_tls.c
    ${UPSTREAM_MAIN}/noise_control.cpp
    ${UPSTREAM_MAIN}/stack_monitor.c
    ${UPSTREAM_MAIN}/vm_api.c
)

# Compat headers first; then esp32/main (gadget_attest.h, ota.h, led_status.h...) and
# esp32/components/impo (impo_commands.h, impo_state.h), for this library only.
target_include_directories(${LISTENAI_CURRENT_LIBRARY} BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(${LISTENAI_CURRENT_LIBRARY} PRIVATE ${UPSTREAM_MAIN} ${UPSTREAM_IMPO})
# esp_compat.h for the glue in link/.
listenai_include_directories(${CMAKE_CURRENT_SOURCE_DIR})

target_compile_options(${LISTENAI_CURRENT_LIBRARY} PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17 -fno-exceptions -fno-rtti -fno-threadsafe-statics>
)
