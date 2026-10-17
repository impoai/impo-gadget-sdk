/*
 * noise_control.cpp's PsaCryptoBackend on ARCS. The SDK ships mbedtls 2.28
 * without PSA, so the session uses noise_core's MbedtlsCryptoBackend: the same
 * CryptoBackend interface (X25519, SHA-256, HKDF, AES-256-GCM) on mbedtls's
 * classic API, as the ESP32 builds did before the PSA backend.
 */
#pragma once

#include <xplat/noise/core/MbedtlsCryptoBackend.h>

namespace impogadgets::noise::core {
using PsaCryptoBackend = MbedtlsCryptoBackend;
} // namespace impogadgets::noise::core
