#!/bin/sh
set -eu
cd "$(dirname "$0")"
output=$(mktemp -d)
trap 'rm -rf "$output"' EXIT
python3 test_product.py
python3 ../../tools/product.py --output "$output/default"
python3 ../../tools/product.py --product ../../examples/button_companion/product.json --output "$output/example"
python3 ../../tools/product.py --product fixtures/wake-product.json --output "$output/wake"
B=../../boards/arcs_mini_voice/gadget
G=../../gadget
S=../../components/impo_arcs_mini
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -I"$B" test_arcs_mini_button.c "$B/arcs_mini_button.c" -o "$output/button"
"$output/button"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Iruntime_stubs -I"$G" -I"$B" -I"$output/default" \
  test_arcs_mini_screen.c "$B/arcs_mini_screen_ui.c" -o "$output/screen"
"$output/screen"
for profile in default example wake; do
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Iruntime_stubs -I"$S/include" -I"$G" -I../../link \
      -I"$B" -I"$output/$profile" test_sdk_api.c "$S/src/impo_arcs_mini.c" -o "$output/api"
    id=arcs-mini
    wake=off
    if [ "$profile" = example ]; then id=mini-companion; fi
    if [ "$profile" = wake ]; then id=mini-wake-test; wake=on; fi
    "$output/api" "$id" "$wake"
done
"${CC:-cc}" -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -Iruntime_stubs -I"$G" \
  test_sound_buffer.c "$G/gadget_sound.c" "$G/gadget_pcm.c" -lm -o "$output/sound"
"$output/sound"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Iruntime_stubs test_mic_ownership.c -o "$output/mic"
"$output/mic"
"${CC:-cc}" -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror \
  -Iwifi_stubs -Iruntime_stubs test_wifi_preference.c -o "$output/wifi"
"$output/wifi"
sh run_cloud_route.sh
if [ -n "${ARCS_BASE:-}" ]; then
    J=$ARCS_BASE/modules/cjson
    M=$ARCS_BASE/modules/mbedtls/mbedtls
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -I"$J" -Iruntime_stubs -I"$G" -I../../link \
      test_speech_buffer.c "$G/gadget_pcm.c" "$J/cJSON.c" -lm -o "$output/speech"
    "$output/speech"
    "${CC:-cc}" -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -I"$J" -Iruntime_stubs -I"$G" \
      -I../../link test_voice_capture.c "$G/gadget_voice_logic.c" "$G/gadget_pcm.c" "$J/cJSON.c" -lm -o "$output/voice"
    "$output/voice"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -I. -DMBEDTLS_CONFIG_FILE='"host_sha256_config.h"' \
      -I"$J" -I"$S/include" -I"$M/include" test_wake_resources.c "$S/src/impo_arcs_wake_resources.c" \
      "$J/cJSON.c" "$M/library/sha256.c" "$M/library/platform_util.c" -lm -o "$output/wake-test"
    "$output/wake-test"
    set -- test_tls_reads.c
    for source in "$M"/library/*.c; do
        case "$source" in */net_sockets.c) continue ;; esac
        set -- "$@" "$source"
    done
    "${CC:-cc}" -std=c11 -w -include stdbool.h -I. -DMBEDTLS_CONFIG_FILE=\"host_tls_config.h\" \
      -Iruntime_stubs -I"$M/include" "$@" -lm -o "$output/tls"
    "$output/tls"
    sh run.sh
else
    echo 'ARCS_BASE unset: skipped external-SDK voice, wake, TLS and baseline harnesses'
fi
