// Copyright (c) Impo contributors. Licensed under the Apache License, Version 2.0.

/*
 * Robin, the default avatar.
 *
 * A round, front-facing robin: olive-brown head, back and wings, an orange
 * face and breast, a cream belly, a small yellow beak, black bead eyes and
 * two orange feet. It is drawn from a head ellipse, a body ellipse and two
 * wing ellipses on a 64 x 64 grid, lit from the top left, with ordered
 * dithering and a hard outline. Calm and attentive: it cups its wings to
 * listen, tucks one under its beak to think, and hops when it is petted.
 */

#include "impo_pixel.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define W IMPO_PX_W
#define H IMPO_PX_H
#define TAU 6.2831853f
#define ONE 4096 /* Q12 */

enum {
    C_BG = 0,
    C_OUT,
    C_SEAM,
    C_BACK_D,
    C_BACK_M,
    C_BACK_L,
    C_BREAST_D,
    C_BREAST_M,
    C_BREAST_L,
    C_BELLY_D,
    C_BELLY_L,
    C_BEAK,
    C_BEAK_D,
    C_EYE,
    C_SHINE,
    C_BLUSH,
    C_MOUTH,
    C_LEG,
    C_SHADOW,
    C_HEART,
    C_G0, /* mode glow ramp, bright ... */
    C_G1,
    C_G2,
    C_G3, /* ... deep */
    C_ACC,
    C_COUNT,
};

static const uint32_t FIXED[C_COUNT] = {
    [C_BG] = 0x000000,
    [C_OUT] = 0x2b2118,
    [C_SEAM] = 0x4a3b2a,
    [C_BACK_D] = 0x5e4c36,
    [C_BACK_M] = 0x80694a,
    [C_BACK_L] = 0xa58c66,
    [C_BREAST_D] = 0xc45f26,
    [C_BREAST_M] = 0xe8833a,
    [C_BREAST_L] = 0xf9aa5e,
    [C_BELLY_D] = 0xd9cbb0,
    [C_BELLY_L] = 0xf6eedd,
    [C_BEAK] = 0xf6cd5a,
    [C_BEAK_D] = 0xb98a2c,
    [C_EYE] = 0x14100c,
    [C_SHINE] = 0xffffff,
    [C_BLUSH] = 0xf2705e,
    [C_MOUTH] = 0x47201a,
    [C_LEG] = 0xd98a3f,
    [C_SHADOW] = 0x15140f,
    [C_HEART] = 0xff5a7d,
};

typedef struct {
    uint32_t glow[4];
    uint32_t accent;
} scheme_t;

static const scheme_t SCHEMES[IMPO_MODE_COUNT] = {
    [IMPO_MODE_BOOT]      = { { 0xfcf8ee, 0xf7d6a8, 0xe0914d, 0x8a5a2b }, 0xf7d6a8 },
    [IMPO_MODE_IDLE]      = { { 0xe6f0dc, 0xc4d4b8, 0x6f9a7a, 0x2f5f4b }, 0x8fbf9a },
    [IMPO_MODE_LISTENING] = { { 0xe8faff, 0x8fdcff, 0x3fa2ff, 0x2a5bd7 }, 0x5cb8ff },
    [IMPO_MODE_THINKING]  = { { 0xfff0d6, 0xf7d6a8, 0xe0914d, 0x9a5a22 }, 0xf0a860 },
    [IMPO_MODE_SPEAKING]  = { { 0xeafff4, 0x9ff5cf, 0x3fd9a0, 0x1f9a7a }, 0x6ff0bf },
    [IMPO_MODE_ERROR]     = { { 0xffd6d6, 0xff6b6b, 0xc7304a, 0x6b1a3a }, 0xff5c5c },
    [IMPO_MODE_OFF]       = { { 0xd8d8c8, 0x9a9a84, 0x5e5e4e, 0x2e2e26 }, 0x8a8a74 },
};

static const uint8_t BAYER[4][4] = {
    { 0, 8, 2, 10 },
    { 12, 4, 14, 6 },
    { 3, 11, 1, 9 },
    { 15, 7, 13, 5 },
};

enum { PART_NONE = 0, PART_BODY, PART_WING };

