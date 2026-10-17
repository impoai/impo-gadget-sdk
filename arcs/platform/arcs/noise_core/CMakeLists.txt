# The Noise core of esp32/components/noise_core, built for ARCS. The ARCS SDK's
# mbedtls 2.28 has no PSA, so the session uses MbedtlsCryptoBackend; newlib here
# has no gthreads, so NOISE_CORE_FREERTOS_MUTEX gives it a FreeRTOS mutex.
listenai_library_named(noise_core)

listenai_library_sources(
    ${UPSTREAM_NOISE_CORE}/src/ClientSession.cpp
    ${UPSTREAM_NOISE_CORE}/src/InitiatorHandshake.cpp
    ${UPSTREAM_NOISE_CORE}/src/MbedtlsCryptoBackend.cpp
    ${UPSTREAM_NOISE_CORE}/src/ServiceCodec.cpp
    ${UPSTREAM_NOISE_CORE}/src/Status.cpp
    ${UPSTREAM_NOISE_CORE}/src/Transport.cpp
    ${UPSTREAM_NOISE_CORE}/src/TransportFrameCodec.cpp
)

listenai_include_directories(${UPSTREAM_NOISE_CORE}/include)

target_compile_definitions(${LISTENAI_CURRENT_LIBRARY} PRIVATE NOISE_CORE_FREERTOS_MUTEX=1)
target_compile_options(${LISTENAI_CURRENT_LIBRARY} PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17 -fno-exceptions -fno-rtti -fno-threadsafe-statics>
)
