// SPDX-License-Identifier: Apache-2.0
#include "display_canvas.h"
#include "pixel_font.h"
#include <string.h>

static const display_canvas_ops_t *s_ops;
void display_canvas_bind(const display_canvas_ops_t *ops) { s_ops = ops; }

static cJSON *error(const char *code, const char *message) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", false);
    cJSON *e = cJSON_AddObjectToObject(r, "error");
    cJSON_AddStringToObject(e, "code", code);
    cJSON_AddStringToObject(e, "message", message);
    return r;
}

static cJSON *success(cJSON *payload) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    cJSON_AddItemToObject(r, "payload", payload);
    return r;
}

cJSON *display_canvas_info(void) {
    if (!s_ops) return error("unsupported", "no canvas");
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    cJSON_AddNumberToObject(r, "width", s_ops->width);
    cJSON_AddNumberToObject(r, "height", s_ops->height);
    cJSON_AddNumberToObject(r, "bits_per_pixel", 1);
    cJSON_AddBoolToObject(r, "color", false);
    cJSON_AddBoolToObject(r, "canvas", true);
    cJSON_AddBoolToObject(r, "busy", s_ops->busy());
    cJSON_AddStringToObject(r, "origin", "top-left; x right, y down; pixels");
    cJSON_AddStringToObject(r, "colors", "black,white");
    cJSON_AddStringToObject(r, "text", "ASCII; 6x8 cell times scale 1..8; newline and wrap; max 512 bytes");
    cJSON_AddStringToObject(r, "images", "baseline JPEG or big-endian RGB565; render Unicode/formulas into images");
    cJSON_AddStringToObject(r, "refresh", "full screen, about 4 seconds; draw with refresh:false then display.refresh; poll busy");
    if (s_ops->power_off_image_state) {
        int state = s_ops->power_off_image_state();
        cJSON *off = cJSON_AddObjectToObject(r, "power_off_image");
        cJSON_AddBoolToObject(off, "supported", state >= 0);
        cJSON_AddBoolToObject(off, "stored", state > 0);
        cJSON_AddBoolToObject(off, "persistent", true);
    }
    return r;
}

static void param(cJSON *o, const char *name, const char *type, const char *description) {
    cJSON *p = cJSON_AddObjectToObject(o, name);
    cJSON_AddStringToObject(p, "type", type);
    cJSON_AddStringToObject(p, "description", description);
}
static cJSON *describe(cJSON *commands, const char *name, const char *desc) {
    cJSON *c = cJSON_AddObjectToObject(commands, name);
    cJSON_AddStringToObject(c, "description", desc);
    cJSON_AddObjectToObject(c, "required");
    return cJSON_AddObjectToObject(c, "optional");
}
void display_canvas_describe(cJSON *commands) {
    describe(commands, "display.get_info", "Get canvas size, colors, text/image formats and busy state.");
    const char *names[] = {"display.clear", "display.draw_text"};
    const char *descs[] = {
        "Fill a rectangle, preserving everything outside it. Defaults to the whole canvas, white. Buffered until display.refresh.",
        "Draw text in a rectangle, preserving pixels outside it. ASCII only; wrap at width and clip at height. Render Unicode/formulas as an image. Buffered until display.refresh.",
    };
    for (int i = 0; i < 2; i++) {
        cJSON *o = describe(commands, names[i], descs[i]);
        param(o, "x", "integer", "Left pixel; default 0.");
        param(o, "y", "integer", "Top pixel; default 0.");
        param(o, "width", "integer", "Rectangle width; default remaining screen width.");
        param(o, "height", "integer", "Rectangle height; default remaining screen height.");
        param(o, "color", "string", i ? "black (default) or white." : "white (default) or black.");
        param(o, "refresh", "boolean", "Queue one full refresh after drawing; default false.");
        if (i) {
            param(cJSON_GetObjectItem(cJSON_GetObjectItem(commands, names[i]), "required"),
                  "text", "string", "1..512 ASCII characters; newline allowed.");
            param(o, "scale", "integer", "Font scale 1..8; default 3 (18x24 pixel cell).");
            param(o, "background", "string", "transparent (default), white or black; fills this rectangle.");
        }
    }
    describe(commands, "display.refresh", "Queue the buffered canvas for a full refresh. Returns queued:true; poll display.get_info busy until false. Retry busy responses after the current image/refresh ends.");
    cJSON *off = describe(commands, "display.set_power_off_image",
        "Save the current complete canvas as the power-off image, including images and text. "
        "First draw the desired page, await successful drawing/refresh and busy:false, then call with {}. "
        "This command takes NO URL and does NOT shut down. Returns stored:true only after flash write/readback. "
        "The saved page survives reboot, works offline and is independent of later drawings. "
        "Use reset:true to restore the default goodbye page; that returns stored:false.");
    param(off, "reset", "boolean", "Default false: save current canvas. True: remove custom power-off image.");
}

