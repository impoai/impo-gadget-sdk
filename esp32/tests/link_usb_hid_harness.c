// SPDX-License-Identifier: MIT
// Host harness for usb_hid.c's text-to-keys and move-splitting functions,
// included from usb_hid_reports.inc (the "host-tested" section).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "usb_hid_reports.inc"

static int failures;

#define CHECK(cond, ...)                                  \
    do {                                                  \
        if (!(cond)) {                                    \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                 \
            fprintf(stderr, "\n");                        \
            failures++;                                   \
        }                                                 \
    } while (0)

static void expect_key(char ch, uint8_t modifier, uint8_t keycode) {
    uint8_t m = 0xff, k = 0xff;
    int r = usb_hid_key_for_char(ch, &m, &k);
    CHECK(r == 1 && m == modifier && k == keycode,
          "'%c' (0x%02x): got r=%d mod=0x%02x key=0x%02x, want mod=0x%02x key=0x%02x",
          ch, (unsigned char)ch, r, m, k, modifier, keycode);
}

static void test_known_keys(void) {
    const uint8_t S = USB_HID_MOD_LEFT_SHIFT;
    expect_key('a', 0, 0x04);
    expect_key('z', 0, 0x1D);
    expect_key('A', S, 0x04);
    expect_key('Z', S, 0x1D);
    expect_key('1', 0, 0x1E);
    expect_key('9', 0, 0x26);
    expect_key('0', 0, 0x27);
    expect_key('!', S, 0x1E);
    expect_key('@', S, 0x1F);
    expect_key('(', S, 0x26);
    expect_key(')', S, 0x27);
    expect_key('\n', 0, 0x28);
    expect_key('\t', 0, 0x2B);
    expect_key(' ', 0, 0x2C);
    expect_key('-', 0, 0x2D);
    expect_key('_', S, 0x2D);
    expect_key('\\', 0, 0x31);
    expect_key('|', S, 0x31);
    expect_key('\'', 0, 0x34);
    expect_key('"', S, 0x34);
    expect_key('`', 0, 0x35);
    expect_key('~', S, 0x35);
    expect_key('/', 0, 0x38);
    expect_key('?', S, 0x38);
}

static void test_skipped_and_untypable(void) {
    uint8_t m, k;
    CHECK(usb_hid_key_for_char('\r', &m, &k) == 0, "'\\r' should be skipped");
    CHECK(usb_hid_key_for_char('\x7f', &m, &k) == -1, "DEL should be untypable");
    CHECK(usb_hid_key_for_char('\x01', &m, &k) == -1, "control characters should be untypable");
    CHECK(usb_hid_key_for_char((char)0xe4, &m, &k) == -1, "UTF-8 bytes should be untypable");
    CHECK(usb_hid_untypable_at("hello") == -1, "ASCII text is typable");
    CHECK(usb_hid_untypable_at("line one\r\nline two\tend") == -1, "CR, LF and Tab are fine");
    CHECK(usb_hid_untypable_at("hi \xe4\xbd\xa0") == 3, "first untypable byte is at 3");
}

// Every printable ASCII character has a key, and no two share one.
static void test_printable_ascii_is_one_to_one(void) {
    int seen[256][2];
    memset(seen, 0, sizeof(seen));
    for (int c = 0x20; c <= 0x7e; c++) {
        uint8_t m, k;
        int r = usb_hid_key_for_char((char)c, &m, &k);
        CHECK(r == 1, "'%c' has no key", c);
        if (r != 1) continue;
        int shifted = m == USB_HID_MOD_LEFT_SHIFT;
        CHECK(m == 0 || shifted, "'%c' uses modifier 0x%02x", c, m);
        CHECK(!seen[k][shifted], "'%c' shares key 0x%02x with '%c'", c, k, seen[k][shifted]);
        seen[k][shifted] = c;
    }
}

static void test_moves_add_up(void) {
    CHECK(usb_hid_move_steps(0, 0, 10) == 0, "no move takes no reports");
    CHECK(usb_hid_move_steps(10, -3, 10) == 1, "a 10-unit move is one report");
    CHECK(usb_hid_move_steps(-11, 0, 10) == 2, "an 11-unit move is two reports");
    const int totals[] = {0, 1, -1, 7, 9, 10, 11, -25, 99, 100, 101, -333, 1234, -2999, 3000, -3000};
    const int n = sizeof(totals) / sizeof(totals[0]);
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            int dx = totals[i], dy = totals[j];
            int steps = usb_hid_move_steps(dx, dy, 10);
            int sx = 0, sy = 0;
            for (int step = 1; step <= steps; step++) {
                int x = usb_hid_move_delta(dx, step, steps);
                int y = usb_hid_move_delta(dy, step, steps);
                CHECK(abs(x) <= 10 && abs(y) <= 10, "(%d,%d) step %d is (%d,%d)", dx, dy, step, x, y);
                sx += x;
                sy += y;
            }
            CHECK(sx == dx && sy == dy, "(%d,%d) moved (%d,%d)", dx, dy, sx, sy);
        }
    }
}

int main(void) {
    test_known_keys();
    test_skipped_and_untypable();
    test_printable_ascii_is_one_to_one();
    test_moves_add_up();
    if (failures) {
        fprintf(stderr, "%d failures\n", failures);
        return 1;
    }
    printf("usb_hid reports passed\n");
    return 0;
}
