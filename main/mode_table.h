/*
   This code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

/* Backing store for the Mode Select cluster's SupportedModes attribute. */

#pragma once

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Publish the effect list to CHIP's Mode Select server.
 *
 * Must be called after the light endpoint exists (it reads light_endpoint_id) and
 * before esp_matter::start(). Without it the cluster serves an empty SupportedModes
 * list, ChangeToMode rejects everything, and Home Assistant silently declines to
 * create the select entity.
 */
esp_err_t mode_table_register(void);

#ifdef __cplusplus
}
#endif