static uint8_t s_fb[W * H];
static uint8_t s_part[W * H];
static uint16_t s_pal[C_COUNT];
static uint16_t s_pal_dim[C_COUNT];
static float s_glow[5][3]; /* blended glow ramp and accent, 0..255 */

/* -- Small helpers ----------------------------------------------------------- */

static uint32_t s_seed = 0x1badb002u;

static float frand(void)
{
    s_seed = s_seed * 1664525u + 1013904223u;
    return (float)(s_seed >> 8) / 16777216.0f;
}

static inline float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline int iround(float v)
{
    return (int)floorf(v + 0.5f);
}

static inline void put(int x, int y, uint8_t colour)
{
    if (x >= 0 && x < W && y >= 0 && y < H) {
        s_fb[y * W + x] = colour;
    }
}

/* Background effects never draw over the bird. */
static inline void put_behind(int x, int y, uint8_t colour)
{
    if (x >= 0 && x < W && y >= 0 && y < H && s_part[y * W + x] == PART_NONE) {
        s_fb[y * W + x] = colour;
    }
}

/* '#' draws `fill`, 'o' draws `alt`, anything else is transparent. */
static void stamp(const char *const *rows, int count, int x0, int y0, uint8_t fill, uint8_t alt)
{
    for (int r = 0; r < count; r++) {
        for (int c = 0; rows[r][c]; c++) {
            if (rows[r][c] == '#') {
                put(x0 + c, y0 + r, fill);
            } else if (rows[r][c] == 'o') {
                put(x0 + c, y0 + r, alt);
            }
        }
    }
}

static uint16_t to565(float r, float g, float b)
{
    int ri = (int)clampf(r + 0.5f, 0, 255), gi = (int)clampf(g + 0.5f, 0, 255), bi = (int)clampf(b + 0.5f, 0, 255);
    return (uint16_t)(((ri >> 3) << 11) | ((gi >> 2) << 5) | (bi >> 3));
}

uint32_t impo_pixel_accent(impo_mode_t mode)
{
    return SCHEMES[mode < IMPO_MODE_COUNT ? mode : IMPO_MODE_IDLE].accent;
}

/* Ease the glow colours toward the mode's scheme, then rebuild both palettes. */
static void update_palette(impo_mode_t mode, float dt, float fade)
{
    static bool started;
    const scheme_t *target = &SCHEMES[mode];
    float k = started ? 1.0f - expf(-dt * 7.0f) : 1.0f;
    started = true;
    for (int i = 0; i < 5; i++) {
        uint32_t hex = i < 4 ? target->glow[i] : target->accent;
        float rgb[3] = { (float)((hex >> 16) & 0xff), (float)((hex >> 8) & 0xff), (float)(hex & 0xff) };
        for (int c = 0; c < 3; c++) {
            s_glow[i][c] += (rgb[c] - s_glow[i][c]) * k;
        }
    }
    for (int i = 0; i < C_COUNT; i++) {
        float r, g, b;
        if (i >= C_G0) {
            const float *glow = s_glow[i - C_G0];
            r = glow[0] * fade;
            g = glow[1] * fade;
            b = glow[2] * fade;
        } else {
            r = (float)((FIXED[i] >> 16) & 0xff);
            g = (float)((FIXED[i] >> 8) & 0xff);
            b = (float)(FIXED[i] & 0xff);
        }
        s_pal[i] = to565(r, g, b);
        s_pal_dim[i] = to565(r * 0.72f, g * 0.72f, b * 0.72f);
    }
}

/* -- Ellipses in fixed point --------------------------------------------------- */

typedef struct {
    int32_t cx, cy;   /* centre, Q8 pixels */
    int32_t irx, iry; /* 16 / radius, Q8: turns a Q8 offset into Q12 units of radius */
    int32_t c, s;     /* rotation, Q12 */
    int x0, x1, y0, y1;
} ellipse_t;

