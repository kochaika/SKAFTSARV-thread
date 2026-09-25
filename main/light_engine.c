/*
   This code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <sdkconfig.h>

#include <inttypes.h>
#include <math.h>
#include <string.h>

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "light_engine.h"
#include "strip.h"

static const char *TAG = "light";

#define RENDER_TASK_STACK    4096
/* Below OpenThread and lwIP: the radio must always win. Jitter in an animation is
 * invisible, a dropped 802.15.4 deadline is not. */
#define RENDER_TASK_PRIORITY 2

#define IDENTIFY_HALF_PERIOD_MS 500   /* ~1 Hz square blink */

static SemaphoreHandle_t s_lock;
static bool              s_started;

/* --- state, all guarded by s_lock --------------------------------------- */
static bool                s_power       = true;
static uint8_t             s_level       = LIGHT_MATTER_MAX / 2;  /* matches
                                            DEFAULT_BRIGHTNESS until Matter
                                            restores the real value */
static light_colour_mode_t s_colour_mode = LIGHT_COLOUR_HS;
static uint8_t             s_hue;
static uint8_t             s_saturation;
static uint16_t            s_x;
static uint16_t            s_y;
static uint32_t            s_kelvin      = 4000;
static uint8_t             s_effect      = EFFECT_SOLID;
static bool                s_identify;
static bool                s_console_override;
static bool                s_dirty       = true;
static bool                s_mode_reset_pending;
static uint32_t            s_effect_start_ms;

static light_mode_reset_cb_t s_mode_reset_cb;

/* Render-task private. */
static RGB_color_t s_frame[CONFIG_SKAFT_LED_COUNT];

/* Brightness -> PWM curve. A WS281x driven linearly wastes most of its 8 bits at
 * the top: the bottom of the Matter range collapses into a handful of visible
 * steps. The LUT is what makes the lamp usable below ~20%. */
static uint8_t s_gamma[256];

static inline uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void gamma_table_init(void)
{
    const float g = (float)CONFIG_SKAFT_GAMMA_X10 / 10.0f;
    s_gamma[0] = 0;
    for (int i = 1; i < 256; i++) {
        int v = (int)(255.0f * powf((float)i / 255.0f, g) + 0.5f);
        /* Never let a non-zero level round away to darkness -- Matter level 1 has
         * to show something, or clients think the light failed to turn on. */
        s_gamma[i] = (uint8_t)(v < 1 ? 1 : v);
    }
}

/* --- state helpers (call with s_lock held) ------------------------------ */

static void mark_dirty(void)
{
    s_dirty = true;
}

/* Any colour change drops the lamp back to a plain solid light. Apple Home and
 * Google Home do not render the Mode Select cluster, so without this a user on
 * those ecosystems could start an effect from Home Assistant and then have no way
 * to get out of it. Level and on/off deliberately leave the effect running. */
static void colour_touched(void)
{
    if (s_effect != EFFECT_SOLID) {
        s_effect = EFFECT_SOLID;
        s_effect_start_ms = now_ms();
        s_mode_reset_pending = true;
    }
    s_console_override = false;
    mark_dirty();
}

static RGB_color_t base_colour(void)
{
    RGB_color_t rgb = {0, 0, 0};

    switch (s_colour_mode) {
    case LIGHT_COLOUR_HS: {
        /* hsv_to_rgb() wants degrees and percent, not Matter's 0-254 scale, and
         * treats brightness as 0-100 -- see device_hal/led_driver/utils/color_format.c.
         * Ask it for full brightness and dim separately. */
        HS_color_t hs = {
            .hue        = (uint16_t)((uint32_t)s_hue * 360 / LIGHT_MATTER_MAX),
            .saturation = (uint8_t)((uint32_t)s_saturation * 100 / LIGHT_MATTER_MAX),
        };
        hsv_to_rgb(hs, 100, &rgb);
        break;
    }
    case LIGHT_COLOUR_CT: {
        /* The strip has no white LEDs, so colour temperature is a tinted white. */
        HS_color_t hs;
        temp_to_hs(s_kelvin, &hs);
        hsv_to_rgb(hs, 100, &rgb);
        break;
    }
    case LIGHT_COLOUR_XY: {
        XY_color_t xy = { .x = s_x, .y = s_y };
        xy_to_rgb(xy, 255, &rgb);   /* 255 -> chromaticity only */
        break;
    }
    }
    return rgb;
}

static uint8_t level_scale(void)
{
    if (!s_power || s_level == 0) {
        return 0;
    }
    uint32_t linear = (uint32_t)s_level * 255 / LIGHT_MATTER_MAX;
    if (linear > 255) {
        linear = 255;
    }
    return s_gamma[linear];
}

/* --- render task -------------------------------------------------------- */

