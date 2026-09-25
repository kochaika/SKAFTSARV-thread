/*
   This code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

/* Small esp_console REPL for bench-testing the strip over USB-Serial-JTAG.
 *
 * The lamp has no buttons and, once it is glued back together, no accessible ones
 * either -- this and the XIAO's BOOT button are the only local ways in.
 */

#pragma once

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Start the REPL. A no-op when CONFIG_SKAFT_ENABLE_CONSOLE is off. */
esp_err_t app_console_init(void);

#ifdef __cplusplus
}
#endif