static ellipse_t ellipse(float cx, float cy, float rx, float ry, float angle)
{
    float reach = (rx > ry ? rx : ry) + 1.0f;
    ellipse_t e = {
        .cx = (int32_t)(cx * 256.0f), .cy = (int32_t)(cy * 256.0f),
        .irx = (int32_t)(4096.0f / rx), .iry = (int32_t)(4096.0f / ry),
        .c = (int32_t)(cosf(angle) * ONE), .s = (int32_t)(sinf(angle) * ONE),
        .x0 = (int)floorf(cx - reach), .x1 = (int)ceilf(cx + reach),
        .y0 = (int)floorf(cy - reach), .y1 = (int)ceilf(cy + reach),
    };
    if (e.x0 < 0) e.x0 = 0;
    if (e.y0 < 0) e.y0 = 0;
    if (e.x1 > W - 1) e.x1 = W - 1;
    if (e.y1 > H - 1) e.y1 = H - 1;
    return e;
}

/* Position of a pixel centre in units of the radii (Q12); true when inside. */
static inline bool inside(const ellipse_t *e, int x, int y, int32_t *nu, int32_t *nv)
{
    int32_t dx = (x << 8) + 128 - e->cx, dy = (y << 8) + 128 - e->cy;
    int32_t u = ((dx * e->c + dy * e->s) >> 12) * e->irx >> 8;
    int32_t v = ((dy * e->c - dx * e->s) >> 12) * e->iry >> 8;
    if (u > ONE || u < -ONE || v > ONE || v < -ONE) {
        return false;
    }
    *nu = u;
    *nv = v;
    return u * u + v * v <= ONE * ONE;
}

/* Dark, mid or light (0..2) for a surface lit from the top left. */
static inline int tone(int32_t nu, int32_t nv, int x, int y)
{
    int32_t light = -(nu + nv) + (BAYER[y & 3][x & 3] - 8) * 90;
    return light > 1500 ? 2 : (light < -1900 ? 0 : 1);
}

static void fill_wing(const ellipse_t *wing)
{
    static const uint8_t tones[3] = { C_BACK_D, C_BACK_D, C_BACK_M };
    for (int y = wing->y0; y <= wing->y1; y++) {
        for (int x = wing->x0; x <= wing->x1; x++) {
            int32_t nu, nv;
            if (inside(wing, x, y, &nu, &nv)) {
                s_fb[y * W + x] = tones[tone(nu, nv, x, y)];
                s_part[y * W + x] = PART_WING;
            }
        }
    }
}

static void fill_bird(const ellipse_t *head, const ellipse_t *body, const ellipse_t *bib,
                      const ellipse_t *belly, int texture_x, int texture_y)
{
    static const uint8_t back[3] = { C_BACK_D, C_BACK_M, C_BACK_L };
    static const uint8_t breast[3] = { C_BREAST_D, C_BREAST_M, C_BREAST_L };
    static const uint8_t cream[3] = { C_BELLY_D, C_BELLY_L, C_BELLY_L };
    int y0 = head->y0 < body->y0 ? head->y0 : body->y0;
    int x0 = head->x0 < body->x0 ? head->x0 : body->x0;
    int x1 = head->x1 > body->x1 ? head->x1 : body->x1;
    for (int y = y0; y <= body->y1; y++) {
        for (int x = x0; x <= x1; x++) {
            int32_t nu, nv, unused_u, unused_v;
            if (!inside(head, x, y, &nu, &nv) && !inside(body, x, y, &nu, &nv)) {
                continue;
            }
            int shade = tone(nu, nv, x, y);
            uint8_t colour;
            if (inside(belly, x, y, &unused_u, &unused_v)) {
                colour = cream[shade];
            } else if (inside(bib, x, y, &unused_u, &unused_v)) {
                colour = breast[shade];
            } else {
                colour = back[shade];
                /* Sparse feather flecks that travel with the body. */
                uint32_t hash = (uint32_t)((x - texture_x) * 73856093) ^ (uint32_t)((y - texture_y) * 19349663);
                if (shade > 0 && ((hash >> 7) & 15) == 0) {
                    colour = back[shade - 1];
                }
            }
            s_fb[y * W + x] = colour;
            s_part[y * W + x] = PART_BODY;
        }
    }
}

