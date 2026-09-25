/*
   This code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <string.h>

#include <esp_log.h>

#include <app/clusters/mode-select-server/supported-modes-manager.h>

#include "effects.h"
#include "mode_table.h"

/* esp-matter creates the Mode Select cluster and registers SupportedModes as
 * ATTRIBUTE_FLAG_MANAGED_INTERNALLY with an empty array
 * (esp_matter_cluster.cpp: create_supported_modes(cluster, NULL, 0, 0)). CHIP reads
 * the real list from a SupportedModesManager that esp-matter never supplies, so this
 * file provides one. The list is built from effects_name() rather than duplicated,
 * so labels cannot drift from the renderers in effects.c.
 */

using chip::CharSpan;
using chip::EndpointId;
using Status = chip::Protocols::InteractionModel::Status;

namespace ModeSelect = chip::app::Clusters::ModeSelect;
using ModeOptionType = ModeSelect::Structs::ModeOptionStruct::Type;

/* Defined in app_main.cpp. */
extern uint16_t light_endpoint_id;

static const char *TAG = "mode_table";

namespace {

ModeOptionType sModes[EFFECT_COUNT];

class EffectModesManager : public ModeSelect::SupportedModesManager
{
public:
    ModeOptionsProvider getModeOptionsProvider(EndpointId endpointId) const override
    {
        if (endpointId != light_endpoint_id) {
            return ModeOptionsProvider();   /* empty: begin() == nullptr */
        }
        return ModeOptionsProvider(&sModes[0], &sModes[EFFECT_COUNT]);
    }

    Status getModeOptionByMode(EndpointId endpointId, uint8_t mode,
                               const ModeOptionType **dataPtr) const override
    {
        if (endpointId != light_endpoint_id) {
            return Status::UnsupportedEndpoint;
        }
        for (const ModeOptionType &option : sModes) {
            if (option.mode == mode) {
                *dataPtr = &option;
                return Status::Success;
            }
        }
        return Status::InvalidCommand;
    }
};

EffectModesManager sManager;

} // namespace

esp_err_t mode_table_register(void)
{
    if (light_endpoint_id == 0) {
        ESP_LOGE(TAG, "light endpoint not created yet");
        return ESP_ERR_INVALID_STATE;
    }

    for (uint8_t i = 0; i < EFFECT_COUNT; i++) {
        /* effects_name() returns string literals, so these spans stay valid. */
        const char *label = effects_name(i);
        sModes[i].label = CharSpan(label, strlen(label));
        sModes[i].mode  = i;
        /* semanticTags stays an empty list -- the spec allows it, and none of the
         * standard tag namespaces describe "rainbow". */
    }

    ModeSelect::setSupportedModesManager(&sManager);
    ESP_LOGI(TAG, "published %d modes on endpoint %u", EFFECT_COUNT, light_endpoint_id);
    return ESP_OK;
}
