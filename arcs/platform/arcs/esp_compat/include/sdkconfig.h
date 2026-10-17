/*
 * The ESP-IDF build configuration the shared link (esp32/main:
 * noise_control.cpp and vm_api.c, built unchanged) is compiled
 * with on ARCS. Only the switches those files read; everything not listed is
 * off (an undefined CONFIG_ is 0 in #if).
 */
#pragma once

#include <autoconf.h>   /* the ARCS Kconfig, e.g. CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN */

#ifdef __cplusplus
extern "C" {
#endif

/* An Impo build: request streams (noise_ctrl_req_*), impo_commands_describe/
 * capabilities and impo_state_power(), bridged to gadget/ in link/impo_glue_commands.c. */
#define CONFIG_IMPO_ENABLED 1

/*
 * CONFIG_SPIRAM stays unset although every ARCS board has PSRAM. It selects
 * upstream's buffer profile for Impo riding Link's own session
 * (SMALL_CONTROL_SESSION in noise_control.cpp): 17 KB inbound frames, which
 * the gateway's 16 KB /tts and /chat/subscribe chunks need. With CONFIG_SPIRAM
 * the session takes 12 KB frames, because PSRAM ESP32 boards run chat on a
 * second session of their own (impo_chat_session.cpp). The buffers still land
 * in PSRAM: malloc() puts anything over 1 KB there.
 */

/* Unset as well: CONFIG_HOMEHUB_TUNNEL (no home-network tunnel), the display,
 * camera, voice.configure and sensor commands, CONFIG_HOMEHUB_SHOW_AGENT_NAME. */

/* The product this custom gadget declares in link.register (contracts/gadget-identity.md).
 * The board's gadget_board_t.product_id, set by link/ at start; "" when not set up. */
const char *esp_compat_product_id(void);
#define CONFIG_GADGET_PRODUCT_ID esp_compat_product_id()

#ifdef __cplusplus
}
#endif
