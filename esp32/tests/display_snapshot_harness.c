// SPDX-License-Identifier: Apache-2.0
#include "display_snapshot.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned char flash[DISPLAY_SNAPSHOT_STORAGE_BYTES];
static unsigned char old_flash[sizeof(flash)], a[48000], b[48000], output[48000];
static int mutations, fail_at;
static bool read_error;
static bool read_flash(size_t offset, void *data, size_t n) {
    assert(offset <= sizeof(flash) && n <= sizeof(flash) - offset);
    if (read_error) return false;
    memcpy(data, flash + offset, n);
    return true;
}
static bool erase_flash(size_t offset, size_t n) {
    assert(offset % 4096 == 0 && n % 4096 == 0 && offset + n <= sizeof(flash));
    bool fail = ++mutations == fail_at;
    memset(flash + offset, 255, fail ? n / 2 : n);
    return !fail;
}
static bool write_flash(size_t offset, const void *data, size_t n) {
    assert(offset + n <= sizeof(flash));
    bool fail = ++mutations == fail_at;
    const unsigned char *p = data;
    if (fail) n /= 2;
    for (size_t i = 0; i < n; i++) {
        assert((flash[offset + i] & p[i]) == p[i]);
        flash[offset + i] &= p[i];
    }
    return !fail;
}
static display_snapshot_io_t io = {read_flash, erase_flash, write_flash};
int main(void) {
    display_snapshot_t s;
    memset(flash, 255, sizeof(flash));
    for (size_t i = 0; i < sizeof(a); i++) { a[i] = (unsigned char)i; b[i] = (unsigned char)~i; }
    assert(display_snapshot_open(&s, io, sizeof(a), 2) && s.active == -1);
    assert(!display_snapshot_load(&s, output));
    assert(display_snapshot_save(&s, a));
    assert(display_snapshot_open(&s, io, sizeof(a), 2) && s.active == 0);
    assert(display_snapshot_load(&s, output) && !memcmp(a, output, sizeof(a)));
    int before = mutations;
    assert(display_snapshot_save(&s, a) && mutations == before);
    memcpy(old_flash, flash, sizeof(flash));
    // Power loss during erase, pixel write or final header write must retain A.
    for (int cut = 1; cut <= 3; cut++) {
        memcpy(flash, old_flash, sizeof(flash));
        assert(display_snapshot_open(&s, io, sizeof(a), 2));
        mutations = 0; fail_at = cut;
        assert(!display_snapshot_save(&s, b));
        fail_at = 0;
        assert(display_snapshot_open(&s, io, sizeof(a), 2));
        assert(display_snapshot_load(&s, output) && !memcmp(a, output, sizeof(a)));
    }
    assert(display_snapshot_save(&s, b));
    assert(display_snapshot_open(&s, io, sizeof(a), 2) && s.active == 1);
    assert(display_snapshot_load(&s, output) && !memcmp(b, output, sizeof(b)));
    // Corrupted pixels invalidate the newer slot, even with an intact header.
    flash[DISPLAY_SNAPSHOT_SLOT_BYTES + 100] ^= 1;
    assert(display_snapshot_open(&s, io, sizeof(a), 2) && s.active == 0);
    assert(display_snapshot_load(&s, output) && !memcmp(a, output, sizeof(a)));
    assert(display_snapshot_save(&s, NULL));
    assert(display_snapshot_open(&s, io, sizeof(a), 2) && s.length == 0);
    assert(!display_snapshot_load(&s, output));
    before = mutations;
    assert(display_snapshot_save(&s, NULL) && mutations == before);
    assert(display_snapshot_save(&s, b));
    assert(display_snapshot_open(&s, io, sizeof(a), 2));
    assert(display_snapshot_load(&s, output) && !memcmp(b, output, sizeof(b)));
    assert(display_snapshot_open(&s, io, sizeof(a) - 1, 2) && s.length == 0);
    assert(!display_snapshot_load(&s, output));
    // X4 uses one 64 KiB data slot. A partial update must safely fall back
    // to the default, never display partially written pixels.
    memset(flash, 255, sizeof(flash));
    assert(display_snapshot_open(&s, io, sizeof(a), 1));
    assert(display_snapshot_save(&s, a));
    memcpy(old_flash, flash, sizeof(flash));
    for (int cut = 1; cut <= 3; cut++) {
        memcpy(flash, old_flash, sizeof(flash));
        assert(display_snapshot_open(&s, io, sizeof(a), 1));
        mutations = 0; fail_at = cut;
        assert(!display_snapshot_save(&s, b));
        assert(!display_snapshot_load(&s, output));
        fail_at = 0;
        assert(display_snapshot_open(&s, io, sizeof(a), 1));
        assert(!display_snapshot_load(&s, output));
        assert(display_snapshot_save(&s, b));
        assert(display_snapshot_open(&s, io, sizeof(a), 1));
        assert(display_snapshot_load(&s, output) && !memcmp(b, output, sizeof(b)));
    }
    assert(display_snapshot_save(&s, NULL));
    assert(display_snapshot_open(&s, io, sizeof(a), 1) && s.length == 0);
    read_error = true;
    assert(!display_snapshot_open(&s, io, sizeof(a), 2));
    assert(!display_snapshot_open(&s, io, DISPLAY_SNAPSHOT_SLOT_BYTES, 2));
    puts("snapshot: persistence, partial writes, CRC fallback, reset and wear avoidance passed");
}
