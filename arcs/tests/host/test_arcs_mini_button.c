/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "arcs_mini_button.h"
static unsigned edge(arcs_mini_button_t *s, int down, uint32_t t)
{
    unsigned e = 0;
    for (unsigned i = 0; i < 4; ++i) e |= arcs_mini_button_poll(s, down, t + i * 10);
    return e;
}
int main(void)
{
    arcs_mini_button_t s = {0};
    assert(arcs_mini_button_poll(&s, true, 0) == 1);
    assert(edge(&s, false, 100) == 2); /* Power-on release never starts recording. */
    assert(edge(&s, true, 500) == 1);
    assert(edge(&s, false, 700) == 14); /* Exactly one voice trigger on short click. */
    for (uint32_t i = 0; i < 1000; ++i) assert(!arcs_mini_button_poll(&s, false, 800 + i * 10));
    assert(edge(&s, true, 12000) == 1);
    assert(edge(&s, false, 15100) == 2); /* Long power hold is not a voice turn. */
    assert(!arcs_mini_button_poll(&s, true, 16000));
    assert(!arcs_mini_button_poll(&s, false, 16010));
    assert(!arcs_mini_button_poll(&s, true, 16020));
    assert(!arcs_mini_button_poll(&s, false, 16030)); /* Contact bounce. */
    assert(edge(&s, true, UINT32_MAX - 100) == 1);
    assert(edge(&s, false, 100) == 14); /* Uptime wrap preserves short-click timing. */
    puts("ARCS Mini button: boot, debounce, short click, long hold, idle and wrap passed");
    return 0;
}
