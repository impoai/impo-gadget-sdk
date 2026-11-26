/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    bool enabled;
    const char *label;
    const char *const *keywords;
    size_t keyword_count;
    const char *model_file, *wrap_file;
    uint8_t model_sha256[32], wrap_sha256[32];
} impo_arcs_wake_config_t;
typedef struct {
    uint32_t model_offset, model_size, wrap_offset, wrap_size;
} impo_arcs_wake_resources_t;
/* Reads the existing ROMFS only. Requires exact resource hashes, compatible
 * model header and one-mic wrap; never installs or generates acoustic models. */
bool impo_arcs_wake_resources(const uint8_t *fs, size_t capacity,
    const impo_arcs_wake_config_t *config, impo_arcs_wake_resources_t *out);
bool impo_arcs_wake_result(const char *json, const impo_arcs_wake_config_t *config);
#ifdef __cplusplus
}
#endif
