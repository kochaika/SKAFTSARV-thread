/*
   This code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <esp_log.h>
#include <stdlib.h>
#include <string.h>

#include <esp_matter.h>
#include <app_priv.h>
#include <common_macros.h>

#include <device.h>
#include <button_gpio.h>

#include "light_engine.h"

using namespace chip::app::Clusters;
using namespace esp_matter;

static const char *TAG = "app_driver";
extern uint16_t light_endpoint_id;

/* Something non-NULL to hand the endpoint as priv_data: esp_matter passes it back
 * as the driver handle, and app_attribute_update_cb() treats NULL as "no driver".
 * The light engine is a singleton, so there is no real per-endpoint state. */
static int s_driver_sentinel;

/* Matter's x/y arrive in separate attribute writes; hold on to the partner value so
 * a write of either one can be turned into a complete colour. */
static uint16_t s_current_x;
static uint16_t s_current_y;

/* Pushed back into the data model when a colour change cancels a running effect.
 * Invoked from the render task, so it has to take the CHIP stack lock itself. */
static void mode_reset_cb()
{
    if (light_endpoint_id == 0) {
        return;
    }
    esp_matter_attr_val_t val = esp_matter_uint8(EFFECT_SOLID);

    lock::ScopedChipStackLock guard(portMAX_DELAY);
    attribute::update(light_endpoint_id, ModeSelect::Id, ModeSelect::Attributes::CurrentMode::Id, &val);
}

esp_err_t app_driver_attribute_update(app_driver_handle_t driver_handle, uint16_t endpoint_id,
                                      uint32_t cluster_id, uint32_t attribute_id,
                                      esp_matter_attr_val_t *val)
{
    if (endpoint_id != light_endpoint_id) {
        return ESP_OK;
    }

    if (cluster_id == OnOff::Id) {
        if (attribute_id == OnOff::Attributes::OnOff::Id) {
            light_set_power(val->val.b);
        }
    } else if (cluster_id == LevelControl::Id) {
        if (attribute_id == LevelControl::Attributes::CurrentLevel::Id) {
            light_set_level(val->val.u8);
        }
    } else if (cluster_id == ColorControl::Id) {
        /* Which attribute was written picks the colour mode. ColorMode itself is not
         * usable here: at PRE_UPDATE the cluster server has not updated it yet, so it
         * still describes the *previous* colour. */
        if (attribute_id == ColorControl::Attributes::CurrentHue::Id) {
            light_set_hue(val->val.u8);
        } else if (attribute_id == ColorControl::Attributes::CurrentSaturation::Id) {
            light_set_saturation(val->val.u8);
        } else if (attribute_id == ColorControl::Attributes::ColorTemperatureMireds::Id) {
            uint16_t mireds = val->val.u16;
            if (mireds > 0) {
                light_set_kelvin(1000000u / mireds);
            }
        } else if (attribute_id == ColorControl::Attributes::CurrentX::Id) {
            s_current_x = val->val.u16;
            light_set_xy(s_current_x, s_current_y);
        } else if (attribute_id == ColorControl::Attributes::CurrentY::Id) {
            s_current_y = val->val.u16;
            light_set_xy(s_current_x, s_current_y);
        }
    } else if (cluster_id == ModeSelect::Id) {
        if (attribute_id == ModeSelect::Attributes::CurrentMode::Id) {
            light_set_effect(val->val.u8);
        }
    }

    return ESP_OK;
}

