# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

from __future__ import annotations

import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


def function_source(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for end in range(brace, len(source)):
        if source[end] == "{":
            depth += 1
        elif source[end] == "}":
            depth -= 1
            if depth == 0:
                return source[start : end + 1]
    raise AssertionError(f"Unclosed function: {signature}")


class LinkOtaTest(unittest.TestCase):
    def compile_and_run(self, sources: list[Path], include: Path) -> str:
        compiler = shlex.split(os.environ.get("CC", "cc"))
        if not compiler or shutil.which(compiler[0]) is None:
            self.skipTest("C compiler unavailable")
        binary = include / "ota-test"
        compiled = subprocess.run(
            [*compiler, "-std=c11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror",
             "-DLINK_FAKE_CUSTOM_TASKS=1",
             "-I", str(include), "-I", str(ROOT / "tests/link_fakes"),
             "-I", str(ROOT / "main"), *map(str, sources), "-o", str(binary)],
            capture_output=True, text=True,
        )
        self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
        ran = subprocess.run([str(binary)], capture_output=True, text=True)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
        return ran.stdout

    def test_ota_disabled_and_enabled(self) -> None:
        for enabled, dev in ((False, True), (True, True), (True, False)):
            with self.subTest(enabled=enabled, dev=dev), tempfile.TemporaryDirectory() as name:
                tmp = Path(name)
                (tmp / "sdkconfig.h").write_text(
                    ("#define CONFIG_HOMEHUB_OTA_ENABLED 1\n" if enabled else "")
                    + f"#define CONFIG_HOMEHUB_DEV_BUILD {int(dev)}\n"
                )
                # Type-check log arguments without printing or making them unused.
                (tmp / "esp_log.h").write_text(
                    "#include <stdio.h>\n"
                    "#define ESP_LOGI(tag, ...) ((void)(tag), (void)sizeof(printf(__VA_ARGS__)))\n"
                    "#define ESP_LOGW ESP_LOGI\n#define ESP_LOGE ESP_LOGI\n"
                )
                self.compile_and_run(
                    [ROOT / "main/ota.c", ROOT / "tests/link_ota_harness.c"], tmp,
                )

    def test_disabled_ota_does_not_interrupt_setup(self) -> None:
        # Exercise the actual entry-point functions with fake connectivity/UI,
        # without linking the rest of app.c's hardware orchestration.
        app = (ROOT / "main/app.c").read_text()
        functions = "\n".join(function_source(app, signature) for signature in (
            "static bool start_wifi_join_ota_if_requested(",
            "static void on_ble_ota(",
        ))
        with tempfile.TemporaryDirectory() as name:
            tmp = Path(name)
            (tmp / "sdkconfig.h").write_text("")
            source = tmp / "setup.c"
            source.write_text('''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "ota.h"
#include "esp_app_desc.h"
#define ESP_LOGI(...) ((void)0)
static esp_app_desc_t app = { .version = "999.0.0" };
const esp_app_desc_t *esp_app_get_description(void) { return &app; }
static int wifi_checks, statuses, callbacks;
static bool wifi_mgr_is_connected(void) { wifi_checks++; return false; }
static void ble_server_send_status(const char *s) {
    assert(strcmp(s, "ota_skipped") == 0);
    statuses++;
}
static bool ble_server_send_pairing_status(const char *s, uint32_t generation) {
    (void)s; (void)generation;
    assert(!"Disabled OTA must not send pairing status");
    return false;
}
static bool require_provisioning_pairing_session(uint32_t generation) {
    (void)generation;
    assert(!"Disabled OTA must not check the provisioning session");
    return false;
}
static void ui_set_status(const char *s) {
    assert(strcmp(s, "ota_skipped") == 0);
    statuses++;
}
static void ble_ota_status(const ota_event_t *ev, void *user) {
    (void)user;
    assert(ev->result == OTA_RESULT_SKIPPED);
    callbacks++;
}
''' + functions + '''
int main(void) {
    for (int force = 0; force <= 1; force++) {
        // False means provision continues normal setup.
        assert(!start_wifi_join_ota_if_requested("https://example.com/fw.bin", force, 0));
        assert(!start_wifi_join_ota_if_requested("https://example.com/fw.bin", force, 42));
        assert(statuses == force * 2);
        on_ble_ota("https://example.com/fw.bin", force);
        assert(statuses == (force + 1) * 2);
    }
    assert(wifi_checks == 0);
    assert(callbacks == 0); // Reports skipped directly, without calling ota_start.
    return 0;
}
''')
            self.compile_and_run([source, ROOT / "main/ota.c"], tmp)

    def test_registration_only_advertises_enabled_ota(self) -> None:
        noise = (ROOT / "main/noise_control.cpp").read_text()
        functions = "\n".join(function_source(noise, signature) for signature in (
            "static cJSON *add_command(",
            "static void add_register_metadata_string(",
            "static char *build_register_json(",
        ))
        with tempfile.TemporaryDirectory() as name:
            tmp = Path(name)
            source = tmp / "registration.c"
            # These builders use the C subset of C++, with nullptr as the only
            # exception. Execute them with the same cJSON fake as other tests.
            source.write_text('''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_app_desc.h"
#define nullptr NULL
#define CONFIG_HOMEHUB_TUNNEL 1
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
static void *heap_caps_malloc_prefer(size_t size, size_t count, ...) { (void)count; return malloc(size); }
static bool enabled;
#define NOISE_CTRL_WAKEUP_SUPPORTED enabled
bool ota_is_enabled(void) { return enabled; }
static esp_app_desc_t app = { .version = "999.0.0" };
const esp_app_desc_t *esp_app_get_description(void) { return &app; }
static char s_wifi_ssid[33] = "test-wifi", s_register_req_id[40];
static const char *s_node_id = "test-node", *s_display_name = "Test device";
static void copy_wifi_ssid(char *out, size_t size) { snprintf(out, size, "%s", s_wifi_ssid); }
static void make_uuid(char *out, size_t size) { snprintf(out, size, "test-request"); }
// gadget_attest.c has its own harness; here, check what the builder hands it.
static bool gadget_attest_add(cJSON *params, const uint8_t *hash, size_t len) {
    if (!hash || len != 32 || hash[0] != 0xa5) return false;
    cJSON_AddStringToObject(params, "attest", "seen");
    return true;
}
''' + functions + '''
int main(void) {
    for (int on = 0; on <= 1; on++) {
        enabled = on;
        static const uint8_t hash[32] = {0xa5};
        char *json = on ? build_register_json(hash, sizeof(hash))
                        : build_register_json(NULL, 0);
        assert(json);
        puts(json);
        free(json);
    }
    return 0;
}
''')
            output = self.compile_and_run([source, ROOT / "tests/link_fakes/cJSON.c"], tmp)
            registrations = [json.loads(line)["params"] for line in output.splitlines()]
            self.assertEqual(len(registrations), 2)
            for enabled, params in zip((False, True), registrations):
                commands = params["commands_v2"]
                self.assertIn("device.health", commands)
                self.assertIn("device.discover", commands)
                self.assertEqual("device.ota" in commands, enabled)
                # Names only: a board without a description describes nothing.
                self.assertEqual(commands["device.health"], {})
                self.assertEqual(commands["device.discover"], {"timeout_ms": 90000})
                self.assertEqual(params["version"], "999.0.0")
                self.assertEqual(params["is_wakeup_supported"], enabled)
                # The session's handshake hash reaches attestation unchanged.
                self.assertEqual(params.get("attest"), "seen" if enabled else None)
                # The firmware names no product and no account.
                self.assertNotIn("product_id", params)
                for field in ("instructions", "events", "description"):
                    self.assertNotIn(field, params)


    def test_registration_carries_the_description_as_written(self) -> None:
        # A board with a description (devices/<device>/description.json) sends it with
        # the platform's (main/platform.json), each as written, in link.register.
        noise = (ROOT / "main/noise_control.cpp").read_text()
        functions = "\n".join(function_source(noise, signature) for signature in (
            "static cJSON *add_command(",
            "static cJSON *raw_reference(",
            "static void add_register_metadata_string(",
            "static char *build_register_json(",
        ))
        platform = (ROOT / "main/platform.json").read_text()
        device = (ROOT / "devices/m5stack-stackchan/description.json").read_text()
        literal = lambda text: json.dumps(text)
        with tempfile.TemporaryDirectory() as name:
            tmp = Path(name)
            source = tmp / "registration.c"
            source.write_text('''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_app_desc.h"
#define nullptr NULL
#define CONFIG_IMPO_ENABLED 1
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
static size_t largest;
static void *heap_caps_malloc_prefer(size_t size, size_t count, ...) { (void)count; if (size > largest) largest = size; return malloc(size); }
bool ota_is_enabled(void) { return false; }
static esp_app_desc_t app = { .version = "1.0.0" };
const esp_app_desc_t *esp_app_get_description(void) { return &app; }
static char s_wifi_ssid[33] = "test-wifi", s_register_req_id[40];
static const char *s_node_id = "test-node", *s_display_name = "Test device";
static void copy_wifi_ssid(char *out, size_t size) { snprintf(out, size, "%s", s_wifi_ssid); }
static void make_uuid(char *out, size_t size) { snprintf(out, size, "test-request"); }
static bool gadget_attest_add(cJSON *params, const uint8_t *hash, size_t len) { (void)params; (void)hash; (void)len; return false; }
static void impo_commands_capabilities(cJSON *params) { (void)params; }
static void impo_commands_describe(cJSON *commands) { cJSON_AddItemToObject(commands, "head.stop", cJSON_CreateObject()); }
typedef struct { const char *description; } impo_board_t;
static const impo_board_t board = { ''' + literal(device) + ''' };
static const impo_board_t *impo_board = &board;
static const char platform_json[] = ''' + literal(platform) + ''';
''' + functions + '''
int main(void) {
    char *json = build_register_json(NULL, 0);
    assert(json);
    puts(json);
    free(json);
    fprintf(stderr, "%zu\\n", largest);
    return 0;
}
''')
            output = self.compile_and_run([source, ROOT / "tests/link_fakes/cJSON.c"], tmp)
            params = json.loads(output)["params"]
            self.assertEqual(params["description"], {"platform": json.loads(platform), "device": json.loads(device)})
            self.assertNotIn("product_id", params)
            self.assertNotIn("device.set_name", params["commands_v2"])
            self.assertEqual(params["commands_v2"]["head.stop"], {})


if __name__ == "__main__":
    unittest.main()