/* Hard outline where the silhouette meets the background, a seam between parts. */
static void outline(void)
{
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint8_t part = s_part[y * W + x];
            if (part == PART_NONE) {
                continue;
            }
            uint8_t left = x > 0 ? s_part[y * W + x - 1] : PART_NONE;
            uint8_t right = x < W - 1 ? s_part[y * W + x + 1] : PART_NONE;
            uint8_t up = y > 0 ? s_part[(y - 1) * W + x] : PART_NONE;
            uint8_t down = y < H - 1 ? s_part[(y + 1) * W + x] : PART_NONE;
            if (!left || !right || !up || !down) {
                s_fb[y * W + x] = C_OUT;
            } else if (part == PART_WING && (left == PART_BODY || right == PART_BODY || up == PART_BODY || down == PART_BODY)) {
                s_fb[y * W + x] = C_SEAM;
            }
        }
    }
    /* Rim light: tint the lit edge, just inside the outline, with the mode's glow. */
    for (int y = 1; y < H; y++) {
        for (int x = 1; x < W; x++) {
            int i = y * W + x;
            if (s_part[i] == PART_BODY && s_fb[i] != C_OUT && (s_fb[i - 1] == C_OUT || s_fb[i - W] == C_OUT) && s_part[i - 1] != PART_WING) {
                s_fb[i] = C_G1;
            }
        }
    }
}

/* -- Background effects ---------------------------------------------------------- */

static void draw_aura(float cx, float cy, float strength)
{
    ellipse_t e = ellipse(cx, cy, 27.0f, 28.0f, 0);
    int32_t gain = (int32_t)(strength * ONE);
    for (int y = e.y0; y <= e.y1; y++) {
        for (int x = e.x0; x <= e.x1; x++) {
            int32_t nu, nv;
            if (!inside(&e, x, y, &nu, &nv)) {
                continue;
            }
            int32_t falloff = ONE - ((nu * nu + nv * nv) >> 12); /* 1 at the centre, 0 at the rim */
            int32_t level = ((falloff * gain) >> 12) * 20 >> 12;
            if (level > BAYER[y & 3][x & 3]) {
                s_fb[y * W + x] = level > BAYER[y & 3][x & 3] + 7 ? C_G2 : C_G3;
            }
        }
    }
}

static void draw_shadow(float cx, float y, float half_width)
{
    int row = iround(y);
    for (int x = iround(cx - half_width); x <= iround(cx + half_width); x++) {
        put_behind(x, row, C_SHADOW);
        if ((x & 1) == 0) {
            put_behind(x, row + 1, C_SHADOW);
        }
    }
}

/* Dotted rings that expand from the body and grow brighter with the level. */
static void draw_rings(float cx, float cy, float t, float level)
{
    for (int ring = 0; ring < 2; ring++) {
        float phase = t * 0.9f + ring * 0.5f;
        phase -= floorf(phase);
        float radius = 19.0f + phase * (9.0f + level * 5.0f);
        uint8_t colour = phase < 0.5f ? C_G1 : C_G3;
        for (int dot = 0; dot < 22; dot++) {
            float angle = dot * TAU / 22.0f + ring * 0.14f;
            put_behind(iround(cx + cosf(angle) * radius), iround(cy + sinf(angle) * radius * 0.96f), colour);
        }
    }
}

static void draw_sparkles(float cx, float cy, float t, float speed, int count, bool front)
{
    for (int i = 0; i < count; i++) {
        float angle = t * speed + i * TAU / count;
        if ((sinf(angle) > 0) != front) {
            continue;
        }
        int x = iround(cx + cosf(angle) * 25.0f);
        int y = iround(cy + sinf(angle) * 7.0f - 6.0f + sinf(t * 1.3f + i) * 9.0f);
        bool bright = sinf(t * 5.0f + i * 1.7f) > 0.2f;
        if (s_part[(y < 0 ? 0 : (y >= H ? H - 1 : y)) * W + (x < 0 ? 0 : (x >= W ? W - 1 : x))] != PART_NONE) {
            continue;
        }
        put(x, y, C_G0);
        if (bright) {
            put(x - 1, y, C_G1);
            put(x + 1, y, C_G1);
            put(x, y - 1, C_G1);
            put(x, y + 1, C_G1);
        }
    }
}

/* -- Face and extras --------------------------------------------------------------- */

