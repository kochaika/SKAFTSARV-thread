/*
   This code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <sdkconfig.h>

#include <stdio.h>
#include <string.h>

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>

#include <driver/gpio.h>

#include <esp_matter.h>
#include <esp_matter_console.h>
#include <esp_matter_ota.h>

#include <common_macros.h>
#include <log_heap_numbers.h>

#include <app_priv.h>
#include <app_reset.h>

#include "app_console.h"
#include "light_engine.h"
#include "mode_table.h"

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
#include <platform/ESP32/OpenthreadLauncher.h>
#endif

#include <app/server/CommissioningWindowManager.h>
#include <app/server/Server.h>
#include <lib/support/TypeTraits.h>

#ifdef CONFIG_ENABLE_SET_CERT_DECLARATION_API
#include <esp_matter_providers.h>
#include <lib/support/Span.h>
#ifdef CONFIG_SEC_CERT_DAC_PROVIDER
#include <platform/ESP32/ESP32SecureCertDACProvider.h>
#elif defined(CONFIG_FACTORY_PARTITION_DAC_PROVIDER)
#include <platform/ESP32/ESP32FactoryDataProvider.h>
#endif
using namespace chip::DeviceLayer;
#endif

static const char *TAG = "app_main";
uint16_t light_endpoint_id = 0;

using namespace esp_matter;
using namespace esp_matter::attribute;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

constexpr auto k_timeout_seconds = 300;

#ifdef CONFIG_ENABLE_SET_CERT_DECLARATION_API
extern const uint8_t cd_start[] asm("_binary_certification_declaration_der_start");
extern const uint8_t cd_end[] asm("_binary_certification_declaration_der_end");

const chip::ByteSpan cdSpan(cd_start, static_cast<size_t>(cd_end - cd_start));
#endif // CONFIG_ENABLE_SET_CERT_DECLARATION_API

static void app_event_cb(const ChipDeviceEvent *event, intptr_t arg)
{
    switch (event->Type) {
    case chip::DeviceLayer::DeviceEventType::kInterfaceIpAddressChanged:
        ESP_LOGI(TAG, "Interface IP Address changed");
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
        ESP_LOGI(TAG, "Commissioning complete");
        MEMORY_PROFILER_DUMP_HEAP_STAT("commissioning complete");
        break;

    case chip::DeviceLayer::DeviceEventType::kFailSafeTimerExpired:
        ESP_LOGI(TAG, "Commissioning failed, fail safe timer expired");
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningSessionStarted:
        ESP_LOGI(TAG, "Commissioning session started");
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningSessionStopped:
        ESP_LOGI(TAG, "Commissioning session stopped");
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningWindowOpened:
        ESP_LOGI(TAG, "Commissioning window opened");
        MEMORY_PROFILER_DUMP_HEAP_STAT("commissioning window opened");
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningWindowClosed:
        ESP_LOGI(TAG, "Commissioning window closed");
        break;

    case chip::DeviceLayer::DeviceEventType::kFabricRemoved: {
        ESP_LOGI(TAG, "Fabric removed successfully");
        if (chip::Server::GetInstance().GetFabricTable().FabricCount() == 0) {
            chip::CommissioningWindowManager &commissionMgr = chip::Server::GetInstance().GetCommissioningWindowManager();
            constexpr auto kTimeoutSeconds = chip::System::Clock::Seconds16(k_timeout_seconds);
            if (!commissionMgr.IsCommissioningWindowOpen()) {
                /* The device keeps its Thread credentials and so still has IP
                 * connectivity, so it only needs to advertise on DNS-SD. */
                CHIP_ERROR err = commissionMgr.OpenBasicCommissioningWindow(kTimeoutSeconds,
                                                                            chip::CommissioningWindowAdvertisement::kDnssdOnly);
                if (err != CHIP_NO_ERROR) {
                    ESP_LOGE(TAG, "Failed to open commissioning window, err:%" CHIP_ERROR_FORMAT, err.Format());
                }
            }
        }
        break;
    }

    case chip::DeviceLayer::DeviceEventType::kFabricWillBeRemoved:
        ESP_LOGI(TAG, "Fabric will be removed");
        break;

    case chip::DeviceLayer::DeviceEventType::kFabricUpdated:
        ESP_LOGI(TAG, "Fabric is updated");
        break;

    case chip::DeviceLayer::DeviceEventType::kFabricCommitted:
        ESP_LOGI(TAG, "Fabric is committed");
        break;

    case chip::DeviceLayer::DeviceEventType::kBLEDeinitialized:
        ESP_LOGI(TAG, "BLE deinitialized and memory reclaimed");
        MEMORY_PROFILER_DUMP_HEAP_STAT("BLE deinitialized");
        break;

    default:
        break;
    }
}

