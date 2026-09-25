/*
   This code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <sdkconfig.h>

#include <string.h>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <driver/rmt.h>
#include <led_strip.h>

#include "strip.h"

/* Why the legacy RMT API and led_strip 1.0.0, and not driver/rmt_tx.h:
 *
 * device_hal/led_driver/ws2812/led_driver.c is compiled into this build -- the
 * esp32c6_devkit_c device config sets led_type=ws2812, and we want that component
 * anyway for color_format.c. It includes the legacy <driver/rmt.h>. ESP-IDF refuses
 * to run an image that links both RMT drivers: rmt_legacy.c has a constructor that
 * aborts at boot with "CONFLICT! driver_ng is not allowed to be used with the legacy
 * driver". So the whole app stays on the legacy driver.
 *
 * We never call led_driver_init(), so no RMT channel is taken by esp-matter and the
 * strip is the only user of the peripheral.
 */

static const char *TAG = "strip";

#define STRIP_REFRESH_TIMEOUT_MS 100

static led_strip_t      *s_strip;
static RGB_color_t       s_pixels[CONFIG_SKAFT_LED_COUNT];
static SemaphoreHandle_t s_lock;

esp_err_t strip_init(void)
{
    if (s_strip) {
        return ESP_OK;
    }

    s_lock = xSemaphoreCreateRecursiveMutex();
    if (!s_lock) {
        ESP_LOGE(TAG, "Failed to create strip mutex");
        return ESP_ERR_NO_MEM;
    }

    rmt_config_t rmt_cfg = RMT_DEFAULT_CONFIG_TX((gpio_num_t)CONFIG_SKAFT_LED_GPIO,
                                                 (rmt_channel_t)CONFIG_SKAFT_LED_RMT_CHANNEL);
    rmt_cfg.clk_div = 2;
    /* Each C6 RMT channel owns 48 symbol words and one WS281x bit costs one symbol,
     * so a single block buffers only ~60 us of output. The refill runs from an ISR
     * that competes with the 802.15.4 radio, and a late refill latches the strip
     * mid-frame -- a visible flicker. Claiming both TX blocks doubles the margin. */
    rmt_cfg.mem_block_num = CONFIG_SKAFT_LED_MEM_BLOCKS;

    esp_err_t err = rmt_config(&rmt_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_config failed: %s", esp_err_to_name(err));
        return err;
    }
    err = rmt_driver_install(rmt_cfg.channel, 0, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }

    led_strip_config_t strip_config =
        LED_STRIP_DEFAULT_CONFIG(CONFIG_SKAFT_LED_COUNT, (led_strip_dev_t)rmt_cfg.channel);
    s_strip = led_strip_new_rmt_ws2812(&strip_config);
    if (!s_strip) {
        ESP_LOGE(TAG, "led_strip_new_rmt_ws2812 failed");
        rmt_driver_uninstall(rmt_cfg.channel);
        return ESP_FAIL;
    }

    memset(s_pixels, 0, sizeof(s_pixels));
    s_strip->clear(s_strip, STRIP_REFRESH_TIMEOUT_MS);

    ESP_LOGI(TAG, "%d WS281x pixels on GPIO%d, RMT channel %d, %d memory block(s)",
             CONFIG_SKAFT_LED_COUNT, CONFIG_SKAFT_LED_GPIO,
             CONFIG_SKAFT_LED_RMT_CHANNEL, CONFIG_SKAFT_LED_MEM_BLOCKS);
    return ESP_OK;
}

uint16_t strip_pixel_count(void)
{
    return CONFIG_SKAFT_LED_COUNT;
}

void strip_lock(void)
{
    if (s_lock) {
        xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    }
}

void strip_unlock(void)
{
    if (s_lock) {
        xSemaphoreGiveRecursive(s_lock);
    }
}

void strip_set_pixel(uint16_t index, RGB_color_t colour)
{
    if (index >= CONFIG_SKAFT_LED_COUNT) {
        return;
    }
    strip_lock();
    s_pixels[index] = colour;
    strip_unlock();
}

void strip_fill(RGB_color_t colour)
{
    strip_lock();
    for (uint16_t i = 0; i < CONFIG_SKAFT_LED_COUNT; i++) {
        s_pixels[i] = colour;
    }
    strip_unlock();
}

void strip_set_all(const RGB_color_t *src)
{
    if (!src) {
        return;
    }
    strip_lock();
    memcpy(s_pixels, src, sizeof(s_pixels));
    strip_unlock();
}

esp_err_t strip_show(void)
{
    if (!s_strip) {
        return ESP_ERR_INVALID_STATE;
    }

    strip_lock();
    /* led_strip 1.0.0 packs set_pixel()'s named r/g/b arguments into the wire's
     * native GRB order for us, so nothing here reorders bytes. */
    for (uint16_t i = 0; i < CONFIG_SKAFT_LED_COUNT; i++) {
        s_strip->set_pixel(s_strip, i, s_pixels[i].red, s_pixels[i].green, s_pixels[i].blue);
    }
    esp_err_t err = s_strip->refresh(s_strip, STRIP_REFRESH_TIMEOUT_MS);
    strip_unlock();

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "refresh failed: %s", esp_err_to_name(err));
    }
    return err;
}