esp_err_t app_driver_light_set_defaults(uint16_t endpoint_id)
{
    esp_matter_attr_val_t val = esp_matter_invalid(NULL);
    attribute_t *attribute = NULL;

    /* Colour first, then level, then power: that order means the strip only ever
     * lights up once, already showing the right thing. */

    /* Unlike the PRE_UPDATE path, this runs after Matter has started, so ColorMode
     * is authoritative and is the only way to know which colour the data model
     * actually restored. */
    attribute = attribute::get(endpoint_id, ColorControl::Id, ColorControl::Attributes::ColorMode::Id);
    if (attribute) {
        attribute::get_val(attribute, &val);
        uint8_t colour_mode = val.val.u8;

        if (colour_mode == (uint8_t)ColorControl::ColorMode::kCurrentHueAndCurrentSaturation) {
            uint8_t hue = 0, saturation = 0;
            attribute = attribute::get(endpoint_id, ColorControl::Id, ColorControl::Attributes::CurrentHue::Id);
            if (attribute) {
                attribute::get_val(attribute, &val);
                hue = val.val.u8;
            }
            attribute = attribute::get(endpoint_id, ColorControl::Id, ColorControl::Attributes::CurrentSaturation::Id);
            if (attribute) {
                attribute::get_val(attribute, &val);
                saturation = val.val.u8;
            }
            light_set_hue(hue);
            light_set_saturation(saturation);
        } else if (colour_mode == (uint8_t)ColorControl::ColorMode::kColorTemperature) {
            attribute = attribute::get(endpoint_id, ColorControl::Id,
                                       ColorControl::Attributes::ColorTemperatureMireds::Id);
            if (attribute) {
                attribute::get_val(attribute, &val);
                if (val.val.u16 > 0) {
                    light_set_kelvin(1000000u / val.val.u16);
                }
            }
        } else if (colour_mode == (uint8_t)ColorControl::ColorMode::kCurrentXAndCurrentY) {
            attribute = attribute::get(endpoint_id, ColorControl::Id, ColorControl::Attributes::CurrentX::Id);
            if (attribute) {
                attribute::get_val(attribute, &val);
                s_current_x = val.val.u16;
            }
            attribute = attribute::get(endpoint_id, ColorControl::Id, ColorControl::Attributes::CurrentY::Id);
            if (attribute) {
                attribute::get_val(attribute, &val);
                s_current_y = val.val.u16;
            }
            light_set_xy(s_current_x, s_current_y);
        } else {
            ESP_LOGW(TAG, "Unhandled ColorMode %d, leaving the colour at its default", colour_mode);
        }
    }

    /* The effect is restored last of the colour-ish settings, because every
     * light_set_*colour* call above deliberately resets the mode to Solid. */
    attribute = attribute::get(endpoint_id, ModeSelect::Id, ModeSelect::Attributes::CurrentMode::Id);
    if (attribute) {
        attribute::get_val(attribute, &val);
        light_set_effect(val.val.u8);
    }

    attribute = attribute::get(endpoint_id, LevelControl::Id, LevelControl::Attributes::CurrentLevel::Id);
    if (attribute) {
        attribute::get_val(attribute, &val);
        light_set_level(val.val.u8);
    }

    attribute = attribute::get(endpoint_id, OnOff::Id, OnOff::Attributes::OnOff::Id);
    if (attribute) {
        attribute::get_val(attribute, &val);
        light_set_power(val.val.b);
    }

    return ESP_OK;
}

app_driver_handle_t app_driver_light_init()
{
    esp_err_t err = light_engine_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize the light engine: %s", esp_err_to_name(err));
        return NULL;
    }
    light_engine_register_mode_reset_cb(mode_reset_cb);

    /* Deliberately no led_driver_init(): device_hal's ws2812 backend is hardcoded to
     * a single pixel and would grab an RMT channel and the devkit's GPIO8 for it. */
    return (app_driver_handle_t)&s_driver_sentinel;
}

app_driver_handle_t app_driver_button_init()
{
    button_handle_t handle = NULL;
    const button_config_t btn_cfg = {0};
    button_gpio_config_t btn_gpio_cfg = button_driver_get_config();
    btn_gpio_cfg.gpio_num = BUTTON_GPIO;

    if (iot_button_new_gpio_device(&btn_cfg, &btn_gpio_cfg, &handle) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create button device");
        return NULL;
    }

    /* No toggle callback: the lamp has no user-facing button. app_reset registers the
     * long-press handler on this same device in app_main(). */
    return (app_driver_handle_t)handle;
}
