#!/bin/sh
set -eu
cd "$(dirname "$0")/../../.."
output=$(mktemp -d)
trap 'rm -rf "$output"' EXIT
p=arcs/platform/arcs
t=arcs/tests/host
"${CC:-cc}" -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror \
  -include "$t/cloud_stubs/critical.h" -I"$t/cloud_stubs" -I"$p/esp_compat/include" \
  -I"$p" -I"$p/esp_compat" "$t/test_cloud_transport.c" "$p/http_client.c" \
  "$p/esp_compat/esp_http_client.c" "$p/esp_compat/esp_compat_url.c" \
  "$p/esp_compat/esp_tls.c" -o "$output/cloud"
"$output/cloud"
python3 esp32/tests/test_cloud_route.py

# The actual mbedTLS configuration test additionally needs the external SDK.
if [ -n "${ARCS_BASE:-}" ]; then
    M=$ARCS_BASE/modules/mbedtls/mbedtls
    set -- "$t/test_tls_identity.c"
    for source in "$M"/library/*.c; do
        case "$source" in */net_sockets.c) continue ;; esac
        set -- "$@" "$source"
    done
    "${CC:-cc}" -std=c11 -w -include stdbool.h -I"$t" \
      -DMBEDTLS_CONFIG_FILE=\"cloud_mbedtls_config.h\" -I"$t/cloud_tls_stubs" -I"$t/cloud_stubs" \
      -I"$M/include" "$@" -lm -o "$output/tls"
    "$output/tls"
fi
