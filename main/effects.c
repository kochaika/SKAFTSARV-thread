/*
   This code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <string.h>

#include "effects.h"

static const char *NAMES[EFFECT_COUNT] = {
    [EFFECT_SOLID]         = "Solid",
    [EFFECT_RAINBOW]       = "Rainbow",
    [EFFECT_COLOUR_WIPE]   = "Colour Wipe",
    [EFFECT_BREATHE]       = "Breathe",
    [EFFECT_TWINKLE]       = "Twinkle",
    [EFFECT_FIRE]          = "Fire",
    [EFFECT_THEATER_CHASE] = "Theater Chase",
};

/* Animation periods, in milliseconds. */
#define RAINBOW_CYCLE_MS  6000   /* one full hue rotation */
#define WIPE_CYCLE_MS     3000   /* fill then empty */
#define BREATHE_CYCLE_MS  4000
#define TWINKLE_CYCLE_MS  1600   /* per-pixel, phase-offset by a hash of the index */
#define FIRE_STEP_MS       110   /* noise keyframe interval */
#define CHASE_STEP_MS      120   /* one pixel of travel */

#define BREATHE_FLOOR       24   /* never fully dark, or the lamp looks broken */

const char *effects_name(uint8_t effect_id)
{
    if (effect_id >= EFFECT_COUNT || !NAMES[effect_id]) {
        return "Solid";
    }
    return NAMES[effect_id];
}

bool effects_is_animated(uint8_t effect_id)
{
    return effect_id != EFFECT_SOLID && effect_id < EFFECT_COUNT;
}

/* ---------------------------------------------------------------- helpers */

static inline uint8_t mul8(uint8_t a, uint8_t b)
{
    return (uint8_t)(((uint16_t)a * (uint16_t)b + 127) / 255);
}

static inline RGB_color_t scale_rgb(RGB_color_t c, uint8_t s)
{
    RGB_color_t out = { mul8(c.red, s), mul8(c.green, s), mul8(c.blue, s) };
    return out;
}

static const RGB_color_t BLACK = {0, 0, 0};

/* Cheap integer hash. Used to give each pixel a stable, arbitrary-looking phase
 * without carrying any state between frames. */
static uint8_t hash8(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return (uint8_t)x;
}

/* Symmetric 0..255..0 ramp over a period. */
static uint8_t triangle(uint32_t t_ms, uint32_t period_ms)
{
    uint32_t phase = period_ms ? (t_ms % period_ms) : 0;
    uint32_t half  = period_ms / 2;
    if (half == 0) {
        return 255;
    }
    if (phase < half) {
        return (uint8_t)(phase * 255 / half);
    }
    return (uint8_t)((period_ms - phase) * 255 / half);
}

/* --------------------------------------------------------------- patterns */

static void render_solid(RGB_color_t base, uint8_t scale, RGB_color_t *out, uint16_t n)
{
    RGB_color_t c = scale_rgb(base, scale);
    for (uint16_t i = 0; i < n; i++) {
        out[i] = c;
    }
}

/* Full spectrum smeared along the strip and rotating. Ignores the Matter colour --
 * a rainbow has no single hue to honour. */
static void render_rainbow(uint32_t t_ms, uint8_t scale, RGB_color_t *out, uint16_t n)
{
    uint32_t rotation = (t_ms % RAINBOW_CYCLE_MS) * 360 / RAINBOW_CYCLE_MS;
    for (uint16_t i = 0; i < n; i++) {
        HS_color_t hs = { (uint16_t)((rotation + (uint32_t)i * 360 / (n ? n : 1)) % 360), 100 };
        RGB_color_t rgb;
        hsv_to_rgb(hs, 100, &rgb);
        out[i] = scale_rgb(rgb, scale);
    }
}

/* Fills the strip one pixel at a time, then empties it the same way. */
static void render_colour_wipe(uint32_t t_ms, RGB_color_t base, uint8_t scale,
                               RGB_color_t *out, uint16_t n)
{
    RGB_color_t on = scale_rgb(base, scale);
    uint32_t phase = (t_ms % WIPE_CYCLE_MS) * (2u * n) / WIPE_CYCLE_MS;   /* 0 .. 2n-1 */
    bool filling  = phase < n;
    uint16_t edge = (uint16_t)(filling ? phase : phase - n);

    for (uint16_t i = 0; i < n; i++) {
        bool lit = filling ? (i <= edge) : (i > edge);
        out[i] = lit ? on : BLACK;
    }
}

