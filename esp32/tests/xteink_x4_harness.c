// SPDX-License-Identifier: Apache-2.0
#include "xteink_x4_pixels.h"
#include <assert.h>
#include <limits.h>
#include <string.h>

static uint8_t frame[X4_FRAME_BYTES], split[X4_FRAME_BYTES];
static uint8_t rgb[16 * 16 * 2];

int main(void) {
    // Each portrait pixel maps to a distinct native panel bit; fill every bit.
    memset(frame, 0xff, sizeof(frame));
    for (int y = 0; y < X4_HEIGHT; ++y)
        for (int x = 0; x < X4_WIDTH; ++x) x4_pixel(frame, x, y, false);
    for (size_t i = 0; i < sizeof(frame); ++i) assert(frame[i] == 0);
    x4_pixel(frame, 0, 0, true);
    x4_pixel(frame, 479, 799, true);
    assert(frame[479 * 100] == 0x80);
    assert(frame[99] == 1);
    memcpy(split, frame, sizeof(frame));
    x4_pixel(frame, -1, 0, true);
    x4_pixel(frame, 480, 800, true);
    assert(memcmp(split, frame, sizeof(frame)) == 0);

    // RGB565 is big-endian. Pure black/white are exact, independent of Bayer phase.
    for (int gray = 0; gray <= 1; ++gray) {
        memset(rgb, gray ? 0xff : 0, sizeof(rgb));
        memset(frame, gray ? 0 : 0xff, sizeof(frame));
        assert(x4_draw_rgb565(frame, 0, 0, 16, 16, rgb));
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 16; ++x) {
                size_t offset = (479 - x) * 100 + y / 8;
                assert(!!(frame[offset] & (0x80 >> (y & 7))) == gray);
            }
    }
    // Decoding in MCU/strip chunks must not change the dither or rotation.
    for (size_t i = 0; i < sizeof(rgb); ++i) rgb[i] = (uint8_t)(i * 37);
    memset(frame, 0xff, sizeof(frame));
    memset(split, 0xff, sizeof(split));
    assert(x4_draw_rgb565(frame, 17, 29, 16, 16, rgb));
    for (int row = 0; row < 16; ++row)
        assert(x4_draw_rgb565(split, 17, 29 + row, 16, 1, rgb + row * 32));
    assert(memcmp(frame, split, sizeof(frame)) == 0);
    assert(!x4_draw_rgb565(frame, -1, 0, 1, 1, rgb));
    assert(!x4_draw_rgb565(frame, 479, 799, 2, 1, rgb));
    assert(!x4_draw_rgb565(frame, 0, 0, INT_MAX, INT_MAX, rgb));
    assert(!x4_draw_rgb565(frame, 0, 0, 480, 24, rgb));
    assert(!x4_draw_rgb565(frame, 0, 0, 1, 1, NULL));
    assert(memcmp(frame, split, sizeof(frame)) == 0);

    assert(!x4_confirm_raw(-1) && !x4_confirm_raw(5)); // Right
    assert(!x4_confirm_raw(1493)); // Left
    assert(!x4_confirm_raw(2090));
    assert(x4_confirm_raw(2091) && x4_confirm_raw(2694) && x4_confirm_raw(3100));
    assert(!x4_confirm_raw(3101) && !x4_confirm_raw(3512)); // Back
    assert(!x4_confirm_raw(4095)); // Released
    return 0;
}