typedef enum { EYE_OPEN, EYE_WIDE, EYE_CLOSED, EYE_HAPPY, EYE_CROSS } eye_t;

static void draw_eye(int x, int y, eye_t style)
{
    static const char *const open[] = { "###", "###", "###" };
    static const char *const wide[] = { ".##.", "####", "####", ".##." };
    static const char *const closed[] = { "###" };
    static const char *const happy[] = { ".#.", "#.#" };
    static const char *const cross[] = { "#.#", ".#.", "#.#" };
    switch (style) {
    case EYE_OPEN:
        stamp(open, 3, x - 1, y - 1, C_EYE, C_EYE);
        put(x - 1, y - 1, C_SHINE);
        break;
    case EYE_WIDE:
        stamp(wide, 4, x - 2, y - 2, C_EYE, C_EYE);
        put(x - 1, y - 1, C_SHINE);
        put(x, y - 1, C_SHINE);
        break;
    case EYE_CLOSED:
        stamp(closed, 1, x - 1, y, C_EYE, C_EYE);
        break;
    case EYE_HAPPY:
        stamp(happy, 2, x - 1, y - 1, C_EYE, C_EYE);
        break;
    case EYE_CROSS:
        stamp(cross, 3, x - 1, y - 1, C_EYE, C_EYE);
        break;
    }
}

/* `open` is the number of rows between the two halves of the beak. */
static void draw_beak(int x, int y, int open)
{
    static const char *const upper[] = { "#####", ".###." };
    static const char *const gap[] = { ".###." };
    static const char *const lower[] = { ".###.", "..#.." };
    static const char *const tip[] = { "..#.." };
    stamp(upper, 2, x - 2, y, C_BEAK, C_BEAK);
    if (open <= 0) {
        stamp(tip, 1, x - 2, y + 2, C_BEAK_D, C_BEAK_D);
        return;
    }
    for (int row = 0; row < open; row++) {
        stamp(gap, 1, x - 2, y + 2 + row, C_MOUTH, C_MOUTH);
    }
    stamp(lower, 2, x - 2, y + 2 + open, C_BEAK_D, C_BEAK_D);
}

static void draw_blush(int x, int y, bool strong)
{
    put(x, y, C_BLUSH);
    put(x + 1, y, C_BLUSH);
    if (strong) {
        put(x, y + 1, C_BLUSH);
        put(x + 1, y + 1, C_BLUSH);
        put(x - 1, y, C_BLUSH);
    }
}

static void draw_foot(int x, int y)
{
    static const char *const foot[] = { ".#.", "###" };
    stamp(foot, 2, x - 1, y, C_LEG, C_LEG);
}

static void draw_heart(int x, int y)
{
    static const char *const heart[] = { "#.#", "###", ".#." };
    stamp(heart, 3, x - 1, y, C_HEART, C_HEART);
}

static void draw_alert(int x, int y)
{
    static const char *const mark[] = { "##", "##", "##", "##", "..", "##" };
    stamp(mark, 6, x, y, C_G1, C_G1);
}

static void draw_thought_dots(float x, float y, float t)
{
    int shown = 1 + ((int)(t * 2.5f) % 3);
    for (int i = 0; i < shown; i++) {
        int px = iround(x + i * 4.0f), py = iround(y - i * 5.0f);
        put(px, py, C_G0);
        if (i > 0) {
            put(px + 1, py, C_G1);
            put(px, py + 1, C_G1);
        }
        if (i > 1) {
            put(px + 1, py + 1, C_G1);
            put(px - 1, py, C_G1);
            put(px, py - 1, C_G1);
        }
    }
}

/* -- Blink and gaze, kept across frames ------------------------------------------------ */

static float s_blink_in = 2.5f, s_blink_left, s_gaze_in, s_gaze_x, s_gaze_y, s_target_x, s_target_y;
static bool s_blink_again;

