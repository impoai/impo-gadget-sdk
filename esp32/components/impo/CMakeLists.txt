# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Impo: the voice companion UI that runs beside Home Link on boards with a
# display (CONFIG_IMPO_BOARD_*). Link-only builds register an empty component,
# except the Voice PE, which borrows the Hatch voice session.
#
# Requirements are resolved before Kconfig is known, so everything any board
# needs is listed unconditionally. Managed components (LVGL, codecs, BSPs) come
# from idf_component.yml, where rules keep them out of Link-only builds.
set(srcs)
set(embed)
if(CONFIG_IMPO_ENABLED)
    list(APPEND srcs
        "impo_app.c"
        "impo_board.c"
        "impo_console.c"
        "impo_state.c"
        "impo_battery.c"
        "impo_ui.c"
        "impo_audio.c"
        "impo_pmu.c"
        "impo_input.c"
        "impo_menu.c"
        "impo_voice.c"
        "impo_adpcm.c"
        "impo_settings.c"
        "impo_settings_ui.c"
        "impo_text.c"
        "impo_keypad.c"
        "impo_lv_mem.c"
        "impo_link.c"
        "impo_ble.c"
        "impo_commands.c")
    if(CONFIG_IMPO_HATCH)
        list(APPEND srcs "impo_chat.c" "impo_account_api.c" "impo_chat_session.cpp")
        list(APPEND embed "test_reply.mp3")   # impo_hatch_mp3_selftest
    else()
        list(APPEND srcs "impo_chat_link.c")   # text replies over Link's session
    endif()
    list(APPEND srcs "impo_chat_text.c")
    if(CONFIG_IMPO_CJK_FONT)
        list(APPEND srcs "fonts/impo_font_cjk_16.c")   # captions' CJK fallback
    endif()
    # Your own character (tools/impo/AVATAR_RECIPE.md) stands in for the default
    # avatar when avatar/impo_pixel.c exists here; this avatar/ is gitignored. The
    # default is in avatar/ at the top of the project.
    file(GLOB custom_avatar CONFIGURE_DEPENDS "${COMPONENT_DIR}/avatar/impo_pixel.c")
    if(custom_avatar)
        message(STATUS "Custom avatar: components/impo/avatar/impo_pixel.c")
        list(APPEND srcs "avatar/impo_pixel.c")
    else()
        list(APPEND srcs "../../avatar/impo_pixel.c")
    endif()
    if(CONFIG_IMPO_BOARD_WAVESHARE_S3_175C OR CONFIG_IMPO_BOARD_WAVESHARE_S3_175)
        # One driver: the 1.75 differs only in pins (its BSP's) and its PWR key.
        list(APPEND srcs "boards/board_waveshare_s3_175c.c" "boards/impo_lcd_bands.c")
    elseif(CONFIG_IMPO_BOARD_WAVESHARE_S3_216)
        list(APPEND srcs "boards/board_waveshare_s3_216.c" "boards/impo_lcd_bands.c")
    elseif(CONFIG_IMPO_BOARD_AIPI)
        list(APPEND srcs "boards/board_aipi.c")
    elseif(CONFIG_IMPO_BOARD_ESPRESSIF_SPARKBOT)
        list(APPEND srcs "boards/board_espressif_sparkbot.c" "boards/sparkbot_imu.c" "boards/sparkbot_touch.c")
    elseif(CONFIG_IMPO_BOARD_ESPRESSIF_BOX_3)
        list(APPEND srcs "boards/board_espressif_box_3.c" "boards/box_3_microphone.c")
    elseif(CONFIG_IMPO_BOARD_WAVESHARE_C6_18)
        list(APPEND srcs "boards/board_waveshare_c6_18.c")
    elseif(CONFIG_IMPO_BOARD_WAVESHARE_C6_206)
        list(APPEND srcs "boards/board_waveshare_c6_206.c")
    elseif(CONFIG_IMPO_BOARD_SENSECAP_WATCHER)
        list(APPEND srcs "boards/board_sensecap_watcher.c" "boards/impo_lcd_bands.c")
        if(CONFIG_IMPO_WATCHER_CAMERA)
            list(APPEND srcs "boards/watcher_camera.c")
        endif()
    elseif(CONFIG_IMPO_BOARD_M5STACK_CARDPUTER_ADV)
        list(APPEND srcs "boards/board_m5stack_cardputer_adv.c")
    elseif(CONFIG_IMPO_BOARD_M5STACK_STICKS3)
        list(APPEND srcs "boards/board_m5stack_sticks3.c")
    elseif(CONFIG_IMPO_BOARD_M5STACK_STICKC_PLUS2)
        list(APPEND srcs "boards/board_m5stack_stickc_plus2.c")
    elseif(CONFIG_IMPO_BOARD_M5STACK_STOPWATCH)
        list(APPEND srcs "boards/board_m5stack_stopwatch.c" "boards/impo_lcd_bands.c")
    elseif(CONFIG_IMPO_BOARD_M5STACK_CORES3)
        list(APPEND srcs "boards/board_m5stack_cores3.c")
    elseif(CONFIG_IMPO_BOARD_M5STACK_CORE2)
        list(APPEND srcs "boards/board_m5stack_core2.c")
    elseif(CONFIG_IMPO_BOARD_FNK0104B)
        list(APPEND srcs "boards/board_fnk0104b.c")
    elseif(CONFIG_IMPO_BOARD_GUITION_JC3248W535)
        list(APPEND srcs "boards/board_guition_jc3248w535.c")
    elseif(CONFIG_IMPO_BOARD_WAVESHARE_S3_LCD7)
        list(APPEND srcs "boards/board_waveshare_s3_lcd7.c")
    elseif(CONFIG_IMPO_BOARD_VN_S3_183)
        list(APPEND srcs "boards/board_vn_s3_183.c")
    elseif(CONFIG_IMPO_BOARD_AI_PASSPORT)
        list(APPEND srcs "boards/board_ai_passport.c")
    endif()
