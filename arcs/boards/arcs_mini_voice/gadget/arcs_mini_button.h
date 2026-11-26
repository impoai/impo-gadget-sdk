/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef struct {
    bool initialized, last, stable, boot_press;
    unsigned same;
    uint32_t pressed_at;
} arcs_mini_button_t;
/* 1=MAIN_PRESS, 2=MAIN_RELEASE, 4=AUX_PRESS, 8=AUX_RELEASE. */
unsigned arcs_mini_button_poll(arcs_mini_button_t *s, bool down, uint32_t now_ms);
