#!/bin/bash
# Build and run host tests against the SDK's mbedtls 2.28 and cJSON sources.
set -e
cd "$(dirname "$0")"
: "${ARCS_BASE:?set ARCS_BASE to the ARCS SDK}"
M=$ARCS_BASE/modules/mbedtls/mbedtls
J=$ARCS_BASE/modules/cjson
OUT=../../tmp/host
mkdir -p $OUT
CFLAGS="-O1 -w -include stdbool.h -I$M/include -I$J -I../../features/pairing -I. -DMBEDTLS_CONFIG_FILE=\"host_mbedtls_config.h\" -DBLE_SETUP_HOST_TEST"
LIBS="$(ls $M/library/*.c | grep -v -E 'net_sockets|timing|entropy_poll|psa_') $J/cJSON.c"
cc $CFLAGS test_pairing_crypto.c ../../features/pairing/pairing_crypto.c $LIBS -o $OUT/test_pairing_crypto
cc $CFLAGS test_ble_setup.c ../../features/pairing/ble_setup.c ../../features/pairing/pairing_crypto.c $LIBS -o $OUT/test_ble_setup
$OUT/test_pairing_crypto | tail -1
$OUT/test_ble_setup

# SDK pure logic (gadget/): audio formats, voice turn decisions.
G=../../gadget
cc $CFLAGS -I$G test_gadget_pcm.c $G/gadget_pcm.c -lm -o $OUT/test_gadget_pcm
cc $CFLAGS -I$G test_gadget_voice_logic.c $G/gadget_voice_logic.c $J/cJSON.c -o $OUT/test_gadget_voice_logic
cc $CFLAGS -I$G -I$J test_gadget_policy.c $G/gadget_policy.c -o $OUT/test_gadget_policy
cc $CFLAGS -I$G -I$J test_gadget_params.c $G/gadget_params.c $J/cJSON.c -lm -o $OUT/test_gadget_params
$OUT/test_gadget_pcm | grep -v '^ok'
$OUT/test_gadget_policy | grep -v '^ok'
$OUT/test_gadget_params | grep -v '^ok'
$OUT/test_gadget_voice_logic | grep -v '^ok'

# Link glue pure logic (link/) and the esp_http_client compat's URL handling.
L=../../link
E=../../platform/arcs/esp_compat
cc $CFLAGS -I$L -I$E -I$J test_link_glue.c $L/link_glue_logic.c $E/esp_compat_url.c $J/cJSON.c -o $OUT/test_link_glue
$OUT/test_link_glue | grep -v '^ok'

# Board pure logic (boards/arcs_mini_v2_1/gadget/dog_logic.c).
B=../../boards/arcs_mini_v2_1/gadget
cc $CFLAGS -I$B test_dog_logic.c $B/dog_logic.c -lm -o $OUT/test_dog_logic
$OUT/test_dog_logic | grep -v '^ok'