static bool update_eyes(float dt, bool wander)
{
    s_blink_in -= dt;
    if (s_blink_left > 0) {
        s_blink_left -= dt;
        if (s_blink_left <= 0 && s_blink_again) {
            s_blink_again = false;
            s_blink_in = 0.12f;
        }
    } else if (s_blink_in <= 0) {
        s_blink_left = 0.16f;
        s_blink_again = s_blink_in > -1.0f && frand() < 0.2f;
        s_blink_in = 2.2f + frand() * 3.0f;
    }
    s_gaze_in -= dt;
    if (s_gaze_in <= 0) {
        s_gaze_in = 1.2f + frand() * 2.4f;
        s_target_x = (frand() - 0.5f) * 3.0f;
        s_target_y = (frand() - 0.5f) * 1.6f;
    }
    if (!wander) {
        s_target_x = s_target_y = 0;
    }
    float k = 1.0f - expf(-dt * 14.0f);
    s_gaze_x += (s_target_x - s_gaze_x) * k;
    s_gaze_y += (s_target_y - s_gaze_y) * k;
    return s_blink_left > 0;
}

/* -- Frame --------------------------------------------------------------------------------- */

void impo_pixel_render(const impo_pose_t *p)
{
    static float s_last_t = -1.0f;
    float dt = s_last_t < 0 ? 0.04f : clampf(p->t - s_last_t, 0, 0.2f);
    s_last_t = p->t;

    impo_mode_t mode = p->mode < IMPO_MODE_COUNT ? p->mode : IMPO_MODE_IDLE;
    float t = p->t, mt = p->mode_t;
    float level = clampf(p->level, 0, 1);
    float happy = mode == IMPO_MODE_ERROR ? 0 : clampf(p->happy, 0, 1);
    bool joyful = happy > 0.15f;

    float fade = mode == IMPO_MODE_OFF ? clampf(1.0f - mt / 1.3f, 0, 1) : 1.0f;
    update_palette(mode, dt, fade);
    bool blinking = update_eyes(dt, mode == IMPO_MODE_IDLE || mode == IMPO_MODE_BOOT || mode == IMPO_MODE_SPEAKING);

    /* Body pose. */
    float breathe = sinf(t * 2.1f) * 0.03f;
    float squash = 1.0f;
    if (mode == IMPO_MODE_BOOT && mt < 0.6f) {
        float s = mt / 0.6f;
        squash = 0.45f + 0.55f * s + 0.18f * sinf(s * 3.1416f);
    }
    float bob = sinf(t * 1.7f) * 0.8f;
    if (mode == IMPO_MODE_SPEAKING) {
        bob += level * 1.4f * sinf(t * 11.0f);
    }
    float hop = happy * fabsf(sinf(t * 9.0f)) * 5.0f;
    float shake = mode == IMPO_MODE_ERROR && mt < 0.6f ? sinf(mt * 42.0f) * 2.0f : 0;
    float lean = mode == IMPO_MODE_THINKING ? 1.2f : 0;

    float cx = 32.0f + shake;
    float ground = 56.0f - hop;
    float body_rx = 16.0f * (1.0f + breathe) * (1.0f + (1.0f - squash) * 0.5f);
    float body_ry = 15.0f * (1.0f - breathe) * squash;
    float body_cy = ground - 1.0f - body_ry;
    float head_rx = 13.0f * (1.0f + breathe * 0.5f);
    float head_ry = 12.0f * squash;
    float head_cx = cx + lean;
    float head_cy = body_cy - body_ry * 0.62f - head_ry * 0.42f + bob;

    ellipse_t body = ellipse(cx, body_cy, body_rx, body_ry, 0);
    ellipse_t head = ellipse(head_cx, head_cy, head_rx, head_ry, 0);
    ellipse_t bib = ellipse((cx + head_cx) * 0.5f, head_cy + 7.0f * squash, 9.5f, 12.5f * squash, 0);
    ellipse_t belly = ellipse(cx, body_cy + 7.5f * squash, 10.0f, 8.5f * squash, 0);

    /* Wings: [0] is on the left of the screen. */
    float sway = sinf(t * 1.9f) * 0.08f;
    float wing_x[2] = { cx - body_rx + 2.5f, cx + body_rx - 2.5f };
    float wing_y[2] = { body_cy - 1.0f, body_cy - 1.0f };
    float wing_angle[2] = { 0.22f + sway, -0.22f - sway };
    float wing_rx = 3.6f, wing_ry = 8.0f * squash;
    bool wing_front[2] = { false, false };
    if (joyful) {
        float wiggle = sinf(t * 18.0f) * 0.35f;
        wing_x[0] -= 2.0f, wing_x[1] += 2.0f;
        wing_y[0] = wing_y[1] = body_cy - 9.0f;
        wing_angle[0] = -0.75f + wiggle, wing_angle[1] = 0.75f - wiggle;
    } else if (mode == IMPO_MODE_LISTENING) {
        wing_x[0] = head_cx - head_rx - 0.5f, wing_x[1] = head_cx + head_rx + 0.5f;
        wing_y[0] = wing_y[1] = head_cy + 4.0f;
        wing_angle[0] = -0.45f, wing_angle[1] = 0.45f;
        wing_ry = 6.5f;
    } else if (mode == IMPO_MODE_THINKING) {
        wing_x[1] = head_cx + 7.0f;
        wing_y[1] = head_cy + 12.5f;
        wing_angle[1] = 1.05f;
        wing_rx = 3.0f, wing_ry = 6.0f, wing_front[1] = true;
    } else if (mode == IMPO_MODE_SPEAKING) {
        float gesture = (0.25f + level * 0.5f) * sinf(t * 9.0f);
        wing_x[0] -= 1.5f, wing_x[1] += 1.5f;
        wing_angle[0] = 0.5f + gesture, wing_angle[1] = -0.5f + gesture;
    } else if (mode == IMPO_MODE_OFF && mt < 0.9f) {
        wing_x[1] += 3.0f;
        wing_y[1] = body_cy - 9.0f;
        wing_angle[1] = 0.8f + sinf(mt * 13.0f) * 0.45f;
    } else if (mode == IMPO_MODE_ERROR) {
        wing_angle[0] = 0.05f, wing_angle[1] = -0.05f;
        wing_y[0] = wing_y[1] = body_cy + 1.0f;
    }
    ellipse_t wings[2] = {
        ellipse(wing_x[0], wing_y[0], wing_rx, wing_ry, wing_angle[0]),
        ellipse(wing_x[1], wing_y[1], wing_rx, wing_ry, wing_angle[1]),
    };

    /* Draw back to front. */
    memset(s_fb, C_BG, sizeof(s_fb));
    memset(s_part, PART_NONE, sizeof(s_part));
    float appear = mode == IMPO_MODE_BOOT ? clampf(mt / 0.9f, 0, 1) : 1.0f;
    float aura = (0.62f + 0.08f * sinf(t * 1.4f) + level * 0.25f + happy * 0.2f) * appear;
    draw_aura(cx, body_cy - 6.0f, aura);

    for (int i = 0; i < 2; i++) {
        if (!wing_front[i]) {
            fill_wing(&wings[i]);
        }
    }
    fill_bird(&head, &body, &bib, &belly, iround(cx), iround(body_cy));
    for (int i = 0; i < 2; i++) {
        if (wing_front[i]) {
            fill_wing(&wings[i]);
        }
    }
    outline();

    draw_shadow(cx, 58.0f, 11.0f - hop * 0.8f);
    float speed = mode == IMPO_MODE_THINKING ? 1.5f : 0.6f;
    int sparkles = mode == IMPO_MODE_BOOT ? (int)clampf(mt * 4.0f, 0, 5) : (mode == IMPO_MODE_ERROR ? 0 : 5);
    draw_sparkles(cx, body_cy - 6.0f, t, speed, sparkles, false);
    if (mode == IMPO_MODE_LISTENING || mode == IMPO_MODE_SPEAKING) {
        draw_rings(cx, body_cy - 6.0f, t, level);
    }

    /* Feet shuffle while speaking. */
    int foot_y = iround(ground);
    int step = mode == IMPO_MODE_SPEAKING && sinf(t * 7.0f) > 0.4f ? 1 : 0;
    draw_foot(iround(cx - 5.0f), foot_y - step);
    draw_foot(iround(cx + 5.0f), foot_y - (mode == IMPO_MODE_SPEAKING ? 1 - step : 0));

    /* Face. */
    float gaze_x = s_gaze_x, gaze_y = s_gaze_y;
    if (mode == IMPO_MODE_THINKING) {
        gaze_x = sinf(t * 1.6f) * 1.6f;
        gaze_y = -1.4f;
    }
    eye_t eye = EYE_OPEN;
    if (mode == IMPO_MODE_ERROR) {
        eye = EYE_CROSS;
    } else if (joyful) {
        eye = EYE_HAPPY;
    } else if ((mode == IMPO_MODE_BOOT && mt < 0.9f) || (mode == IMPO_MODE_OFF && mt > 0.5f) || blinking) {
        eye = EYE_CLOSED;
    } else if (mode == IMPO_MODE_LISTENING) {
        eye = EYE_WIDE;
    }
    int eye_y = iround(head_cy - 1.0f + gaze_y);
    draw_eye(iround(head_cx - 5.5f + gaze_x), eye_y, eye);
    draw_eye(iround(head_cx + 5.5f + gaze_x), eye_y, eye);

    int open = 0;
    if (joyful) {
        open = 2;
    } else if (mode == IMPO_MODE_SPEAKING) {
        open = (int)clampf(level * 3.2f + 0.6f + 0.5f * sinf(t * 23.0f), 0, 3);
    } else if (mode == IMPO_MODE_LISTENING) {
        open = 1;
    }
    int beak_x = iround(head_cx + (mode == IMPO_MODE_THINKING ? 1.0f : 0));
    draw_beak(beak_x, iround(head_cy + 3.0f), open);

    bool rosy = joyful || mode == IMPO_MODE_SPEAKING;
    if (mode != IMPO_MODE_ERROR) {
        draw_blush(iround(head_cx - 9.0f), iround(head_cy + 3.0f), rosy);
        draw_blush(iround(head_cx + 8.0f), iround(head_cy + 3.0f), rosy);
    }

    draw_sparkles(cx, body_cy - 6.0f, t, speed, sparkles, true);
    if (mode == IMPO_MODE_THINKING) {
        draw_thought_dots(head_cx + head_rx + 2.0f, head_cy - 6.0f, mt);
    }
    if (mode == IMPO_MODE_ERROR) {
        draw_alert(iround(head_cx + head_rx + 3.0f), iround(head_cy - 10.0f));
    }
    if (joyful) {
        float rise = t * 0.8f - floorf(t * 0.8f);
        draw_heart(iround(cx - 17.0f), iround(head_cy - 6.0f - rise * 10.0f));
        draw_heart(iround(cx + 17.0f), iround(head_cy - 1.0f - (1.0f - rise) * 10.0f));
    }
}

