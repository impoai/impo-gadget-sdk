// SPDX-License-Identifier: Apache-2.0
#include "display_canvas.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

static unsigned char frame[80][48];
static bool locked, unavailable, requested;
static int begins;
static bool begin(void) {
    if (unavailable) return false;
    assert(!locked); locked = true; begins++; return true;
}
static void pixel(int x, int y, bool white) {
    assert(locked && x >= 0 && x < 48 && y >= 0 && y < 80);
    frame[y][x] = white;
}
static void end(bool refresh) { assert(locked); locked = false; requested = refresh; }
static bool busy(void) { return unavailable; }
static int off_state(void) { return 1; }
static const display_canvas_ops_t ops = {
    .width = 48, .height = 80, .begin = begin, .pixel = pixel, .end = end,
    .busy = busy, .power_off_image_state = off_state,
};
static cJSON *run(const char *command, const char *json, bool ok) {
    cJSON *p = json ? cJSON_Parse(json) : NULL;
    cJSON *r = display_canvas_command(command, p);
    cJSON_Delete(p);
    assert(r && cJSON_IsTrue(cJSON_GetObjectItem(r, "ok")) == ok);
    return r;
}
static void call(const char *c, const char *p, bool ok) { cJSON_Delete(run(c, p, ok)); }
int main(void) {
    call("display.get_info", NULL, false);
    display_canvas_bind(&ops);
    cJSON *r = run("display.get_info", NULL, true);
    cJSON *payload = cJSON_GetObjectItem(r, "payload");
    assert(payload && cJSON_GetObjectItem(payload, "width")->valueint == 48);
    assert(cJSON_GetObjectItem(payload, "height")->valueint == 80);
    assert(cJSON_IsFalse(cJSON_GetObjectItem(payload, "busy")));
    cJSON *off = cJSON_GetObjectItem(payload, "power_off_image");
    assert(cJSON_IsTrue(cJSON_GetObjectItem(off, "stored")));
    assert(cJSON_IsTrue(cJSON_GetObjectItem(off, "persistent")));
    cJSON_Delete(r);
    assert(!display_canvas_command("speaker.play", NULL));
    call("display.clear", "{}", true);
    assert(!requested);
    for (int y = 0; y < 80; y++) for (int x = 0; x < 48; x++) assert(frame[y][x] == 1);
    call("display.clear", "{\"x\":4,\"y\":6,\"width\":7,\"height\":9,\"color\":\"black\"}", true);
    for (int y = 0; y < 80; y++) for (int x = 0; x < 48; x++)
        assert(frame[y][x] == !(x >= 4 && x < 11 && y >= 6 && y < 15));
    unsigned char saved[sizeof(frame)];
    memcpy(saved, frame, sizeof(frame));
    int before = begins;
    call("display.clear", "{\"x\":47,\"width\":2}", false);
    call("display.clear", "{\"x\":0.5}", false);
    call("display.clear", "{\"width\":1e30}", false);
    call("display.clear", "{\"color\":\"red\"}", false);
    call("display.clear", "{\"refresh\":1}", false);
    call("display.draw_text", "{\"text\":\"中文\"}", false);
    call("display.draw_text", "{\"text\":\"A\",\"scale\":9}", false);
    call("display.draw_text", "{\"text\":\"A\",\"width\":1}", false);
    call("display.draw_text", "{\"text\":\"A\\tB\"}", false);
    assert(begins == before && !memcmp(saved, frame, sizeof(frame)));
    unavailable = true;
    call("display.clear", "{}", false);
    call("display.refresh", "{}", false);
    unavailable = false;
    assert(begins == before && !memcmp(saved, frame, sizeof(frame)));

    call("display.clear", "{\"color\":\"black\"}", true);
    // Two glyph cells wide, two rows high. The fifth glyph must be clipped.
    call("display.draw_text", "{\"x\":5,\"y\":7,\"width\":12,\"height\":16,\"text\":\"ABCDE\",\"scale\":1,\"background\":\"white\"}", true);
    unsigned black = 0, white = 0;
    for (int y = 0; y < 80; y++) for (int x = 0; x < 48; x++) {
        if (x < 5 || x >= 17 || y < 7 || y >= 23) assert(frame[y][x] == 0);
        else if (frame[y][x]) white++; else black++;
    }
    assert(white && black);
    memcpy(saved, frame, sizeof(frame));
    call("display.clear", "{\"color\":\"black\"}", true);
    call("display.draw_text", "{\"x\":5,\"y\":7,\"width\":12,\"height\":16,\"text\":\"AB\\nCD\",\"scale\":1,\"background\":\"white\"}", true);
    assert(!memcmp(saved, frame, sizeof(frame)));
    r = run("display.refresh", "{}", true);
    payload = cJSON_GetObjectItem(r, "payload");
    // The control transport serializes only payload/error, not arbitrary
    // top-level keys. These fields must survive the actual wire envelope.
    assert(cJSON_IsTrue(cJSON_GetObjectItem(payload, "queued")));
    assert(cJSON_IsFalse(cJSON_GetObjectItem(payload, "buffered")));
    cJSON_Delete(r);
    assert(requested && !memcmp(saved, frame, sizeof(frame)));
    cJSON *commands = cJSON_CreateObject();
    display_canvas_describe(commands);
    assert(cJSON_GetArraySize(commands) == 5);
    assert(cJSON_HasObjectItem(commands, "display.draw_text"));
    assert(cJSON_HasObjectItem(commands, "display.set_power_off_image"));
    cJSON_Delete(commands);
    puts("canvas: geometry, preservation, wrapping, clipping, validation, busy and refresh passed");
    return 0;
}
