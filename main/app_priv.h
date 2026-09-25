/*
   This code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#pragma once

#include <sdkconfig.h>

#include <esp_err.h>
#include <esp_matter.h>

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
#include "esp_openthread_types.h"
#endif

/** Matter's maximum for level, hue and saturation. */
#define MATTER_BRIGHTNESS 254

/** Attribute values used the first time the device boots, before any fabric has
 *  stored something to restore. */
#define DEFAULT_POWER      true
#define DEFAULT_BRIGHTNESS 127   /* 50% of the Matter 0-254 range */

/* Colour temperature bounds, reported to clients as the CT slider's travel.
 *
 * Matter counts in mireds and Kelvin is their reciprocal, so the coolest colour is
 * the *minimum* mired value. The strip has no white LEDs: colour temperature is
 * approximated with tinted white via temp_to_hs(), which is itself only defined
 * between 600 K and 10000 K. */
#define SKAFT_CT_MIN_MIREDS     (1000000 / CONFIG_SKAFT_CT_MAX_KELVIN)  /* 6500 K -> 153 */
#define SKAFT_CT_MAX_MIREDS     (1000000 / CONFIG_SKAFT_CT_MIN_KELVIN)  /* 2000 K -> 500 */
#define SKAFT_CT_DEFAULT_MIREDS 250                                     /* 4000 K */

/* The lamp itself has no buttons. This is the XIAO ESP32-C6's own BOOT button,
 * reachable only with the lamp body open, and it exists purely as the factory-reset
 * escape hatch (long press, CONFIG_BUTTON_LONG_PRESS_TIME_MS). */
#define BUTTON_GPIO 9

/* FM8625H RF switch on the XIAO ESP32-C6. Never repurpose either pin. */
#define RF_SWITCH_ENABLE_GPIO   3   /* active low */
#define RF_ANTENNA_SELECT_GPIO 14   /* 0 = onboard ceramic, 1 = external U.FL */

typedef void *app_driver_handle_t;

/** Initialize the strip, the render task and the Matter glue.
 *
 * @return an opaque non-NULL handle on success, NULL on failure.
 */
app_driver_handle_t app_driver_light_init();

/** Initialize the BOOT button used for factory reset.
 *
 * @return Handle on success, NULL on failure.
 */
app_driver_handle_t app_driver_button_init();

/** Push one Matter attribute change into the light engine.
 *
 * Called from `app_attribute_update_cb()` on PRE_UPDATE.
 */
esp_err_t app_driver_attribute_update(app_driver_handle_t driver_handle, uint16_t endpoint_id,
                                      uint32_t cluster_id, uint32_t attribute_id,
                                      esp_matter_attr_val_t *val);

/** Drive the light from the attribute values the data model restored at boot. */
esp_err_t app_driver_light_set_defaults(uint16_t endpoint_id);

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
#define ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG()                                           \
    {                                                                                   \
        .radio_mode = RADIO_MODE_NATIVE,                                                \
    }

#define ESP_OPENTHREAD_DEFAULT_HOST_CONFIG()                                            \
    {                                                                                   \
        .host_connection_mode = HOST_CONNECTION_MODE_NONE,                              \
    }

#define ESP_OPENTHREAD_DEFAULT_PORT_CONFIG()                                            \
    {                                                                                   \
        .storage_partition_name = "nvs", .netif_queue_size = 10, .task_queue_size = 10, \
    }
#endif