/* The whole strip fades up and down together. */
static void render_breathe(uint32_t t_ms, RGB_color_t base, uint8_t scale,
                           RGB_color_t *out, uint16_t n)
{
    uint8_t ramp = triangle(t_ms, BREATHE_CYCLE_MS);
    uint8_t level = (uint8_t)(BREATHE_FLOOR + (uint16_t)(255 - BREATHE_FLOOR) * ramp / 255);
    render_solid(base, mul8(scale, level), out, n);
}

/* Each pixel pulses on its own phase, so the strip shimmers. */
static void render_twinkle(uint32_t t_ms, RGB_color_t base, uint8_t scale,
                           RGB_color_t *out, uint16_t n)
{
    for (uint16_t i = 0; i < n; i++) {
        uint32_t offset = (uint32_t)hash8(i) * TWINKLE_CYCLE_MS / 255;
        uint8_t ramp = triangle(t_ms + offset, TWINKLE_CYCLE_MS);
        /* Square the ramp so most pixels sit dim and the peaks read as sparkles. */
        out[i] = scale_rgb(base, mul8(scale, mul8(ramp, ramp)));
    }
}

/* Flickering embers. Heat comes from value noise interpolated between keyframes,
 * which keeps this stateless while still looking like it has inertia. Ignores the
 * Matter colour -- fire has its own palette. */
static void render_fire(uint32_t t_ms, uint8_t scale, RGB_color_t *out, uint16_t n)
{
    uint32_t key   = t_ms / FIRE_STEP_MS;
    uint8_t  blend = (uint8_t)((t_ms % FIRE_STEP_MS) * 255 / FIRE_STEP_MS);

    for (uint16_t i = 0; i < n; i++) {
        uint8_t a = hash8((uint32_t)i * 2654435761u + key);
        uint8_t b = hash8((uint32_t)i * 2654435761u + key + 1);
        uint16_t heat = (uint16_t)a + ((int16_t)b - (int16_t)a) * blend / 255;

        /* Cooler near the ends of the tube, so the middle reads as the core. */
        uint16_t mid  = n / 2;
        uint16_t dist = (i > mid) ? (i - mid) : (mid - i);
        uint16_t falloff = mid ? (255 - (uint16_t)(dist * 128 / mid)) : 255;
        heat = heat * falloff / 255;

        /* Palette: black -> red -> orange -> yellow-white. */
        RGB_color_t c;
        if (heat < 96) {
            c.red   = (uint8_t)(heat * 255 / 96);
            c.green = 0;
            c.blue  = 0;
        } else if (heat < 192) {
            c.red   = 255;
            c.green = (uint8_t)((heat - 96) * 170 / 96);
            c.blue  = 0;
        } else {
            c.red   = 255;
            c.green = (uint8_t)(170 + (heat - 192) * 85 / 63);
            c.blue  = (uint8_t)((heat - 192) * 80 / 63);
        }
        out[i] = scale_rgb(c, scale);
    }
}

/* Every third pixel lit, the pattern marching along the strip. */
static void render_theater_chase(uint32_t t_ms, RGB_color_t base, uint8_t scale,
                                 RGB_color_t *out, uint16_t n)
{
    RGB_color_t on = scale_rgb(base, scale);
    uint32_t step = (t_ms / CHASE_STEP_MS) % 3;
    for (uint16_t i = 0; i < n; i++) {
        out[i] = ((i + step) % 3 == 0) ? on : BLACK;
    }
}

/* ------------------------------------------------------------------ entry */

void effects_render(uint8_t effect_id, uint32_t t_ms, RGB_color_t base, uint8_t scale,
                    RGB_color_t *out, uint16_t n)
{
    if (!out || n == 0) {
        return;
    }

    switch (effect_id) {
    case EFFECT_RAINBOW:       render_rainbow(t_ms, scale, out, n);                break;
    case EFFECT_COLOUR_WIPE:   render_colour_wipe(t_ms, base, scale, out, n);      break;
    case EFFECT_BREATHE:       render_breathe(t_ms, base, scale, out, n);          break;
    case EFFECT_TWINKLE:       render_twinkle(t_ms, base, scale, out, n);          break;
    case EFFECT_FIRE:          render_fire(t_ms, scale, out, n);                   break;
    case EFFECT_THEATER_CHASE: render_theater_chase(t_ms, base, scale, out, n);    break;
    case EFFECT_SOLID:
    default:                   render_solid(base, scale, out, n);                  break;
    }
}