elseif(CONFIG_HOMEHUB_VOICE)
    # The Voice PE board uses just the Hatch voice session; the project's main/
    # voice_impo_chat.c stands in for the Impo settings, Wi-Fi and Link hooks.
    list(APPEND srcs "impo_chat.c" "impo_account_api.c" "impo_chat_session.cpp" "impo_chat_text.c" "impo_text.c")
    list(APPEND embed "test_reply.mp3")
endif()

set(impo_priv_requires
    esp_timer esp_driver_usb_serial_jtag esp_psram nvs_flash
    bt esp-tls mbedtls esp_app_format esp_http_client
    noise_core minimp3
    esp_lcd esp_adc esp_driver_i2s esp_driver_ledc esp_driver_spi esp_driver_uart esp_wifi
    esp_driver_pcnt)
# Requirements can't follow CONFIG_ here (they're read before the config is):
# the SparkBot's IMU driver and the touch pads are listed for every board.
list(APPEND impo_priv_requires camera bmi270 esp_driver_touch_sens)
if(CONFIG_IMPO_CAMERA)
    list(APPEND srcs "impo_camera.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "."
    EMBED_FILES ${embed}
    WHOLE_ARCHIVE   # impo_lv_mem.c provides symbols only LVGL references
    REQUIRES esp_driver_gpio esp_driver_i2c
    PRIV_REQUIRES ${impo_priv_requires}
)

if(CONFIG_IMPO_ENABLED)
    # LVGL spins forever on a failed assert unless LV_ASSERT_HANDLER is set,
    # which its Kconfig can't do. Abort, so the panic handler prints a backtrace
    # and reboots. PUBLIC reaches LVGL and every file that includes lvgl.h.
    idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
    target_compile_definitions(${lvgl_lib} PUBLIC
        "LV_ASSERT_HANDLER_INCLUDE=<stdlib.h>"
        "LV_ASSERT_HANDLER=abort()\;")
endif()
