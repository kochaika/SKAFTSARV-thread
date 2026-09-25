/*
   This code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

/* WS281x output layer for the SKAFTSARV's 30-LED strip.
 *
 * Owns a pixel buffer and the RMT channel that shifts it out. Everything above
 * this file works in RGB_color_t and never thinks about wire order or timing.
 */

#pragma once

#include <esp_err.h>
#include <stdint.h>

#include <color_format.h>   /* RGB_color_t, from device_hal/led_driver */

#ifdef __cplusplus
extern "C" {
#endif

/** Bring up the RMT channel and the pixel buffer. The strip is left dark. */
esp_err_t strip_init(void);

/** Number of pixels, i.e. CONFIG_SKAFT_LED_COUNT. */
uint16_t strip_pixel_count(void);

/** Take/release the buffer lock around a batch of writes.
 *
 * Every strip_* call below takes this lock on its own, so single writes need no
 * explicit locking. Wrap a group of writes plus the strip_show() that publishes
 * them when they must reach the wire in the same frame. The lock is recursive.
 */
void strip_lock(void);
void strip_unlock(void);

/** Write one pixel into the buffer. Out-of-range indices are ignored. */
void strip_set_pixel(uint16_t index, RGB_color_t colour);

/** Write every pixel in the buffer to the same colour. */
void strip_fill(RGB_color_t colour);

/** Write the whole buffer from src, which must hold strip_pixel_count() entries. */
void strip_set_all(const RGB_color_t *src);

/** Shift the buffer out to the strip. */
esp_err_t strip_show(void);

#ifdef __cplusplus
}
#endif
