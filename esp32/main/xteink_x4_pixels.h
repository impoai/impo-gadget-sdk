// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Portrait API, landscape SSD1677 RAM. One bit per pixel, 1 = white.
#define X4_WIDTH 480
#define X4_HEIGHT 800
#define X4_FRAME_BYTES (X4_WIDTH * X4_HEIGHT / 8)

static inline void x4_pixel(uint8_t *frame, int x, int y, bool white) {
    if (x < 0 || x >= X4_WIDTH || y < 0 || y >= X4_HEIGHT) return;
    const int px = y, py = X4_WIDTH - 1 - x;
    const size_t offset = (size_t)py * (X4_HEIGHT / 8) + px / 8;
    const uint8_t bit = 0x80u >> (px & 7);
    if (white) frame[offset] |= bit;
    else frame[offset] &= (uint8_t)~bit;
}

static inline bool x4_confirm_raw(int raw) {
    // FreeInk InputManager's GPIO1 ladder: Back, Confirm, Left, Right.
    return raw > 2090 && raw <= 3100;
}

static inline bool x4_draw_rgb565(uint8_t *frame, int x, int y, int w, int h,
                                 const uint8_t *pixels) {
    if (!frame || !pixels || x < 0 || y < 0 || w <= 0 || h <= 0 ||
        x >= X4_WIDTH || y >= X4_HEIGHT || w > X4_WIDTH - x ||
        h > X4_HEIGHT - y || (size_t)w * h > X4_WIDTH * 23) return false;
    static const uint8_t bayer[4][4] = {
        {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}
    };
    for (int row = 0; row < h; ++row) {
        for (int col = 0; col < w; ++col) {
            uint16_t p = ((uint16_t)pixels[0] << 8) | pixels[1];
            pixels += 2;
            unsigned r = ((p >> 11) & 31) * 255 / 31;
            unsigned g = ((p >> 5) & 63) * 255 / 63;
            unsigned b = (p & 31) * 255 / 31;
            unsigned luma = (77 * r + 150 * g + 29 * b) >> 8;
            unsigned threshold = bayer[(y + row) & 3][(x + col) & 3] * 16 + 8;
            x4_pixel(frame, x + col, y + row, luma > threshold);
        }
    }
    return true;
}
