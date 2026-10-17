// SPDX-License-Identifier: Apache-2.0
#include "display_snapshot.h"
#include <string.h>

// This on-flash format is little-endian (ESP32 and the supported host tests).
typedef struct {
    uint32_t magic, version, sequence, length, checksum, header_checksum;
} header_t;
_Static_assert(sizeof(header_t) == 24, "snapshot header layout");
#define MAGIC 0x34464f49u // IOF4

static uint32_t crc(uint32_t state, const void *data, size_t n) {
    const uint8_t *p = data;
    while (n--) {
        state ^= *p++;
        for (int bit = 0; bit < 8; bit++)
            state = (state >> 1) ^ (0xedb88320u & (0u - (state & 1u)));
    }
    return state;
}
static uint32_t checksum(const void *data, size_t n) { return ~crc(UINT32_MAX, data, n); }
static bool verify_pixels(display_snapshot_t *s, int slot, const header_t *h) {
    uint8_t buffer[512];
    uint32_t state = UINT32_MAX;
    for (size_t pos = 0; pos < h->length;) {
        size_t n = h->length - pos;
        if (n > sizeof(buffer)) n = sizeof(buffer);
        if (!s->io.read(slot * DISPLAY_SNAPSHOT_SLOT_BYTES + sizeof(*h) + pos, buffer, n)) return false;
        state = crc(state, buffer, n);
        pos += n;
    }
    return ~state == h->checksum;
}
static bool valid_header(display_snapshot_t *s, const header_t *h) {
    return h->magic == MAGIC && h->version == 1 && h->sequence &&
        (h->length == 0 || h->length == s->frame_bytes) &&
        checksum(h, offsetof(header_t, header_checksum)) == h->header_checksum;
}
bool display_snapshot_open(display_snapshot_t *s, display_snapshot_io_t io, size_t frame_bytes, unsigned slots) {
    if (!s || !io.read || !io.erase || !io.write || !frame_bytes ||
        !slots || slots > 2 || frame_bytes > DISPLAY_SNAPSHOT_SLOT_BYTES - sizeof(header_t)) return false;
    *s = (display_snapshot_t){.io = io, .frame_bytes = frame_bytes, .slots = slots, .active = -1};
    for (unsigned slot = 0; slot < slots; slot++) {
        header_t h;
        if (!io.read(slot * DISPLAY_SNAPSHOT_SLOT_BYTES, &h, sizeof(h))) return false;
        if (valid_header(s, &h) && h.sequence > s->sequence && verify_pixels(s, slot, &h)) {
            s->active = slot;
            s->sequence = h.sequence;
            s->length = h.length;
            s->checksum = h.checksum;
        }
    }
    return true;
}
bool display_snapshot_save(display_snapshot_t *s, const void *frame) {
    if (!s || !s->io.write || s->sequence == UINT32_MAX) return false;
    header_t h = {.magic = MAGIC, .version = 1, .sequence = s->sequence + 1,
                  .length = frame ? (uint32_t)s->frame_bytes : 0};
    h.checksum = checksum(frame, h.length);
    h.header_checksum = checksum(&h, offsetof(header_t, header_checksum));
    // Avoid flash wear for an unchanged picture or repeated reset.
    if (s->active >= 0 && s->length == h.length && s->checksum == h.checksum) return true;
    int slot = s->slots == 2 && s->active == 0 ? 1 : 0;
    size_t offset = slot * DISPLAY_SNAPSHOT_SLOT_BYTES;
    if (s->slots == 1) { s->active = -1; s->length = 0; }
    if (!s->io.erase(offset, DISPLAY_SNAPSHOT_SLOT_BYTES)) return false;
    if (h.length && !s->io.write(offset + sizeof(h), frame, h.length)) return false;
    if (!verify_pixels(s, slot, &h) || !s->io.write(offset, &h, sizeof(h))) return false;
    header_t readback;
    if (!s->io.read(offset, &readback, sizeof(readback)) || memcmp(&readback, &h, sizeof(h))) return false;
    s->active = slot;
    s->sequence = h.sequence;
    s->length = h.length;
    s->checksum = h.checksum;
    return true;
}
bool display_snapshot_load(display_snapshot_t *s, void *frame) {
    if (!s || !frame || s->active < 0 || s->length != s->frame_bytes) return false;
    return s->io.read(s->active * DISPLAY_SNAPSHOT_SLOT_BYTES + sizeof(header_t), frame, s->length) &&
        checksum(frame, s->length) == s->checksum;
}