// Invoked when clients interact with the Identify cluster. The blink itself is
// rendered by the light engine's task, so it cannot race a frame and needs no timer.
static esp_err_t app_identification_cb(identification::callback_type_t type, uint16_t endpoint_id, uint8_t effect_id,
                                       uint8_t effect_variant, void *priv_data)
{
    ESP_LOGI(TAG, "Identification callback: type: %u, effect: %u, variant: %u", type, effect_id, effect_variant);

    switch (type) {
    case identification::START:
        light_identify_start();
        break;
    case identification::STOP:
        light_identify_stop();
        break;
    case identification::EFFECT:
        if (effect_id == chip::to_underlying(Identify::EffectIdentifierEnum::kFinishEffect) ||
            effect_id == chip::to_underlying(Identify::EffectIdentifierEnum::kStopEffect)) {
            light_identify_stop();
        } else {
            /* Blink, Breathe, Okay and ChannelChange all map to the same blink. */
            light_identify_start();
        }
        break;
    default:
        break;
    }

    return ESP_OK;
}

// Called for every attribute update. Handle the attributes of interest and return
// ESP_OK for everything else.
static esp_err_t app_attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id, uint32_t cluster_id,
                                         uint32_t attribute_id, esp_matter_attr_val_t *val, void *priv_data)
{
    esp_err_t err = ESP_OK;

    if (type == PRE_UPDATE) {
        app_driver_handle_t driver_handle = (app_driver_handle_t)priv_data;
        err = app_driver_attribute_update(driver_handle, endpoint_id, cluster_id, attribute_id, val);
    }

    return err;
}

