listenai_library_named(gadget_pairing)

listenai_library_sources(
    pairing_crypto.c
    ble_setup.c
    pairing_service.c
)

listenai_include_directories(${CMAKE_CURRENT_SOURCE_DIR})