static bool integer(const cJSON *p, const char *key, int fallback, int lo, int hi, int *out) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(p, key);
    if (!v) { *out = fallback; return true; }
    if (!cJSON_IsNumber(v) || v->valuedouble < lo || v->valuedouble > hi ||
        v->valuedouble != v->valueint) return false;
    *out = v->valueint;
    return true;
}
static bool color(const cJSON *p, const char *key, int fallback, bool transparent, int *out) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(p, key);
    if (!v) { *out = fallback; return true; }
    if (!cJSON_IsString(v)) return false;
    if (!strcmp(v->valuestring, "white")) *out = 1;
    else if (!strcmp(v->valuestring, "black")) *out = 0;
    else if (transparent && !strcmp(v->valuestring, "transparent")) *out = -1;
    else return false;
    return true;
}

cJSON *display_canvas_command(const char *command, const cJSON *p) {
    bool clear = !strcmp(command, "display.clear");
    bool text = !strcmp(command, "display.draw_text");
    bool present = !strcmp(command, "display.refresh");
    if (!strcmp(command, "display.get_info")) {
        cJSON *info = display_canvas_info();
        if (!s_ops) return info;
        cJSON_DeleteItemFromObject(info, "ok");
        return success(info);
    }
    if (!clear && !text && !present) return NULL;
    if (!s_ops) return error("unsupported", "no canvas");
    if (p && !cJSON_IsObject(p)) return error("invalid_params", "params must be an object");
    int x = 0, y = 0, w = 0, h = 0, scale = 3, ink = 0, background = -1;
    const char *value = NULL;
    bool refresh = present;
    if (!present) {
        const cJSON *r = cJSON_GetObjectItemCaseSensitive(p, "refresh");
        if (r && !cJSON_IsBool(r)) return error("invalid_params", "refresh must be boolean");
        refresh = cJSON_IsTrue(r);
        if (!integer(p, "x", 0, 0, s_ops->width - 1, &x) ||
            !integer(p, "y", 0, 0, s_ops->height - 1, &y) ||
            !integer(p, "width", s_ops->width - x, 1, s_ops->width - x, &w) ||
            !integer(p, "height", s_ops->height - y, 1, s_ops->height - y, &h) ||
            !color(p, "color", clear ? 1 : 0, false, &ink))
            return error("invalid_params", "rectangle must fit the canvas; color must be black or white");
        if (text) {
            const cJSON *t = cJSON_GetObjectItemCaseSensitive(p, "text");
            if (!cJSON_IsString(t) || !t->valuestring[0] || strlen(t->valuestring) > 512)
                return error("invalid_params", "text must contain 1..512 ASCII characters");
            value = t->valuestring;
            for (const unsigned char *c = (const unsigned char *)value; *c; c++)
                if ((*c < 32 && *c != '\n') || *c > 126)
                    return error("invalid_params", "render Unicode text into an image");
            if (!integer(p, "scale", 3, 1, 8, &scale) ||
                !color(p, "background", -1, true, &background) || w < 6 * scale || h < 8 * scale)
                return error("invalid_params", "scale must be 1..8 and one character must fit; background is transparent/black/white");
        }
    }
    if (!s_ops->begin()) return error("busy", "display is refreshing, downloading or shutting down; retry");
    if (clear || background >= 0)
        for (int yy = y; yy < y + h; yy++)
            for (int xx = x; xx < x + w; xx++) s_ops->pixel(xx, yy, clear ? ink : background);
    if (text) {
        int cx = x, cy = y;
        for (const unsigned char *c = (const unsigned char *)value; *c; c++) {
            if (*c == '\n') { cx = x; cy += 8 * scale; continue; }
            if (cx + 6 * scale > x + w) { cx = x; cy += 8 * scale; }
            if (cy + 8 * scale > y + h) break;
            for (int gx = 0; gx < 5; gx++)
                for (int gy = 0; gy < 8; gy++)
                    if (pixel_font[*c - PIXEL_FONT_FIRST][gx] & (1 << gy))
                        for (int dx = 0; dx < scale; dx++)
                            for (int dy = 0; dy < scale; dy++)
                                s_ops->pixel(cx + gx * scale + dx, cy + gy * scale + dy, ink);
            cx += 6 * scale;
        }
    }
    s_ops->end(refresh);
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddBoolToObject(payload, "queued", refresh);
    cJSON_AddBoolToObject(payload, "buffered", !refresh);
    return success(payload);
}