/* Point the XIAO ESP32-C6's FM8625H RF switch at the chosen antenna. */
static void app_antenna_init(void)
{
    gpio_config_t rf_cfg = {
        .pin_bit_mask = (1ULL << RF_SWITCH_ENABLE_GPIO) | (1ULL << RF_ANTENNA_SELECT_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&rf_cfg);

    gpio_set_level((gpio_num_t)RF_SWITCH_ENABLE_GPIO, 0);   /* active low */
#if CONFIG_SKAFT_USE_EXTERNAL_ANTENNA
    gpio_set_level((gpio_num_t)RF_ANTENNA_SELECT_GPIO, 1);
    ESP_LOGI(TAG, "Radio on the external U.FL antenna");
#else
    gpio_set_level((gpio_num_t)RF_ANTENNA_SELECT_GPIO, 0);
    ESP_LOGI(TAG, "Radio on the onboard ceramic antenna");
#endif
}

static void app_nvs_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs an erase (%s), reinitialising", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

extern "C" void app_main()
{
    esp_err_t err = ESP_OK;

    app_antenna_init();
    app_nvs_init();

    MEMORY_PROFILER_DUMP_HEAP_STAT("Bootup");

    /* Initialize drivers */
    app_driver_handle_t light_handle = app_driver_light_init();
    ABORT_APP_ON_FAILURE(light_handle != NULL, ESP_LOGE(TAG, "Failed to initialize the light driver"));

    app_driver_handle_t button_handle = app_driver_button_init();
    app_reset_button_register(button_handle);

    app_console_init();

    /* Create a Matter node and add the mandatory Root Node device type on endpoint 0 */
    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    ABORT_APP_ON_FAILURE(node != nullptr, ESP_LOGE(TAG, "Failed to create Matter node"));

    MEMORY_PROFILER_DUMP_HEAP_STAT("node created");

    extended_color_light::config_t light_config;
    light_config.on_off.on_off                                 = DEFAULT_POWER;
    light_config.on_off_lighting.start_up_on_off               = nullptr;   /* restore last */
    light_config.level_control.current_level                   = DEFAULT_BRIGHTNESS;
    light_config.level_control.on_level                        = nullptr;
    light_config.level_control_lighting.start_up_current_level = DEFAULT_BRIGHTNESS;

    /* Hue/saturation at 0/0 is plain white, which is the right thing for a lamp to
     * come up as on its very first boot. */
    light_config.color_control.color_mode =
        (uint8_t)ColorControl::ColorMode::kCurrentHueAndCurrentSaturation;
    light_config.color_control.enhanced_color_mode =
        (uint8_t)ColorControl::ColorMode::kCurrentHueAndCurrentSaturation;
    /* Leave color_capabilities at 0: each feature::add() ORs its own bit in. */

    light_config.color_control_color_temperature.color_temp_physical_min_mireds        = SKAFT_CT_MIN_MIREDS;
    light_config.color_control_color_temperature.color_temp_physical_max_mireds        = SKAFT_CT_MAX_MIREDS;
    light_config.color_control_color_temperature.couple_color_temp_to_level_min_mireds = SKAFT_CT_MIN_MIREDS;
    light_config.color_control_color_temperature.color_temperature_mireds              = SKAFT_CT_DEFAULT_MIREDS;
    light_config.color_control_color_temperature.start_up_color_temperature_mireds     = nullptr;

    endpoint_t *endpoint = extended_color_light::create(node, &light_config, ENDPOINT_FLAG_NONE, light_handle);
    ABORT_APP_ON_FAILURE(endpoint != nullptr, ESP_LOGE(TAG, "Failed to create the light endpoint"));

    light_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "Light created with endpoint_id %d", light_endpoint_id);

    /* extended_color_light::add() hard-wires only ColorTemperature and XY, and offers
     * no way to ask for hue/saturation, so HS is layered on afterwards. The resulting
     * FeatureMap and ColorCapabilities are both 0x0019 (HS | XY | CT). */
    cluster_t *color_control_cluster = cluster::get(endpoint, ColorControl::Id);
    ABORT_APP_ON_FAILURE(color_control_cluster != nullptr,
                         ESP_LOGE(TAG, "Failed to find the color control cluster"));
    cluster::color_control::feature::hue_saturation::config_t hue_saturation_config;
    err = cluster::color_control::feature::hue_saturation::add(color_control_cluster, &hue_saturation_config);
    ABORT_APP_ON_FAILURE(err == ESP_OK, ESP_LOGE(TAG, "Failed to add the hue/saturation feature, err:%d", err));

    /* Effect picker. Not part of the Extended Color Light device type; Home Assistant
     * renders it as a select, Apple and Google Home ignore it. */
    cluster::mode_select::config_t mode_select_config;
    snprintf(mode_select_config.description, sizeof(mode_select_config.description), "Effect");
    mode_select_config.current_mode = EFFECT_SOLID;
    cluster_t *mode_select_cluster = cluster::mode_select::create(endpoint, &mode_select_config, CLUSTER_FLAG_SERVER);
    ABORT_APP_ON_FAILURE(mode_select_cluster != nullptr,
                         ESP_LOGE(TAG, "Failed to create the mode select cluster"));
    cluster::mode_select::command::create_change_to_mode(mode_select_cluster);

    /* SupportedModes is served from our own manager, not from the data model. Has to
     * happen before esp_matter::start(). */
    err = mode_table_register();
    ABORT_APP_ON_FAILURE(err == ESP_OK, ESP_LOGE(TAG, "Failed to publish the effect list, err:%d", err));

    /* Mark deferred persistence for the attributes a colour picker moves rapidly, so
     * dragging a slider does not hammer NVS. */
    static const struct {
        uint32_t cluster_id;
        uint32_t attribute_id;
    } deferred_attributes[] = {
        { LevelControl::Id, LevelControl::Attributes::CurrentLevel::Id },
        { ColorControl::Id, ColorControl::Attributes::CurrentHue::Id },
        { ColorControl::Id, ColorControl::Attributes::CurrentSaturation::Id },
        { ColorControl::Id, ColorControl::Attributes::ColorTemperatureMireds::Id },
        { ColorControl::Id, ColorControl::Attributes::CurrentX::Id },
        { ColorControl::Id, ColorControl::Attributes::CurrentY::Id },
    };
    for (const auto &deferred : deferred_attributes) {
        attribute_t *attr = attribute::get(light_endpoint_id, deferred.cluster_id, deferred.attribute_id);
        if (attr) {
            attribute::set_deferred_persistence(attr);
        }
    }

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
    /* Set OpenThread platform config */
    esp_openthread_platform_config_t config = {
        .radio_config = ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG(),
        .host_config = ESP_OPENTHREAD_DEFAULT_HOST_CONFIG(),
        .port_config = ESP_OPENTHREAD_DEFAULT_PORT_CONFIG(),
    };
    set_openthread_platform_config(&config);
#endif

#ifdef CONFIG_ENABLE_SET_CERT_DECLARATION_API
    auto * dac_provider = get_dac_provider();
#ifdef CONFIG_SEC_CERT_DAC_PROVIDER
    static_cast<ESP32SecureCertDACProvider *>(dac_provider)->SetCertificationDeclaration(cdSpan);
#elif defined(CONFIG_FACTORY_PARTITION_DAC_PROVIDER)
    static_cast<ESP32FactoryDataProvider *>(dac_provider)->SetCertificationDeclaration(cdSpan);
#endif
#endif // CONFIG_ENABLE_SET_CERT_DECLARATION_API

    /* Matter start */
    err = esp_matter::start(app_event_cb);
    ABORT_APP_ON_FAILURE(err == ESP_OK, ESP_LOGE(TAG, "Failed to start Matter, err:%d", err));

    MEMORY_PROFILER_DUMP_HEAP_STAT("matter started");

    /* Starting driver with default values */
    app_driver_light_set_defaults(light_endpoint_id);

#if CONFIG_ENABLE_CHIP_SHELL
    esp_matter::console::diagnostics_register_commands();
    esp_matter::console::factoryreset_register_commands();
    esp_matter::console::attribute_register_commands();
#if CONFIG_OPENTHREAD_CLI
    esp_matter::console::otcli_register_commands();
#endif
    esp_matter::console::init();
#endif

    while (true) {
        MEMORY_PROFILER_DUMP_HEAP_STAT("Idle");
        vTaskDelay(10000 / portTICK_PERIOD_MS);
    }
}