static void render_task(void *arg)
{
    const TickType_t period = pdMS_TO_TICKS(1000 / CONFIG_SKAFT_RENDER_FPS);
    TickType_t last_wake = xTaskGetTickCount();

    while (true) {
        vTaskDelayUntil(&last_wake, period > 0 ? period : 1);

        /* Snapshot and clear the edge-triggered flags in one critical section, so a
         * setter running mid-frame cannot have its redraw swallowed. */
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool     override     = s_console_override;
        bool     dirty        = s_dirty;
        bool     identify     = s_identify;
        uint8_t  effect       = s_effect;
        uint32_t elapsed      = now_ms() - s_effect_start_ms;
        bool     reset_mode   = s_mode_reset_pending;
        RGB_color_t base      = base_colour();
        uint8_t  scale        = level_scale();
        s_dirty = false;
        s_mode_reset_pending = false;
        xSemaphoreGive(s_lock);

        if (reset_mode && s_mode_reset_cb) {
            /* Runs here, not in the attribute callback that triggered it: writing an
             * attribute from inside attribute::update()'s own callback re-enters the
             * data model. */
            s_mode_reset_cb();
        }

        if (override) {
            continue;   /* the console owns the strip */
        }

        bool animated = identify || effects_is_animated(effect);
        if (!animated && !dirty) {
            /* A solid colour is sent once. No reason to re-transmit 30 pixels
             * CONFIG_SKAFT_RENDER_FPS times a second at a lamp that is not moving. */
            continue;
        }

        if (identify) {
            /* Blink the whole strip at the current colour, or at full brightness if
             * the lamp is off -- identify has to be visible either way. */
            uint8_t on_scale = scale ? scale : 255;
            uint8_t blink = ((elapsed / IDENTIFY_HALF_PERIOD_MS) % 2) ? 0 : on_scale;
            effects_render(EFFECT_SOLID, elapsed, base, blink, s_frame, CONFIG_SKAFT_LED_COUNT);
        } else {
            effects_render(effect, elapsed, base, scale, s_frame, CONFIG_SKAFT_LED_COUNT);
        }

        strip_lock();
        strip_set_all(s_frame);
        strip_show();
        strip_unlock();
    }
}

/* --- public API --------------------------------------------------------- */

esp_err_t light_engine_init(void)
{
    if (s_started) {
        return ESP_OK;
    }

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        ESP_LOGE(TAG, "Failed to create light mutex");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = strip_init();
    if (err != ESP_OK) {
        return err;
    }

    gamma_table_init();
    s_effect_start_ms = now_ms();

    if (xTaskCreate(render_task, "led_render", RENDER_TASK_STACK, NULL,
                    RENDER_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create render task");
        return ESP_ERR_NO_MEM;
    }

    s_started = true;
    ESP_LOGI(TAG, "Render task at %d fps, gamma %d.%d",
             CONFIG_SKAFT_RENDER_FPS, CONFIG_SKAFT_GAMMA_X10 / 10, CONFIG_SKAFT_GAMMA_X10 % 10);
    return ESP_OK;
}

void light_engine_register_mode_reset_cb(light_mode_reset_cb_t cb)
{
    s_mode_reset_cb = cb;
}

void light_set_power(bool on)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_power = on;
    s_console_override = false;
    mark_dirty();
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "power %s", on ? "on" : "off");
}

void light_set_level(uint8_t matter_level)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_level = matter_level > LIGHT_MATTER_MAX ? LIGHT_MATTER_MAX : matter_level;
    s_console_override = false;
    mark_dirty();
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "level %d", matter_level);
}

void light_set_hue(uint8_t matter_hue)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_hue = matter_hue;
    s_colour_mode = LIGHT_COLOUR_HS;
    colour_touched();
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "hue %d", matter_hue);
}

void light_set_saturation(uint8_t matter_saturation)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_saturation = matter_saturation;
    s_colour_mode = LIGHT_COLOUR_HS;
    colour_touched();
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "saturation %d", matter_saturation);
}

void light_set_xy(uint16_t x, uint16_t y)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_x = x;
    s_y = y;
    s_colour_mode = LIGHT_COLOUR_XY;
    colour_touched();
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "xy %u %u", x, y);
}

void light_set_kelvin(uint32_t kelvin)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_kelvin = kelvin;
    s_colour_mode = LIGHT_COLOUR_CT;
    colour_touched();
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "colour temperature %" PRIu32 " K", kelvin);
}

void light_set_effect(uint8_t effect_id)
{
    if (effect_id >= EFFECT_COUNT) {
        ESP_LOGW(TAG, "unknown effect %d, ignoring", effect_id);
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (effect_id != s_effect) {
        s_effect = effect_id;
        s_effect_start_ms = now_ms();
    }
    s_console_override = false;
    mark_dirty();
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "effect %d (%s)", effect_id, effects_name(effect_id));
}

void light_identify_start(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_identify = true;
    s_console_override = false;
    s_effect_start_ms = now_ms();   /* start the blink on a rising edge */
    mark_dirty();
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "identify started");
}

void light_identify_stop(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_identify = false;
    s_effect_start_ms = now_ms();
    mark_dirty();
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "identify stopped");
}

bool light_identify_active(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool active = s_identify;
    xSemaphoreGive(s_lock);
    return active;
}

void light_console_override(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool was = s_console_override;
    s_console_override = true;
    xSemaphoreGive(s_lock);
    if (!was) {
        ESP_LOGW(TAG, "console is driving the strip; the next Matter update takes it back");
    }
}

void light_console_release(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_console_override = false;
    mark_dirty();
    xSemaphoreGive(s_lock);
}

void light_get_status(light_status_t *out)
{
    if (!out) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    out->power            = s_power;
    out->level            = s_level;
    out->colour_mode      = s_colour_mode;
    out->hue              = s_hue;
    out->saturation       = s_saturation;
    out->x                = s_x;
    out->y                = s_y;
    out->kelvin           = s_kelvin;
    out->effect           = s_effect;
    out->effect_name      = effects_name(s_effect);
    out->identify         = s_identify;
    out->console_override = s_console_override;
    out->base             = base_colour();
    out->scale            = level_scale();
    xSemaphoreGive(s_lock);
}
