/*
   This code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <sdkconfig.h>
#include "app_console.h"

#if CONFIG_SKAFT_ENABLE_CONSOLE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <esp_console.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_matter.h>

#include "light_engine.h"
#include "strip.h"

/* C++ rather than C only because `factoryreset` calls esp_matter::factory_reset(). */

static const char *TAG = "console";

static const char *COLOUR_MODE_NAMES[] = { "hue/saturation", "xy", "colour temperature" };

/* ------------------------------------------------------------- helpers */

static bool parse_u32(const char *s, uint32_t max, uint32_t *out)
{
    char *end = NULL;
    long v = strtol(s, &end, 0);
    if (end == s || *end != '\0' || v < 0 || (uint32_t)v > max) {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

static void show_console_frame(void)
{
    light_console_override();
    strip_show();
}

/* ------------------------------------------------------------ commands */

static int cmd_status(int argc, char **argv)
{
    light_status_t st;
    light_get_status(&st);

    printf("power       : %s\n", st.power ? "on" : "off");
    printf("level       : %d/%d  (scaled %d/255)\n", st.level, LIGHT_MATTER_MAX, st.scale);
    printf("colour mode : %s\n", COLOUR_MODE_NAMES[st.colour_mode]);
    printf("  hue/sat   : %d / %d\n", st.hue, st.saturation);
    printf("  xy        : %u / %u\n", st.x, st.y);
    printf("  kelvin    : %lu\n", (unsigned long)st.kelvin);
    printf("base colour : R%d G%d B%d\n", st.base.red, st.base.green, st.base.blue);
    printf("effect      : %d (%s)\n", st.effect, st.effect_name);
    printf("identify    : %s\n", st.identify ? "active" : "idle");
    printf("strip       : %d pixels%s\n", strip_pixel_count(),
           st.console_override ? ", driven by the console" : "");
    return 0;
}

static int cmd_rgb(int argc, char **argv)
{
    uint32_t r, g, b;
    if (argc != 4 || !parse_u32(argv[1], 255, &r) || !parse_u32(argv[2], 255, &g) ||
        !parse_u32(argv[3], 255, &b)) {
        printf("usage: rgb <0-255> <0-255> <0-255>\n");
        return 1;
    }
    RGB_color_t c = { (uint8_t)r, (uint8_t)g, (uint8_t)b };
    strip_lock();
    strip_fill(c);
    show_console_frame();
    strip_unlock();
    printf("whole strip R%lu G%lu B%lu\n", (unsigned long)r, (unsigned long)g, (unsigned long)b);
    return 0;
}

static int cmd_pixel(int argc, char **argv)
{
    uint32_t i, r, g, b;
    if (argc != 5 || !parse_u32(argv[1], strip_pixel_count() - 1, &i) ||
        !parse_u32(argv[2], 255, &r) || !parse_u32(argv[3], 255, &g) || !parse_u32(argv[4], 255, &b)) {
        printf("usage: pixel <0-%d> <r> <g> <b>\n", strip_pixel_count() - 1);
        return 1;
    }
    RGB_color_t c = { (uint8_t)r, (uint8_t)g, (uint8_t)b };
    strip_lock();
    strip_set_pixel((uint16_t)i, c);
    show_console_frame();
    strip_unlock();
    return 0;
}

static int cmd_clear(int argc, char **argv)
{
    RGB_color_t off = {0, 0, 0};
    strip_lock();
    strip_fill(off);
    show_console_frame();
    strip_unlock();
    return 0;
}

static int cmd_level(int argc, char **argv)
{
    uint32_t level;
    if (argc != 2 || !parse_u32(argv[1], LIGHT_MATTER_MAX, &level)) {
        printf("usage: level <0-%d>\n", LIGHT_MATTER_MAX);
        return 1;
    }
    /* Goes through the engine, so this also hands the strip back from any earlier
     * rgb/pixel/clear command. Matter's own CurrentLevel is untouched and will
     * overwrite this on the next update from a controller. */
    light_set_level((uint8_t)level);
    return 0;
}

static int cmd_selftest(int argc, char **argv)
{
    static const RGB_color_t STEPS[] = {
        {255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 255},
    };
    static const char *NAMES[] = { "red", "green", "blue", "white" };
    const uint16_t n = strip_pixel_count();
    const RGB_color_t off = {0, 0, 0};

    light_console_override();
    printf("walking %d pixels. If 'red' shows green, the wire order is not GRB.\n", n);

    for (size_t s = 0; s < sizeof(STEPS) / sizeof(STEPS[0]); s++) {
        printf("  %s\n", NAMES[s]);
        for (uint16_t i = 0; i < n; i++) {
            strip_lock();
            strip_fill(off);
            strip_set_pixel(i, STEPS[s]);
            strip_show();
            strip_unlock();
            vTaskDelay(pdMS_TO_TICKS(40));
        }
        strip_lock();
        strip_fill(STEPS[s]);
        strip_show();
        strip_unlock();
        vTaskDelay(pdMS_TO_TICKS(400));
    }

    strip_lock();
    strip_fill(off);
    strip_show();
    strip_unlock();
    printf("done. 'level <n>' or any Matter update hands the strip back.\n");
    return 0;
}

static int cmd_release(int argc, char **argv)
{
    light_console_release();
    printf("strip handed back to Matter\n");
    return 0;
}

static int cmd_factoryreset(int argc, char **argv)
{
    printf("Factory resetting: Thread credentials and all fabrics will be erased.\n");
    esp_matter::factory_reset();
    return 0;
}

/* --------------------------------------------------------- registration */

static void register_command(const char *command, const char *help, const char *hint,
                             esp_console_cmd_func_t func)
{
    esp_console_cmd_t cmd = {};
    cmd.command = command;
    cmd.help    = help;
    cmd.hint    = hint;
    cmd.func    = func;
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

esp_err_t app_console_init(void)
{
    esp_console_repl_t *repl = NULL;

    /* Fields set explicitly rather than via ESP_CONSOLE_REPL_CONFIG_DEFAULT(), whose
     * designated initializers are a GNU extension in C++17. */
    esp_console_repl_config_t repl_config = {};
    repl_config.max_history_len    = 16;
    repl_config.history_save_path  = NULL;
    repl_config.task_stack_size    = 4096;
    repl_config.task_priority      = 2;
    repl_config.task_core_id       = tskNO_AFFINITY;
    repl_config.prompt             = "skaft>";
    repl_config.max_cmdline_length = 128;

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    esp_console_dev_usb_serial_jtag_config_t dev_config = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    esp_err_t err = esp_console_new_repl_usb_serial_jtag(&dev_config, &repl_config, &repl);
#else
    esp_console_dev_uart_config_t dev_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    esp_err_t err = esp_console_new_repl_uart(&dev_config, &repl_config, &repl);
#endif
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create the REPL: %s", esp_err_to_name(err));
        return err;
    }

    esp_console_register_help_command();
    register_command("status",       "Show the lamp's current state",                  NULL,                     cmd_status);
    register_command("rgb",          "Paint the whole strip one colour",               "<r> <g> <b>",            cmd_rgb);
    register_command("pixel",        "Paint a single pixel",                           "<index> <r> <g> <b>",    cmd_pixel);
    register_command("clear",        "Blank the strip",                                NULL,                     cmd_clear);
    register_command("level",        "Set brightness and hand the strip back",         "<0-254>",                cmd_level);
    register_command("selftest",     "Walk R/G/B/W along the strip to check count and wire order", NULL,          cmd_selftest);
    register_command("release",      "Hand the strip back to Matter",                  NULL,                     cmd_release);
    register_command("factoryreset", "Erase Thread credentials and all fabrics",       NULL,                     cmd_factoryreset);

    err = esp_console_start_repl(repl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start the REPL: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Debug console ready, type 'help'");
    return ESP_OK;
}

#else /* !CONFIG_SKAFT_ENABLE_CONSOLE */

esp_err_t app_console_init(void)
{
    return ESP_OK;
}

#endif /* CONFIG_SKAFT_ENABLE_CONSOLE */
