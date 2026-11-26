/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "arcs_mini_screen_ui.h"

enum { N = ARCS_MINI_SCREEN_WIDTH * ARCS_MINI_SCREEN_HEIGHT };
static uint16_t storage[N+2], previous[N];
static void guards(void) { assert(storage[0] == 0xa55a && storage[N+1] == 0x5aa5); }
static void preview(const char *dir, const char *name, uint16_t *p)
{
    if (!dir) return;
    char path[1024];
    assert(snprintf(path, sizeof(path), "%s/%s.ppm", dir, name) < (int)sizeof(path));
    FILE *f = fopen(path, "wb"); assert(f);
    fprintf(f, "P6\n240 240\n255\n");
    for (int i = 0; i < N; i++) {
        unsigned char c[3] = { ((p[i]>>11)&31)*255/31, ((p[i]>>5)&63)*255/63, (p[i]&31)*255/31 };
        assert(fwrite(c, 1, 3, f) == 3);
    }
    assert(fclose(f) == 0);
}
int main(int argc, char **argv)
{
    uint16_t *p = storage+1;
    storage[0] = 0xa55a; storage[N+1] = 0x5aa5;
    memset(p, 0x47, sizeof(previous));
    memcpy(previous, p, sizeof(previous));
    assert(!arcs_mini_screen_render(NULL, 240, 240, GADGET_STATUS_IDLE, 0, false, true, false));
    assert(!arcs_mini_screen_render(p, 239, 240, GADGET_STATUS_IDLE, 0, false, true, false));
    assert(!arcs_mini_screen_render(p, 240, 241, GADGET_STATUS_IDLE, 0, false, true, false));
    assert(memcmp(previous, p, sizeof(previous)) == 0); guards();
    /* Exercise every SDK state, blink boundary, animation phase and uptime wrap. */
    const uint32_t times[] = { 0, 400, 500, 1000, 4799, 4800, 4999, 5000, UINT32_MAX };
    for (int st = GADGET_STATUS_BOOTING; st <= GADGET_STATUS_UPDATING; st++)
        for (unsigned t = 0; t < sizeof(times)/sizeof(times[0]); t++) {
            assert(arcs_mini_screen_render(p, 240, 240, (gadget_status_t)st, times[t], false, true, false)); guards();
        }
    assert(arcs_mini_screen_render(p,240,240,GADGET_STATUS_IDLE,1000,false,false,false));
    memcpy(previous,p,sizeof(previous));
    assert(arcs_mini_screen_render(p,240,240,GADGET_STATUS_IDLE,1000,false,true,false));
    assert(memcmp(previous,p,sizeof(previous))!=0); memcpy(previous,p,sizeof(previous));
    assert(arcs_mini_screen_render(p,240,240,GADGET_STATUS_IDLE,1000,false,true,true));
    assert(memcmp(previous,p,sizeof(previous))!=0); guards();
    const struct { gadget_status_t state; const char *name; } samples[] = {
        { GADGET_STATUS_IDLE, "idle" }, { GADGET_STATUS_LISTENING, "listening" },
        { GADGET_STATUS_THINKING, "thinking" }, { GADGET_STATUS_SPEAKING, "speaking" },
        { GADGET_STATUS_OFFLINE, "offline" }
    };
    for (unsigned i = 0; i < sizeof(samples)/sizeof(samples[0]); i++) {
        assert(arcs_mini_screen_render(p, 240, 240, samples[i].state, 1000, false, true, false));
        if (i) assert(memcmp(previous, p, sizeof(previous)) != 0);
        memcpy(previous, p, sizeof(previous));
        preview(argc > 1 ? argv[1] : NULL, samples[i].name, p);
    }
    assert(arcs_mini_screen_render(p, 240, 240, (gadget_status_t)999, 0, true, true, false)); guards();
    assert(p[100*240+30] == 0xf800 && p[100*240+90] == 0x07e0);
    assert(p[100*240+150] == 0x001f && p[100*240+210] == 0xffff);
    assert(p[0] == 0xefbf && p[N-1] == p[0]);
    preview(argc > 1 ? argv[1] : NULL, "colors", p);
    puts("screen: bounds, all SDK states, animation and RGB565 colors passed");
    return 0;
}
