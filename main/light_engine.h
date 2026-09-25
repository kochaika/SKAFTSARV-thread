/*
   This code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

/* The lamp's state model and render loop.
 *
 * Holds everything the strip is currently showing and repaints it from a task at
 * CONFIG_SKAFT_RENDER_FPS. Speaks Matter's units (level and hue/saturation on the
 * 0-254 scale, xy on the 0-65536 scale, colour temperature in kelvin) but knows
 * nothing about the Matter data model itself -- app_driver.cpp is the only file
 * that bridges the two.
 */

#pragma once

#include <esp_err.h>
#include <stdbool.h>
#include <stdint.h>

#include <color_format.h>

#include "effects.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Largest value Matter uses for level, hue and saturation. */
#define LIGHT_MATTER_MAX 254

typedef enum {
    LIGHT_COLOUR_HS = 0,
    LIGHT_COLOUR_XY,
    LIGHT_COLOUR_CT,
} light_colour_mode_t;

typedef struct {
    bool                power;
    uint8_t             level;          /* Matter 0-254 */
    light_colour_mode_t colour_mode;
    uint8_t             hue;            /* Matter 0-254 */
    uint8_t             saturation;     /* Matter 0-254 */
    uint16_t            x;
    uint16_t            y;
    uint32_t            kelvin;
    uint8_t             effect;
    const char         *effect_name;
    bool                identify;
    bool                console_override;
    RGB_color_t         base;           /* colour before brightness is applied */
    uint8_t             scale;          /* gamma-corrected brightness, 0-255 */
} light_status_t;

/** Called from the render task when the lamp wants Matter's ModeSelect::CurrentMode
 *  pushed back to 0. Registered by app_driver.cpp, which owns the data model. */
typedef void (*light_mode_reset_cb_t)(void);

/** Bring up the strip, the gamma table and the render task. */
esp_err_t light_engine_init(void);

void light_engine_register_mode_reset_cb(light_mode_reset_cb_t cb);

/* --- inputs from Matter ------------------------------------------------- */

void light_set_power(bool on);
void light_set_level(uint8_t matter_level);          /* 0-254 */
void light_set_hue(uint8_t matter_hue);              /* 0-254, selects HS mode */
void light_set_saturation(uint8_t matter_saturation);/* 0-254, selects HS mode */
void light_set_xy(uint16_t x, uint16_t y);           /* selects XY mode */
void light_set_kelvin(uint32_t kelvin);              /* selects CT mode */
void light_set_effect(uint8_t effect_id);

void light_identify_start(void);
void light_identify_stop(void);
bool light_identify_active(void);

/* --- console ------------------------------------------------------------ */

/** Hand the strip to the console: the render task stops painting until the next
 *  Matter update (or light_console_release()) takes it back. */
void light_console_override(void);
void light_console_release(void);

void light_get_status(light_status_t *out);

#ifdef __cplusplus
}
#endif
