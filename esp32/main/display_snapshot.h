// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// One or two independently erasable slots. Commit the header after verifying
// the pixels. Two slots retain the previous snapshot on interrupted updates;
// one slot falls back to the default page if its update is interrupted.
#define DISPLAY_SNAPSHOT_SLOT_BYTES 65536u
#define DISPLAY_SNAPSHOT_STORAGE_BYTES (2u * DISPLAY_SNAPSHOT_SLOT_BYTES)
typedef struct {
    bool (*read)(size_t offset, void *data, size_t len);
    bool (*erase)(size_t offset, size_t len);
    bool (*write)(size_t offset, const void *data, size_t len);
} display_snapshot_io_t;
typedef struct {
    display_snapshot_io_t io;
    size_t frame_bytes;
    unsigned slots;
    int active;
    uint32_t sequence, length, checksum;
} display_snapshot_t;
bool display_snapshot_open(display_snapshot_t *store, display_snapshot_io_t io, size_t frame_bytes, unsigned slots);
// NULL persists a reset to the default page. Callers serialize all operations.
bool display_snapshot_save(display_snapshot_t *store, const void *frame);
bool display_snapshot_load(display_snapshot_t *store, void *frame);
