/* SPDX-License-Identifier: MIT */
#include <stddef.h>
#include "arcs_mini_screen_ui.h"
#include "impo_product_font.h"
#include "impo_product_config.h"

#define RGB(r,g,b) ((uint16_t)(((r)>>3)<<11 | ((g)>>2)<<5 | ((b)>>3)))
#define BG RGB(8,16,24)
#define WHITE RGB(235,245,250)
#define MUTED RGB(112,137,151)
#define CYAN RGB(66,214,232)
#define GREEN RGB(110,232,170)
#define AMBER RGB(248,190,84)

static void pixel(uint16_t *p, int x, int y, uint16_t color)
{
    if ((unsigned)x < ARCS_MINI_SCREEN_WIDTH && (unsigned)y < ARCS_MINI_SCREEN_HEIGHT)
        p[y * ARCS_MINI_SCREEN_WIDTH + x] = color;
}
static void rect(uint16_t *p, int x, int y, int w, int h, uint16_t c)
{
    for (int yy = y; yy < y+h; yy++) for (int xx = x; xx < x+w; xx++) pixel(p, xx, yy, c);
}
static void ellipse(uint16_t *p, int x, int y, int rx, int ry, uint16_t c)
{
    for (int dy = -ry; dy <= ry; dy++) for (int dx = -rx; dx <= rx; dx++)
        if (dx*dx*ry*ry + dy*dy*rx*rx <= rx*rx*ry*ry) pixel(p, x+dx, y+dy, c);
}
static const arcs_mini_glyph_t *glyph(uint32_t cp)
{
    for (size_t i = 0; i < sizeof(arcs_mini_glyphs)/sizeof(arcs_mini_glyphs[0]); i++)
        if (arcs_mini_glyphs[i].codepoint == cp) return &arcs_mini_glyphs[i];
    return NULL;
}
static uint32_t next(const unsigned char **s)
{
    unsigned char c = *(*s)++;
    if (c < 128) return c;
    uint32_t cp = c & (c < 224 ? 31u : 15u);
    unsigned n = c < 224 ? 1 : 2;
    while (n--) cp = (cp << 6) | (*(*s)++ & 63u);
    return cp;
}
static void text(uint16_t *p, const char *str, int y, uint16_t color)
{
    const unsigned char *s = (const unsigned char *)str;
    int width = 0;
    while (*s) { const arcs_mini_glyph_t *g = glyph(next(&s)); width += g ? g->advance : 24; }
    int x = (ARCS_MINI_SCREEN_WIDTH-width)/2;
    s = (const unsigned char *)str;
    while (*s) {
        const arcs_mini_glyph_t *g = glyph(next(&s));
        if (g) {
            for (int yy = 0; yy < 24; yy++) for (int xx = 0; xx < 24; xx++)
                if (g->bits[yy*3+xx/8] & (0x80u >> (xx%8)))
                    pixel(p, x+xx-(24-g->advance)/2, y+yy, color);
            x += g->advance;
        } else x += 24;
    }
}

bool arcs_mini_screen_render(uint16_t *p, unsigned width, unsigned height,
                          gadget_status_t state, uint32_t ms, bool test, bool wake_available, bool dialogue_active)
{
    if (!p || width != ARCS_MINI_SCREEN_WIDTH || height != ARCS_MINI_SCREEN_HEIGHT) return false;
    for (unsigned i = 0; i < width*height; i++) p[i] = BG;
    if (test) {
        const uint16_t colors[] = {0xf800, 0x07e0, 0x001f, 0xffff};
        for (int i = 0; i < 4; i++) rect(p, i*60, 60, 60, 100, colors[i]);
        text(p, IMPO_SCREEN_TITLE, 15, WHITE); text(p, "IMPO", 190, WHITE);
        rect(p, 0, 0, 240, 2, WHITE); rect(p, 0, 238, 240, 2, WHITE);
        rect(p, 0, 0, 2, 240, WHITE); rect(p, 238, 0, 2, 240, WHITE);
        return true;
    }
    static const char *const labels[] = {
        "启动中", "等待配对", "连接中", "待机", "我在听", "思考中", "正在回答",
        "网络断开", "电量低", "充电中", "更新中"
    };
    unsigned st = (unsigned)state < sizeof(labels)/sizeof(labels[0]) ? (unsigned)state : 0;
    bool warn = st == GADGET_STATUS_UNPAIRED || st == GADGET_STATUS_CONNECTING ||
                st == GADGET_STATUS_OFFLINE || st == GADGET_STATUS_LOW_BATTERY;
    uint16_t accent = warn ? AMBER : st == GADGET_STATUS_SPEAKING ? GREEN : CYAN;
    bool blink = ms % 5000 >= 4800;
    int look = st == GADGET_STATUS_THINKING ? ((int)((ms/500)%3)-1)*5 : 0;
    text(p, IMPO_SCREEN_TITLE, 14, WHITE);
    for (int eye = 0; eye < 2; eye++) {
        int x = eye ? 165 : 75;
        if (blink || st == GADGET_STATUS_OFFLINE) {
            rect(p, x-20, 105, 40, 6, accent);
        } else {
            int ry = st == GADGET_STATUS_LISTENING ? 33 : 28;
            ellipse(p, x+look, 105, 21, ry, accent);
            ellipse(p, x+look-6, 94, 5, 7, WHITE);
        }
    }
    if (st == GADGET_STATUS_THINKING || st == GADGET_STATUS_CONNECTING) {
        for (int i = 0; i < 3; i++) ellipse(p, 102+i*18, 151, 3, 3, (ms/400)%3 == (unsigned)i ? WHITE : MUTED);
    } else if (st == GADGET_STATUS_LISTENING || st == GADGET_STATUS_SPEAKING) {
        for (int i = 0; i < 9; i++) {
            int h = 3 + (i*7+(int)(ms/250)*3)%10;
            rect(p, 85+i*8, 152-h/2, 4, h, accent);
        }
    } else {
        rect(p, 106, 149, 28, 3, accent); rect(p, 101, 144, 4, 5, accent); rect(p, 135, 144, 4, 5, accent);
    }
    text(p, labels[st], 169, WHITE);
    rect(p, 48, 202, 144, 1, RGB(32,52,66));
    text(p, dialogue_active ? "短按结束" : wake_available ? IMPO_WAKE_LABEL : "短按说话", 210, MUTED);
    return true;
}
