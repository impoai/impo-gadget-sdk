# SPDX-License-Identifier: Apache-2.0
"""The SparkBot's tracked-base commands and USB diagnostics, with a simulated UART and clock.

The production code between the base's first comment and the command table is
compiled against stubs: the UART records what was written and echoes it back
on the next tick, as the base's firmware does; the clock is advanced by the
test, and the base task's tick is run by hand.
"""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
# CI supplies pinned upstream sources; local IDF builds already have cJSON.
JSON = Path(os.environ.get(
    "CJSON_SOURCE_DIR", ROOT / "managed_components/espressif__cjson/cJSON"
))


class SparkbotBase(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        source = (ROOT / "components/impo/boards/board_espressif_sparkbot.c").read_text()
        start = source.index("/* With s_base_lock held: notes any echo")
        end = source.index("\nstatic const impo_command_t s_commands[]", start)
        production = source[start:end]
        harness = r"""
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include "cJSON.h"
typedef unsigned TickType_t;
typedef int esp_err_t;
#define BASE_UART 1
#define BASE_TICK_MS 200
#define BASE_STEP_MS 500
#define BASE_MAX_MS 10000
#define ESP_OK 0
#define ESP_ERR_NOT_FOUND 0x105
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
static int s_base_lock = 1, locked, echo = 1, writes;
static unsigned now, write_time[64];
static char commands[64][32];
static size_t pending;   /* echo bytes waiting in the UART, from the tick after they were sent */
static unsigned pending_at;
static bool s_imu_ok;
typedef struct { float accel[3], gyro[3]; const char *orientation; bool moving; } sparkbot_imu_reading_t;
static esp_err_t sparkbot_imu_read(sparkbot_imu_reading_t *r) { (void)r; return ESP_ERR_NOT_FOUND; }
static unsigned sparkbot_touch_state(void) { return 0; }
static void console_snap(void) { printf("@snap {\"error\":\"no camera here\"}\n"); }
static const char *esp_err_to_name(esp_err_t e) { (void)e; return "ESP_ERR_NOT_FOUND"; }
static int xSemaphoreTake(int lock, int wait) {
    (void)lock; (void)wait;
    if (locked) return 0;
    locked = 1;
    return 1;
}
static void xSemaphoreGive(int lock) { (void)lock; locked = 0; }
static unsigned xTaskGetTickCount(void) { return now; }
static void vTaskDelay(unsigned ticks) { now += ticks; }
static int uart_get_buffered_data_len(int uart, size_t *n) { (void)uart; *n = now > pending_at ? pending : 0; return ESP_OK; }
static int uart_flush_input(int uart) { (void)uart; pending = 0; return ESP_OK; }
static int uart_write_bytes(int uart, const char *text, size_t len) {
    (void)uart;
    assert(locked && writes < 64 && len < sizeof(commands[0]));
    memcpy(commands[writes], text, len);
    write_time[writes++] = now;
    if (echo) { pending += len; pending_at = now; }
    return (int)len;
}
static struct {
    float x, y;
    TickType_t until;
    bool moving;
    bool ever_sent;
    TickType_t asked;
    bool answered;
} s_base;
static cJSON *impo_command_ok(void) { cJSON *r = cJSON_CreateObject(); cJSON_AddBoolToObject(r, "ok", true); return r; }
static cJSON *impo_command_error(const char *code, const char *message) {
    cJSON *r = cJSON_CreateObject(); cJSON_AddBoolToObject(r, "ok", false);
    cJSON *e = cJSON_AddObjectToObject(r, "error");
    cJSON_AddStringToObject(e, "code", code); cJSON_AddStringToObject(e, "message", message); return r;
}
""" + production + r"""
/* One tick of base_task, without its delay. */
static void tick(void) {
    now += BASE_TICK_MS;
    if (xSemaphoreTake(s_base_lock, 50) != pdTRUE) return;
    if ((int32_t)(s_base.until - now) > 0) base_write_drive(s_base.x, s_base.y);
    else if (s_base.moving) { base_write_drive(0, 0); s_base.moving = false; }
    xSemaphoreGive(s_base_lock);
}
static void report(cJSON *result) {
    char *text = result ? cJSON_PrintUnformatted(result) : NULL;
    printf("%s\n", text ? text : "null");
    free(text);
    cJSON_Delete(result);
}
/* argv: a script of steps; each is "cmd:<name>:<json params>", "tick", "console:<line>", "noecho", "lock". */
int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        char *step = argv[i];
        if (!strcmp(step, "tick")) { tick(); continue; }
        if (!strcmp(step, "noecho")) { echo = 0; continue; }
        if (!strcmp(step, "lock")) { locked = 1; continue; }
        if (!strncmp(step, "console:", 8)) {
            bool handled = impo_sparkbot_console(step + 8, true);
            printf("{\"handled\":%s}\n", handled ? "true" : "false");
            continue;
        }
        assert(!strncmp(step, "cmd:", 4));
        char *name = step + 4, *json = strchr(name, ':');
        assert(json); *json++ = 0;
        cJSON *params = cJSON_Parse(json);
        assert(params);
        cJSON *r = !strcmp(name, "drive") ? chassis_drive(params) : !strcmp(name, "move") ? chassis_move(params)
                 : !strcmp(name, "stop") ? chassis_stop(params) : !strcmp(name, "dance") ? chassis_dance(params)
                 : !strcmp(name, "light") ? chassis_light(params) : !strcmp(name, "status") ? chassis_status(params)
                 : !strcmp(name, "imu") ? imu_read(params) : NULL;
        cJSON_Delete(params);
        report(r);
    }
    fprintf(stderr, "{\"writes\":[");
    for (int i = 0; i < writes; i++) fprintf(stderr, "%s[\"%s\",%u]", i ? "," : "", commands[i], write_time[i]);
    fprintf(stderr, "],\"locked\":%d,\"moving\":%s}\n", locked, s_base.moving ? "true" : "false");
    return 0;
}
"""
        c = Path(cls.tmp.name) / "test.c"
        c.write_text(harness)
        cls.binary = Path(cls.tmp.name) / "test"
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                        "-I", str(JSON), str(c), str(JSON / "cJSON.c"), "-lm",
                        "-o", str(cls.binary)], check=True, capture_output=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_steps(self, *steps):
        result = subprocess.run([str(self.binary), *steps], capture_output=True, text=True, check=True)
        outputs = [json.loads(line.removeprefix("@chassis ").removeprefix("@imu ").removeprefix("@touch "))
                   for line in result.stdout.splitlines()]
        return outputs, json.loads(result.stderr)

    def test_move_repeats_until_its_time_then_stops(self):
        out, io = self.run_steps("cmd:move:{\"direction\":\"forward\"}", "tick", "tick", "tick", "tick")
        self.assertTrue(out[0]["ok"])
        self.assertEqual(out[0]["base"], "unknown")   # nothing could have echoed yet
        writes = [w for w, _ in io["writes"]]
        self.assertEqual(writes, ["x0.00 y1.00", "x0.00 y1.00", "x0.00 y1.00", "x0.00 y0.00"])
        self.assertEqual(io["writes"][3][1], 600)
        self.assertFalse(io["moving"])
        self.assertEqual(io["locked"], 0)

    def test_drive_takes_axes_and_duration(self):
        out, io = self.run_steps("cmd:drive:{\"forward\":0.5,\"turn\":-0.25,\"duration_ms\":1000}",
                                 *["tick"] * 6)
        self.assertTrue(out[0]["ok"])
        self.assertEqual(io["writes"][0][0], "x-0.25 y0.50")
        self.assertEqual([w for w, _ in io["writes"]].count("x-0.25 y0.50"), 5)   # 0, 200, 400, 600, 800
        self.assertEqual(io["writes"][-1], ["x0.00 y0.00", 1000])

    def test_stop_ends_a_drive_at_once(self):
        out, io = self.run_steps("cmd:move:{\"direction\":\"forward\",\"duration_ms\":5000}", "tick",
                                 "cmd:stop:{}", "tick", "tick")
        self.assertEqual([w for w, _ in io["writes"]], ["x0.00 y1.00", "x0.00 y1.00", "x0.00 y0.00"])
        self.assertFalse(out[1]["moving"])

    def test_echo_marks_the_base_responding_and_silence_marks_it_silent(self):
        out, _ = self.run_steps("cmd:status:{}", "cmd:dance:{}", "tick", "cmd:status:{}", *["tick"] * 20,
                                "cmd:status:{}")
        self.assertEqual(out[0]["base"], "unknown")      # never spoken to
        self.assertEqual(out[1]["base"], "unknown")      # the echo isn't due yet
        self.assertEqual(out[2]["base"], "responding")   # it came
        self.assertEqual(out[3]["base"], "responding")   # and idle it stays known
        out, _ = self.run_steps("noecho", "cmd:move:{\"direction\":\"left\"}", "tick", "cmd:status:{}")
        self.assertEqual(out[1]["base"], "silent")
        out, _ = self.run_steps("cmd:dance:{}", "tick", "cmd:status:{}", "noecho",
                                "cmd:move:{\"direction\":\"left\"}", "tick", "tick", "cmd:status:{}")
        self.assertEqual(out[0]["base"], "unknown")
        self.assertEqual(out[1]["base"], "responding")   # the dance's echo
        self.assertEqual(out[2]["base"], "unknown")      # the move's isn't due
        self.assertEqual(out[3]["base"], "silent")       # and never came

    def test_dance_and_lights_are_the_base_firmware_strings(self):
        out, io = self.run_steps("cmd:dance:{}", "cmd:light:{\"effect\":\"flowing\"}",
                                 "cmd:light:{\"effect\":\"off\"}", "cmd:light:{\"effect\":\"disco\"}")
        self.assertEqual([w for w, _ in io["writes"]], ["d1", "w6", "w8"])
        self.assertFalse(out[3]["ok"])
        self.assertEqual(out[3]["error"]["code"], "invalid_param")

    def test_bad_params_write_nothing(self):
        out, io = self.run_steps("cmd:move:{\"direction\":\"up\"}", "cmd:drive:{\"forward\":2}",
                                 "cmd:move:{\"direction\":\"forward\",\"duration_ms\":-1}")
        self.assertEqual(io["writes"], [])
        for r in out:
            self.assertEqual(r["error"]["code"], "invalid_param")

    def test_long_drives_are_capped(self):
        _, io = self.run_steps("cmd:move:{\"direction\":\"forward\",\"duration_ms\":99999}", *["tick"] * 60)
        self.assertEqual(io["writes"][-1], ["x0.00 y0.00", 10000])

    def test_busy_lock_refuses(self):
        out, io = self.run_steps("lock", "cmd:move:{\"direction\":\"forward\"}")
        self.assertEqual(out[0]["error"]["code"], "busy")
        self.assertEqual(io["writes"], [])

    def test_console_pulse_is_held_by_the_task_and_reports_the_echo(self):
        out, io = self.run_steps("console:chassis=forward", "tick")
        self.assertEqual(out[0]["direction"], "forward")
        self.assertTrue(out[0]["sent"])
        self.assertEqual(out[0]["base"], "responding")
        self.assertEqual(out[1], {"handled": True})
        self.assertEqual(io["writes"][0][0], "x0.00 y1.00")
        self.assertEqual(io["writes"][-1][0], "x0.00 y0.00")   # 300 ms is over by the tick
        out, _ = self.run_steps("console:chassis=sideways", "console:status")
        self.assertIn("error", out[0])
        self.assertEqual(out[2], {"handled": False})

    def test_imu_without_a_sensor_says_so(self):
        out, _ = self.run_steps("cmd:imu:{}", "console:imu")
        self.assertEqual(out[0]["error"]["code"], "unavailable")
        self.assertIn("error", out[1])


if __name__ == "__main__":
    unittest.main()
