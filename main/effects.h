/*
   This code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

/* Animation patterns for the strip.
 *
 * These are the modes offered through the Matter Mode Select cluster. The ids are
 * the wire values of ModeSelect::CurrentMode, so they are part of the device's
 * external interface: append new effects, never renumber existing ones.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <color_format.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    EFFECT_SOLID = 0,       /* index 0 must stay "plain light" -- see light_engine */
    EFFECT_RAINBOW,
    EFFECT_COLOUR_WIPE,
    EFFECT_BREATHE,
    EFFECT_TWINKLE,
    EFFECT_FIRE,
    EFFECT_THEATER_CHASE,
    EFFECT_COUNT,
} effect_id_t;

/** Human-readable name, also the label shown in the Mode Select cluster. */
const char *effects_name(uint8_t effect_id);

/** True if the effect changes over time and so needs a redraw every frame. */
bool effects_is_animated(uint8_t effect_id);

/** Render one frame of an effect.
 *
 * Pure: no I/O, no allocation and no state carried between calls, so a frame
 * depends only on its arguments and effects can be swapped at any moment.
 *
 * @param effect_id  one of effect_id_t; anything unknown renders as EFFECT_SOLID
 * @param t_ms       milliseconds since the effect started; drives all animation,
 *                   so speeds do not change with CONFIG_SKAFT_RENDER_FPS
 * @param base       the colour Matter asked for, at full brightness
 * @param scale      gamma-corrected brightness, 0-255, applied by every effect
 * @param out        destination buffer, n entries
 * @param n          pixel count
 */
void effects_render(uint8_t effect_id, uint32_t t_ms, RGB_color_t base, uint8_t scale,
                    RGB_color_t *out, uint16_t n);

#ifdef __cplusplus
}
#endif
