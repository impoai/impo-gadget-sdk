/* SPDX-License-Identifier: MIT */
#include "arcs_mini_button.h"
unsigned arcs_mini_button_poll(arcs_mini_button_t *s, bool down, uint32_t now_ms)
{
    if (!s->initialized) {
        s->initialized = true;
        s->last = s->stable = s->boot_press = down;
        s->pressed_at = now_ms;
        return down ? 1u : 0u;
    }
    s->same = down == s->last ? (s->same < 3 ? s->same + 1 : 3) : 0;
    s->last = down;
    if (s->same < 3 || down == s->stable) return 0;
    s->stable = down;
    if (down) {
        s->pressed_at = now_ms;
        return 1u;
    }
    unsigned e = 2u;
    uint32_t held = now_ms - s->pressed_at;
    if (!s->boot_press && held >= 30 && held < 1000) e |= 4u | 8u;
    s->boot_press = false;
    return e;
}