/* -- Scaling to the screen ------------------------------------------------------------------- */

#define MAX_SIZE 512

/* Grid cell behind each screen pixel; the high bit marks a cell's last pixel. */
static uint8_t s_cell[MAX_SIZE];
static int s_size;

void impo_pixel_set_size(int px)
{
    s_size = px > MAX_SIZE ? MAX_SIZE : (px < 0 ? 0 : px);
    bool grid = s_size >= 3 * W; /* cells of 3+ pixels show a faint grid */
    for (int i = 0; i < s_size; i++) {
        int cell = i * W / s_size;
        bool last = (i + 1) * W / s_size != cell;
        s_cell[i] = (uint8_t)(cell | (grid && last ? 0x80 : 0));
    }
}

void impo_pixel_scale(uint16_t *dst, int stride_px, int x0, int x1, int y0, int y1)
{
    int width = x1 - x0 + 1;
    const uint16_t *previous = NULL;
    for (int y = y0; y <= y1; y++, dst += stride_px) {
        /* Screen rows of one cell repeat, apart from the cell's dim last row. */
        if (previous && s_cell[y] == s_cell[y - 1]) {
            memcpy(dst, previous, (size_t)width * sizeof(uint16_t));
            continue;
        }
        const uint8_t *row = &s_fb[(s_cell[y] & 0x7f) * W];
        bool dim_row = (s_cell[y] & 0x80) != 0;
        for (int i = 0; i < width; i++) {
            uint8_t cell = s_cell[x0 + i];
            uint8_t colour = row[cell & 0x7f];
            dst[i] = dim_row || (cell & 0x80) ? s_pal_dim[colour] : s_pal[colour];
        }
        previous = dst;
    }
}
