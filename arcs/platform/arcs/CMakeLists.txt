listenai_library_named(gadget_platform_arcs)

listenai_library_sources(
    power_manager.c
    power_guard.c
    plat_init.c
    usb_adb.c
    app_usb_descriptors.c
    sys_cmds.c
    net_wifi.c
    plat_time.c
    plat_tls.c
    http_client.c
    ca_bundle.c
    crash_log.c
    plat_ble.c
    plat_watchdog.c
    plat_audio.c
)

listenai_include_directories(${CMAKE_CURRENT_SOURCE_DIR})

