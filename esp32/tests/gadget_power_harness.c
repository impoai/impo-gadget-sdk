// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "gadget_power.h"

static char events[32];
static unsigned used, reads, delays;
static esp_err_t prepare_result, hardware_result;
static bool reenter;
static const gadget_power_ops_t ops;

static void record(char event) {
    assert(gadget_power_is_shutting_down());
    assert(used + 1 < sizeof(events));
    events[used++] = event;
    events[used] = 0;
}
static esp_err_t prepare(void) {
    record('P');
    if (reenter) assert(gadget_power_off(&ops) == ESP_ERR_INVALID_STATE);
    return prepare_result;
}
static void show(void) { record('S'); }
static bool pressed(void) {
    // A bounce after the first release must restart the release debounce.
    const bool sequence[] = {true, false, true, false, false};
    assert(reads < sizeof(sequence) / sizeof(sequence[0]));
    record('B');
    return sequence[reads++];
}
void vTaskDelay(int ticks) {
    assert(ticks == 50);
    delays++;
}
static esp_err_t hardware(void) {
    assert(reads == 5 && delays == 5);
    record('H');
    return hardware_result;
}
static void recover(void) { record('R'); }
static const gadget_power_ops_t ops = {
    .show_off = show, .button_pressed = pressed, .power_off = hardware,
};

static void reset(void) {
    assert(!gadget_power_is_shutting_down());
    used = reads = delays = 0;
    events[0] = 0;
    prepare_result = ESP_OK;
    hardware_result = ESP_FAIL;
    reenter = false;
}

static void test_gesture(void) {
    gadget_power_button_t button = {0};
    // Holding the wake key through boot must not shut the device off again.
    assert(!gadget_power_button_update(&button, true, 0));
    assert(!gadget_power_button_update(&button, true, 10000));
    assert(!gadget_power_button_update(&button, false, 10050));
    assert(!gadget_power_button_update(&button, true, 10100));
    assert(!gadget_power_button_update(&button, true, 11599));
    assert(gadget_power_button_update(&button, true, 11600));
    assert(!gadget_power_button_update(&button, true, 20000));
    assert(!gadget_power_button_update(&button, false, 20100));
    // Separate short presses cannot accumulate into a long press.
    assert(!gadget_power_button_update(&button, true, 20200));
    assert(!gadget_power_button_update(&button, false, 21200));
    assert(!gadget_power_button_update(&button, true, 21300));
    assert(!gadget_power_button_update(&button, true, 22300));
    assert(gadget_power_button_update(&button, true, 22800));
}

int main(void) {
    test_gesture();
    gadget_power_set_system_hooks(prepare, recover);
    assert(gadget_power_off(NULL) == ESP_ERR_INVALID_ARG);
    const gadget_power_ops_t invalid = {0};
    assert(gadget_power_off(&invalid) == ESP_ERR_INVALID_ARG);

    reset();
    prepare_result = ESP_ERR_TIMEOUT;
    assert(gadget_power_off(&ops) == ESP_ERR_TIMEOUT);
    assert(strcmp(events, "P") == 0); // No display/hardware mutation if busy.
    reset();
    reenter = true;
    assert(gadget_power_off(&ops) == ESP_FAIL);
    assert(strcmp(events, "PSBBBBBHR") == 0);
    reset();
    hardware_result = ESP_OK; // Returning cannot represent successful shutdown.
    assert(gadget_power_off(&ops) == ESP_FAIL);
    assert(strcmp(events, "PSBBBBBHR") == 0);
    reset();
    hardware_result = ESP_ERR_INVALID_STATE;
    assert(gadget_power_off(&ops) == ESP_ERR_INVALID_STATE);
    assert(strcmp(events, "PSBBBBBHR") == 0);
    reset();
    puts("power gestures, shutdown order, release debounce and recovery passed");
    return 0;
}
