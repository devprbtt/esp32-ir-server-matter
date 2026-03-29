#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_check.h>
#include <esp_err.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_matter.h>
#include <esp_system.h>
#include <nvs.h>
#include <nvs_flash.h>

#include <app-common/zap-generated/cluster-enums.h>
#include <app-common/zap-generated/ids/Attributes.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <app/server/CommissioningWindowManager.h>
#include <app/server/Server.h>
#include <app/clusters/mode-select-server/supported-modes-manager.h>
#include <setup_payload/OnboardingCodesUtil.h>
#include <setup_payload/QRCodeSetupPayloadGenerator.h>

#include <cJSON.h>
#include <ds18b20.h>
#include <onewire_bus.h>

#include "ir_sender.h"

namespace {

static const char *kTag = "hvac_matter";
static const char *kNvsNamespace = "hvacmatter";
static const char *kNvsConfigKey = "config";

static constexpr uint8_t kMaxHvacs = 2;
static constexpr uint8_t kMaxSensors = 4;
static constexpr size_t kMaxBodyBytes = 4096;
static constexpr uint16_t kCommissioningTimeoutSec = 300;
static constexpr int kDefaultEmitterGpio = 4;
static constexpr int kDefaultTempSensorGpio = 16;
static constexpr uint32_t kTempSensorPollMs = 5000;
static constexpr int16_t kMinSetpointCentiC = 1600;
static constexpr int16_t kMaxSetpointCentiC = 3000;
static constexpr uint8_t kProtocolModeDaikin = 0;
static constexpr uint8_t kProtocolModeGree = 1;
static constexpr uint8_t kProtocolModeMidea = 2;
static constexpr uint8_t kProtocolModeLg = 3;

struct TempSensorConfig {
    char id[16];
    char name[32];
};

struct HvacConfig {
    char id[16];
    char protocol[24];
    int16_t model;
    int emitter_gpio;
    uint8_t temp_sensor_index;
    char current_temp_source[16];
};

struct AppConfig {
    uint8_t hvac_count;
    uint8_t temp_sensor_count;
    TempSensorConfig sensors[kMaxSensors];
    HvacConfig hvacs[kMaxHvacs];
};

struct HvacState {
    bool power;
    uint8_t system_mode;
    uint8_t fan_mode;
    int16_t cooling_setpoint;
    int16_t heating_setpoint;
    int16_t local_temperature;
    esp_err_t last_send_err;
};

AppConfig g_config = {};
HvacState g_states[kMaxHvacs] = {};
uint16_t g_endpoint_ids[kMaxHvacs] = {};
httpd_handle_t g_httpd = nullptr;
onewire_bus_handle_t g_onewire_bus = nullptr;
ds18b20_device_handle_t g_ds18b20 = nullptr;
TaskHandle_t g_temp_sensor_task = nullptr;
bool g_temp_sensor_detected = false;
float g_last_sensor_temp_c = 0.0f;

using ModeSelectOption = chip::app::Clusters::ModeSelect::Structs::ModeOptionStruct::Type;
using ModeSelectSemanticTag = chip::app::Clusters::ModeSelect::Structs::SemanticTagStruct::Type;
using ModeSelectStatus = chip::Protocols::InteractionModel::Status;

const ModeSelectSemanticTag kEmptyModeSelectSemanticTags[1] = {};
const ModeSelectOption kProtocolModeOptions[] = {
    {
        .label = chip::CharSpan::fromCharString("Daikin"),
        .mode = kProtocolModeDaikin,
        .semanticTags = chip::app::DataModel::List<const ModeSelectSemanticTag>(kEmptyModeSelectSemanticTags, 0),
    },
    {
        .label = chip::CharSpan::fromCharString("Gree"),
        .mode = kProtocolModeGree,
        .semanticTags = chip::app::DataModel::List<const ModeSelectSemanticTag>(kEmptyModeSelectSemanticTags, 0),
    },
    {
        .label = chip::CharSpan::fromCharString("Midea"),
        .mode = kProtocolModeMidea,
        .semanticTags = chip::app::DataModel::List<const ModeSelectSemanticTag>(kEmptyModeSelectSemanticTags, 0),
    },
    {
        .label = chip::CharSpan::fromCharString("LG"),
        .mode = kProtocolModeLg,
        .semanticTags = chip::app::DataModel::List<const ModeSelectSemanticTag>(kEmptyModeSelectSemanticTags, 0),
    },
};

class ProtocolModeManager : public chip::app::Clusters::ModeSelect::SupportedModesManager
{
public:
    ModeOptionsProvider getModeOptionsProvider(chip::EndpointId) const override
    {
        return ModeOptionsProvider(kProtocolModeOptions,
                                   kProtocolModeOptions + (sizeof(kProtocolModeOptions) / sizeof(kProtocolModeOptions[0])));
    }

    ModeSelectStatus getModeOptionByMode(chip::EndpointId, uint8_t mode, const ModeSelectOption ** data_ptr) const override
    {
        for (const auto & option : kProtocolModeOptions) {
            if (option.mode == mode) {
                if (data_ptr) {
                    *data_ptr = &option;
                }
                return ModeSelectStatus::Success;
            }
        }
        return ModeSelectStatus::InvalidCommand;
    }
};

ProtocolModeManager g_protocol_mode_manager;

int16_t active_target_setpoint(const HvacState &state);
uint8_t normalized_fan_mode(uint8_t requested_mode, bool power);
uint8_t reported_fan_mode_for_state(const HvacState &state);

void log_onboarding_codes()
{
    chip::PayloadContents payload;
    CHIP_ERROR err = GetPayloadContents(payload, chip::RendezvousInformationFlags(chip::RendezvousInformationFlag::kBLE));
    if (err != CHIP_NO_ERROR) {
        ESP_LOGE(kTag, "Failed to build onboarding payload: %" CHIP_ERROR_FORMAT, err.Format());
        return;
    }

    char code_buffer[chip::QRCodeBasicSetupPayloadGenerator::kMaxQRCodeBase38RepresentationLength + 1] = {};
    chip::MutableCharSpan manual_code(code_buffer);
    err = GetManualPairingCode(manual_code, payload);
    if (err == CHIP_NO_ERROR) {
        ESP_LOGI(kTag, "Matter manual pairing code: %s", code_buffer);
    } else {
        ESP_LOGE(kTag, "Failed to generate manual pairing code: %" CHIP_ERROR_FORMAT, err.Format());
    }

    memset(code_buffer, 0, sizeof(code_buffer));
    chip::MutableCharSpan qr_code(code_buffer);
    err = GetQRCode(qr_code, payload);
    if (err == CHIP_NO_ERROR) {
        ESP_LOGI(kTag, "Matter QR payload: %s", code_buffer);
    } else {
        ESP_LOGE(kTag, "Failed to generate Matter QR payload: %" CHIP_ERROR_FORMAT, err.Format());
    }

    ESP_LOGI(kTag, "Matter discriminator: %u", payload.discriminator.GetLongValue());
    ESP_LOGI(kTag, "Matter setup passcode: %u", static_cast<unsigned>(payload.setUpPINCode));
}

void copy_string(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, dst_size, "%s", src);
}

void lowercase_copy(char *dst, size_t dst_size, const char *src)
{
    copy_string(dst, dst_size, src);
    for (size_t i = 0; dst[i] != '\0'; ++i) {
        dst[i] = static_cast<char>(tolower(static_cast<unsigned char>(dst[i])));
    }
}

int read_json_int(cJSON *object, const char *name, int fallback)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

const char *read_json_string(cJSON *object, const char *name)
{
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, name));
}

uint8_t clamp_u8(uint8_t value, uint8_t min_value, uint8_t max_value)
{
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}

int16_t clamp_setpoint(int value)
{
    if (value < kMinSetpointCentiC) {
        return kMinSetpointCentiC;
    }
    if (value > kMaxSetpointCentiC) {
        return kMaxSetpointCentiC;
    }
    return static_cast<int16_t>(((value + 50) / 100) * 100);
}

int16_t temperature_c_to_centi_c(float temperature_c)
{
    return static_cast<int16_t>(temperature_c * 100.0f);
}

int16_t fallback_local_temperature_for_state(const HvacState &state)
{
    return active_target_setpoint(state);
}

void refresh_local_temperature_from_source(uint8_t index)
{
    if (index >= g_config.hvac_count) {
        return;
    }

    if (g_temp_sensor_detected) {
        g_states[index].local_temperature = temperature_c_to_centi_c(g_last_sensor_temp_c);
        return;
    }

    g_states[index].local_temperature = fallback_local_temperature_for_state(g_states[index]);
}

void refresh_all_local_temperatures()
{
    for (uint8_t i = 0; i < g_config.hvac_count; ++i) {
        refresh_local_temperature_from_source(i);
    }
}

void report_local_temperature(uint8_t index)
{
    if (index >= g_config.hvac_count || g_endpoint_ids[index] == 0) {
        return;
    }

    esp_matter_attr_val_t local_temperature = esp_matter_nullable_int16(nullable<int16_t>(g_states[index].local_temperature));
    esp_matter::attribute::report(g_endpoint_ids[index], chip::app::Clusters::Thermostat::Id,
                                  chip::app::Clusters::Thermostat::Attributes::LocalTemperature::Id, &local_temperature);
}

void report_all_local_temperatures()
{
    for (uint8_t i = 0; i < g_config.hvac_count; ++i) {
        report_local_temperature(i);
    }
}

void apply_thermostat_limits(uint16_t endpoint_id)
{
    auto update_limit = [endpoint_id](uint32_t attribute_id, int16_t value) {
        esp_matter_attr_val_t attr = esp_matter_int16(value);
        esp_err_t err = esp_matter::attribute::update(endpoint_id, chip::app::Clusters::Thermostat::Id, attribute_id, &attr);
        if (err != ESP_OK) {
            ESP_LOGW(kTag, "Failed to update thermostat limit attr=0x%08lx on endpoint %u: %s",
                     static_cast<unsigned long>(attribute_id), endpoint_id, esp_err_to_name(err));
        }
    };

    update_limit(chip::app::Clusters::Thermostat::Attributes::AbsMinHeatSetpointLimit::Id, kMinSetpointCentiC);
    update_limit(chip::app::Clusters::Thermostat::Attributes::AbsMaxHeatSetpointLimit::Id, kMaxSetpointCentiC);
    update_limit(chip::app::Clusters::Thermostat::Attributes::AbsMinCoolSetpointLimit::Id, kMinSetpointCentiC);
    update_limit(chip::app::Clusters::Thermostat::Attributes::AbsMaxCoolSetpointLimit::Id, kMaxSetpointCentiC);
    update_limit(chip::app::Clusters::Thermostat::Attributes::MinHeatSetpointLimit::Id, kMinSetpointCentiC);
    update_limit(chip::app::Clusters::Thermostat::Attributes::MaxHeatSetpointLimit::Id, kMaxSetpointCentiC);
    update_limit(chip::app::Clusters::Thermostat::Attributes::MinCoolSetpointLimit::Id, kMinSetpointCentiC);
    update_limit(chip::app::Clusters::Thermostat::Attributes::MaxCoolSetpointLimit::Id, kMaxSetpointCentiC);
}

void apply_all_thermostat_limits()
{
    for (uint8_t i = 0; i < g_config.hvac_count; ++i) {
        if (g_endpoint_ids[i] != 0) {
            apply_thermostat_limits(g_endpoint_ids[i]);
        }
    }
}

uint8_t protocol_to_mode(const char *protocol)
{
    if (!protocol) {
        return kProtocolModeDaikin;
    }
    if (strcmp(protocol, "gree") == 0) {
        return kProtocolModeGree;
    }
    if (strcmp(protocol, "lg") == 0) {
        return kProtocolModeLg;
    }
    if (strcmp(protocol, "midea") == 0) {
        return kProtocolModeMidea;
    }
    return kProtocolModeDaikin;
}

const char *mode_to_protocol(uint8_t mode)
{
    switch (mode) {
    case kProtocolModeGree:
        return "gree";
    case kProtocolModeLg:
        return "lg";
    case kProtocolModeMidea:
        return "midea";
    case kProtocolModeDaikin:
    default:
        return "daikin";
    }
}

void load_default_config()
{
    memset(&g_config, 0, sizeof(g_config));
    g_config.hvac_count = 1;
    g_config.temp_sensor_count = 1;

    copy_string(g_config.sensors[0].id, sizeof(g_config.sensors[0].id), "sensor-1");
    copy_string(g_config.sensors[0].name, sizeof(g_config.sensors[0].name), "Ambient");

    copy_string(g_config.hvacs[0].id, sizeof(g_config.hvacs[0].id), "hvac-1");
    copy_string(g_config.hvacs[0].protocol, sizeof(g_config.hvacs[0].protocol), "daikin");
    g_config.hvacs[0].model = -1;
    g_config.hvacs[0].emitter_gpio = kDefaultEmitterGpio;
    g_config.hvacs[0].temp_sensor_index = 0;
    copy_string(g_config.hvacs[0].current_temp_source, sizeof(g_config.hvacs[0].current_temp_source), "sensor");
}

void load_default_states()
{
    for (uint8_t i = 0; i < kMaxHvacs; ++i) {
        g_states[i].power = false;
        g_states[i].system_mode = static_cast<uint8_t>(chip::app::Clusters::Thermostat::SystemModeEnum::kCool);
        g_states[i].fan_mode = 5;
        g_states[i].cooling_setpoint = 2400;
        g_states[i].heating_setpoint = 2000;
        g_states[i].local_temperature = fallback_local_temperature_for_state(g_states[i]);
        g_states[i].last_send_err = ESP_OK;
    }
}

char *config_to_json()
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "hvac_count", g_config.hvac_count);

    cJSON *sensors = cJSON_AddArrayToObject(root, "temp_sensors");
    for (uint8_t i = 0; i < g_config.temp_sensor_count; ++i) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "id", g_config.sensors[i].id);
        cJSON_AddStringToObject(item, "name", g_config.sensors[i].name);
        cJSON_AddItemToArray(sensors, item);
    }

    cJSON *hvacs = cJSON_AddArrayToObject(root, "hvacs");
    for (uint8_t i = 0; i < g_config.hvac_count; ++i) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "id", g_config.hvacs[i].id);
        cJSON_AddStringToObject(item, "protocol", g_config.hvacs[i].protocol);
        cJSON_AddNumberToObject(item, "model", g_config.hvacs[i].model);
        cJSON_AddNumberToObject(item, "emitter_gpio", g_config.hvacs[i].emitter_gpio);
        cJSON_AddNumberToObject(item, "temp_sensor_index", g_config.hvacs[i].temp_sensor_index);
        cJSON_AddStringToObject(item, "current_temp_source", g_config.hvacs[i].current_temp_source);
        cJSON_AddBoolToObject(item, "protocol_supported", ir_sender_is_protocol_supported(g_config.hvacs[i].protocol));
        cJSON_AddItemToArray(hvacs, item);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

bool parse_config_json(const char *json, AppConfig *out)
{
    if (!json || !out) {
        return false;
    }

    AppConfig next = {};
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        return false;
    }

    cJSON *hvacs = cJSON_GetObjectItemCaseSensitive(root, "hvacs");
    cJSON *sensors = cJSON_GetObjectItemCaseSensitive(root, "temp_sensors");
    if (!cJSON_IsArray(hvacs)) {
        cJSON_Delete(root);
        return false;
    }

    cJSON *item = nullptr;
    cJSON_ArrayForEach(item, sensors) {
        if (next.temp_sensor_count >= kMaxSensors) {
            break;
        }
        TempSensorConfig &sensor = next.sensors[next.temp_sensor_count++];
        copy_string(sensor.id, sizeof(sensor.id), read_json_string(item, "id"));
        copy_string(sensor.name, sizeof(sensor.name), read_json_string(item, "name"));
    }

    cJSON_ArrayForEach(item, hvacs) {
        if (next.hvac_count >= kMaxHvacs) {
            break;
        }
        HvacConfig &hvac = next.hvacs[next.hvac_count++];
        copy_string(hvac.id, sizeof(hvac.id), read_json_string(item, "id"));
        lowercase_copy(hvac.protocol, sizeof(hvac.protocol), read_json_string(item, "protocol"));
        hvac.model = static_cast<int16_t>(read_json_int(item, "model", -1));
        hvac.emitter_gpio = read_json_int(item, "emitter_gpio", kDefaultEmitterGpio);
        hvac.temp_sensor_index =
            clamp_u8(static_cast<uint8_t>(read_json_int(item, "temp_sensor_index", 0)), 0, kMaxSensors - 1);
        lowercase_copy(hvac.current_temp_source, sizeof(hvac.current_temp_source), read_json_string(item, "current_temp_source"));
        if (hvac.current_temp_source[0] == '\0') {
            copy_string(hvac.current_temp_source, sizeof(hvac.current_temp_source), "sensor");
        }
        if (hvac.protocol[0] == '\0') {
            copy_string(hvac.protocol, sizeof(hvac.protocol), "unknown");
        }
        if (hvac.id[0] == '\0') {
            snprintf(hvac.id, sizeof(hvac.id), "hvac-%u", next.hvac_count);
        }
    }

    if (next.hvac_count == 0) {
        cJSON_Delete(root);
        return false;
    }
    if (next.temp_sensor_count == 0) {
        next.temp_sensor_count = 1;
        copy_string(next.sensors[0].id, sizeof(next.sensors[0].id), "sensor-1");
        copy_string(next.sensors[0].name, sizeof(next.sensors[0].name), "Ambient");
    }

    *out = next;
    cJSON_Delete(root);
    return true;
}

esp_err_t save_config()
{
    char *json = config_to_json();
    if (!json) {
        return ESP_ERR_NO_MEM;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, kNvsConfigKey, json);
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
    }

    cJSON_free(json);
    return err;
}

void load_config()
{
    load_default_config();
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }

    size_t size = 0;
    esp_err_t err = nvs_get_str(handle, kNvsConfigKey, nullptr, &size);
    if (err != ESP_OK || size < 2) {
        nvs_close(handle);
        return;
    }

    char *json = static_cast<char *>(calloc(1, size));
    if (!json) {
        nvs_close(handle);
        return;
    }

    err = nvs_get_str(handle, kNvsConfigKey, json, &size);
    if (err == ESP_OK) {
        AppConfig next = {};
        if (parse_config_json(json, &next)) {
            g_config = next;
        }
    }

    free(json);
    nvs_close(handle);
}

int endpoint_index_from_id(uint16_t endpoint_id)
{
    for (uint8_t i = 0; i < g_config.hvac_count; ++i) {
        if (g_endpoint_ids[i] == endpoint_id) {
            return i;
        }
    }
    return -1;
}

int hvac_index_from_id(const char *id)
{
    if (!id) {
        return -1;
    }
    for (uint8_t i = 0; i < g_config.hvac_count; ++i) {
        if (strcmp(g_config.hvacs[i].id, id) == 0) {
            return i;
        }
    }
    return -1;
}

int16_t active_target_setpoint(const HvacState &state)
{
    switch (state.system_mode) {
    case static_cast<uint8_t>(chip::app::Clusters::Thermostat::SystemModeEnum::kHeat):
    case static_cast<uint8_t>(chip::app::Clusters::Thermostat::SystemModeEnum::kEmergencyHeat):
        return state.heating_setpoint;
    default:
        return state.cooling_setpoint;
    }
}

uint8_t normalize_system_mode_for_power(uint8_t requested_mode, bool power)
{
    using chip::app::Clusters::Thermostat::SystemModeEnum;

    if (!power) {
        return static_cast<uint8_t>(SystemModeEnum::kOff);
    }

    if (requested_mode == static_cast<uint8_t>(SystemModeEnum::kOff)) {
        return static_cast<uint8_t>(SystemModeEnum::kCool);
    }

    return requested_mode;
}

uint8_t normalized_fan_mode(uint8_t requested_mode, bool power)
{
    using chip::app::Clusters::FanControl::FanModeEnum;

    switch (requested_mode) {
    case static_cast<uint8_t>(FanModeEnum::kOn):
        return static_cast<uint8_t>(FanModeEnum::kHigh);
    case static_cast<uint8_t>(FanModeEnum::kSmart):
        return static_cast<uint8_t>(FanModeEnum::kAuto);
    case static_cast<uint8_t>(FanModeEnum::kOff):
        return power ? static_cast<uint8_t>(FanModeEnum::kAuto) : static_cast<uint8_t>(FanModeEnum::kOff);
    default:
        return requested_mode;
    }
}

uint8_t reported_fan_mode_for_state(const HvacState &state)
{
    using chip::app::Clusters::FanControl::FanModeEnum;
    using chip::app::Clusters::Thermostat::SystemModeEnum;

    if (!state.power || state.system_mode == static_cast<uint8_t>(SystemModeEnum::kOff)) {
        return static_cast<uint8_t>(FanModeEnum::kOff);
    }

    return normalized_fan_mode(state.fan_mode, true);
}

uint8_t thermostat_running_mode_for_state(const HvacState &state)
{
    using chip::app::Clusters::Thermostat::SystemModeEnum;
    using chip::app::Clusters::Thermostat::ThermostatRunningModeEnum;

    if (!state.power || state.system_mode == static_cast<uint8_t>(SystemModeEnum::kOff)) {
        return static_cast<uint8_t>(ThermostatRunningModeEnum::kOff);
    }

    if (state.system_mode == static_cast<uint8_t>(SystemModeEnum::kHeat) ||
        state.system_mode == static_cast<uint8_t>(SystemModeEnum::kEmergencyHeat)) {
        return static_cast<uint8_t>(ThermostatRunningModeEnum::kHeat);
    }

    return static_cast<uint8_t>(ThermostatRunningModeEnum::kCool);
}

uint16_t thermostat_running_state_for_state(const HvacState &state)
{
    using chip::app::Clusters::Thermostat::RelayStateBitmap;
    using chip::app::Clusters::Thermostat::SystemModeEnum;

    if (!state.power || state.system_mode == static_cast<uint8_t>(SystemModeEnum::kOff)) {
        return 0;
    }

    if (state.system_mode == static_cast<uint8_t>(SystemModeEnum::kHeat) ||
        state.system_mode == static_cast<uint8_t>(SystemModeEnum::kEmergencyHeat)) {
        return static_cast<uint16_t>(RelayStateBitmap::kHeat) | static_cast<uint16_t>(RelayStateBitmap::kFan);
    }

    return static_cast<uint16_t>(RelayStateBitmap::kCool) | static_cast<uint16_t>(RelayStateBitmap::kFan);
}

void sync_linked_matter_state(uint16_t endpoint_id, const HvacState &state, bool report_on_off, bool report_system_mode)
{
    if (endpoint_id == 0) {
        return;
    }

    if (report_on_off) {
        esp_matter_attr_val_t onoff = esp_matter_bool(state.power);
        esp_matter::attribute::report(endpoint_id, chip::app::Clusters::OnOff::Id,
                                      chip::app::Clusters::OnOff::Attributes::OnOff::Id, &onoff);
    }

    if (report_system_mode) {
        esp_matter_attr_val_t mode = esp_matter_enum8(state.system_mode);
        esp_matter::attribute::report(endpoint_id, chip::app::Clusters::Thermostat::Id,
                                      chip::app::Clusters::Thermostat::Attributes::SystemMode::Id, &mode);
    }

    if (esp_matter::attribute::get(endpoint_id, chip::app::Clusters::Thermostat::Id,
                                   chip::app::Clusters::Thermostat::Attributes::ThermostatRunningMode::Id)) {
        esp_matter_attr_val_t running_mode = esp_matter_enum8(thermostat_running_mode_for_state(state));
        esp_matter::attribute::report(endpoint_id, chip::app::Clusters::Thermostat::Id,
                                      chip::app::Clusters::Thermostat::Attributes::ThermostatRunningMode::Id,
                                      &running_mode);
    }

    if (esp_matter::attribute::get(endpoint_id, chip::app::Clusters::Thermostat::Id,
                                   chip::app::Clusters::Thermostat::Attributes::ThermostatRunningState::Id)) {
        esp_matter_attr_val_t running_state = esp_matter_bitmap16(thermostat_running_state_for_state(state));
        esp_matter::attribute::report(endpoint_id, chip::app::Clusters::Thermostat::Id,
                                      chip::app::Clusters::Thermostat::Attributes::ThermostatRunningState::Id,
                                      &running_state);
    }

    if (esp_matter::attribute::get(endpoint_id, chip::app::Clusters::FanControl::Id,
                                   chip::app::Clusters::FanControl::Attributes::FanMode::Id)) {
        esp_matter_attr_val_t fan_mode = esp_matter_enum8(reported_fan_mode_for_state(state));
        esp_matter::attribute::report(endpoint_id, chip::app::Clusters::FanControl::Id,
                                      chip::app::Clusters::FanControl::Attributes::FanMode::Id, &fan_mode);
    }
}

esp_err_t dispatch_hvac_ir(uint8_t index, const char *reason)
{
    if (index >= g_config.hvac_count) {
        return ESP_ERR_INVALID_ARG;
    }

    hvac_ir_command_t command = {
        .protocol = g_config.hvacs[index].protocol,
        .model = g_config.hvacs[index].model,
        .emitter_gpio = g_config.hvacs[index].emitter_gpio,
        .power = g_states[index].power,
        .system_mode = g_states[index].system_mode,
        .fan_mode = g_states[index].fan_mode,
        .target_temperature_centi_c = active_target_setpoint(g_states[index]),
    };

    const esp_err_t err = ir_sender_send(&command);
    g_states[index].last_send_err = err;
    ESP_LOGI(kTag,
             "send reason=%s hvac=%s endpoint=%u protocol=%s emitter_gpio=%d power=%d mode=%u cool=%.2f heat=%.2f fan=%u err=%s",
             reason ? reason : "unknown", g_config.hvacs[index].id, g_endpoint_ids[index], g_config.hvacs[index].protocol,
             g_config.hvacs[index].emitter_gpio, g_states[index].power, g_states[index].system_mode,
             g_states[index].cooling_setpoint / 100.0, g_states[index].heating_setpoint / 100.0, g_states[index].fan_mode,
             esp_err_to_name(err));
    return err;
}

esp_err_t send_json_response(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json ? json : "{}");
}

esp_err_t config_get_handler(httpd_req_t *req)
{
    char *json = config_to_json();
    esp_err_t err = send_json_response(req, json);
    if (json) {
        cJSON_free(json);
    }
    return err;
}

esp_err_t status_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "hvac_count", g_config.hvac_count);

    cJSON *protocols = cJSON_AddArrayToObject(root, "supported_protocols");
    cJSON_AddItemToArray(protocols, cJSON_CreateString(mode_to_protocol(kProtocolModeDaikin)));
    cJSON_AddItemToArray(protocols, cJSON_CreateString(mode_to_protocol(kProtocolModeGree)));
    cJSON_AddItemToArray(protocols, cJSON_CreateString(mode_to_protocol(kProtocolModeMidea)));
    cJSON_AddItemToArray(protocols, cJSON_CreateString(mode_to_protocol(kProtocolModeLg)));

    cJSON *hvacs = cJSON_AddArrayToObject(root, "hvacs");
    for (uint8_t i = 0; i < g_config.hvac_count; ++i) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "id", g_config.hvacs[i].id);
        cJSON_AddNumberToObject(item, "endpoint_id", g_endpoint_ids[i]);
        cJSON_AddStringToObject(item, "protocol", g_config.hvacs[i].protocol);
        cJSON_AddNumberToObject(item, "model", g_config.hvacs[i].model);
        cJSON_AddNumberToObject(item, "emitter_gpio", g_config.hvacs[i].emitter_gpio);
        cJSON_AddBoolToObject(item, "protocol_supported", ir_sender_is_protocol_supported(g_config.hvacs[i].protocol));
        cJSON_AddBoolToObject(item, "power", g_states[i].power);
        cJSON_AddNumberToObject(item, "system_mode", g_states[i].system_mode);
        cJSON_AddNumberToObject(item, "fan_mode", g_states[i].fan_mode);
        cJSON_AddNumberToObject(item, "cooling_setpoint_c", g_states[i].cooling_setpoint / 100);
        cJSON_AddNumberToObject(item, "heating_setpoint_c", g_states[i].heating_setpoint / 100);
        cJSON_AddNumberToObject(item, "local_temperature_c", g_states[i].local_temperature / 100.0);
        cJSON_AddStringToObject(item, "current_temp_source", g_config.hvacs[i].current_temp_source);
        cJSON_AddNumberToObject(item, "temp_sensor_index", g_config.hvacs[i].temp_sensor_index);
        cJSON_AddNumberToObject(item, "temp_sensor_gpio", kDefaultTempSensorGpio);
        cJSON_AddBoolToObject(item, "temp_sensor_detected", g_temp_sensor_detected);
        cJSON_AddStringToObject(item, "last_send_error", esp_err_to_name(g_states[i].last_send_err));
        cJSON_AddItemToArray(hvacs, item);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    esp_err_t err = send_json_response(req, json);
    if (json) {
        cJSON_free(json);
    }
    return err;
}

esp_err_t config_post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > static_cast<int>(kMaxBodyBytes)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"error\":\"invalid_body\"}");
    }

    char *body = static_cast<char *>(calloc(1, req->content_len + 1));
    if (!body) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"oom\"}");
    }

    int offset = 0;
    while (offset < req->content_len) {
        const int read = httpd_req_recv(req, body + offset, req->content_len - offset);
        if (read <= 0) {
            free(body);
            httpd_resp_set_status(req, "400 Bad Request");
            return httpd_resp_sendstr(req, "{\"error\":\"recv_failed\"}");
        }
        offset += read;
    }

    AppConfig next = {};
    if (!parse_config_json(body, &next)) {
        free(body);
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"error\":\"invalid_json\"}");
    }

    g_config = next;
    esp_err_t err = save_config();
    free(body);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"save_failed\"}");
    }

    return httpd_resp_sendstr(req, "{\"ok\":true,\"restart_required\":true}");
}

esp_err_t hvac_send_post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > static_cast<int>(kMaxBodyBytes)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"error\":\"invalid_body\"}");
    }

    char *body = static_cast<char *>(calloc(1, req->content_len + 1));
    if (!body) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"oom\"}");
    }

    int offset = 0;
    while (offset < req->content_len) {
        const int read = httpd_req_recv(req, body + offset, req->content_len - offset);
        if (read <= 0) {
            free(body);
            httpd_resp_set_status(req, "400 Bad Request");
            return httpd_resp_sendstr(req, "{\"error\":\"recv_failed\"}");
        }
        offset += read;
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"error\":\"invalid_json\"}");
    }

    const int index = hvac_index_from_id(read_json_string(root, "id"));
    if (index < 0) {
        cJSON_Delete(root);
        httpd_resp_set_status(req, "404 Not Found");
        return httpd_resp_sendstr(req, "{\"error\":\"unknown_hvac\"}");
    }

    HvacState next = g_states[index];
    cJSON *item = nullptr;

    item = cJSON_GetObjectItemCaseSensitive(root, "power");
    if (cJSON_IsBool(item)) {
        next.power = cJSON_IsTrue(item);
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "system_mode");
    if (cJSON_IsNumber(item)) {
        next.system_mode = static_cast<uint8_t>(item->valueint);
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "fan_mode");
    if (cJSON_IsNumber(item)) {
        next.fan_mode = static_cast<uint8_t>(item->valueint);
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "cooling_setpoint_centi_c");
    if (cJSON_IsNumber(item)) {
        next.cooling_setpoint = clamp_setpoint(item->valueint);
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "heating_setpoint_centi_c");
    if (cJSON_IsNumber(item)) {
        next.heating_setpoint = clamp_setpoint(item->valueint);
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "target_temperature_centi_c");
    if (cJSON_IsNumber(item)) {
        const int16_t target = clamp_setpoint(item->valueint);
        if (next.system_mode == static_cast<uint8_t>(chip::app::Clusters::Thermostat::SystemModeEnum::kHeat) ||
            next.system_mode == static_cast<uint8_t>(chip::app::Clusters::Thermostat::SystemModeEnum::kEmergencyHeat)) {
            next.heating_setpoint = target;
        } else {
            next.cooling_setpoint = target;
        }
    }

    g_states[index] = next;
    const esp_err_t err = dispatch_hvac_ir(static_cast<uint8_t>(index), "http");
    cJSON_Delete(root);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", err == ESP_OK);
    cJSON_AddStringToObject(resp, "id", g_config.hvacs[index].id);
    cJSON_AddStringToObject(resp, "protocol", g_config.hvacs[index].protocol);
    cJSON_AddNumberToObject(resp, "emitter_gpio", g_config.hvacs[index].emitter_gpio);
    cJSON_AddStringToObject(resp, "error", esp_err_to_name(err));
    cJSON_AddNumberToObject(resp, "active_target_c", active_target_setpoint(g_states[index]) / 100);
    char *json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    esp_err_t send_err = send_json_response(req, json);
    if (json) {
        cJSON_free(json);
    }
    return send_err;
}

void start_http_server()
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 10;

    if (httpd_start(&g_httpd, &config) != ESP_OK) {
        ESP_LOGW(kTag, "HTTP server start failed");
        return;
    }

    httpd_uri_t config_get = {
        .uri = "/api/config",
        .method = HTTP_GET,
        .handler = config_get_handler,
        .user_ctx = nullptr,
    };
    httpd_register_uri_handler(g_httpd, &config_get);

    httpd_uri_t config_post = {
        .uri = "/api/config",
        .method = HTTP_POST,
        .handler = config_post_handler,
        .user_ctx = nullptr,
    };
    httpd_register_uri_handler(g_httpd, &config_post);

    httpd_uri_t status_get = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = status_get_handler,
        .user_ctx = nullptr,
    };
    httpd_register_uri_handler(g_httpd, &status_get);

    httpd_uri_t hvac_send_post = {
        .uri = "/api/hvac/send",
        .method = HTTP_POST,
        .handler = hvac_send_post_handler,
        .user_ctx = nullptr,
    };
    httpd_register_uri_handler(g_httpd, &hvac_send_post);
}

static void app_event_cb(const ChipDeviceEvent *event, intptr_t)
{
    switch (event->Type) {
    case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
        ESP_LOGI(kTag, "Matter commissioning complete");
        break;
    case chip::DeviceLayer::DeviceEventType::kCommissioningWindowOpened:
        ESP_LOGI(kTag, "Commissioning window opened");
        log_onboarding_codes();
        break;
    case chip::DeviceLayer::DeviceEventType::kCommissioningWindowClosed:
        ESP_LOGI(kTag, "Commissioning window closed");
        break;
    case chip::DeviceLayer::DeviceEventType::kFabricRemoved:
        ESP_LOGI(kTag, "Fabric removed");
        if (chip::Server::GetInstance().GetFabricTable().FabricCount() == 0) {
            auto &mgr = chip::Server::GetInstance().GetCommissioningWindowManager();
            if (!mgr.IsCommissioningWindowOpen()) {
                mgr.OpenBasicCommissioningWindow(chip::System::Clock::Seconds16(kCommissioningTimeoutSec),
                                                 chip::CommissioningWindowAdvertisement::kDnssdOnly);
            }
        }
        break;
    default:
        break;
    }
}

static esp_err_t app_identification_cb(esp_matter::identification::callback_type_t, uint16_t endpoint_id, uint8_t effect_id,
                                       uint8_t effect_variant, void *)
{
    ESP_LOGI(kTag, "Identify endpoint=%u effect=%u variant=%u", endpoint_id, effect_id, effect_variant);
    return ESP_OK;
}

static esp_err_t app_attribute_update_cb(esp_matter::attribute::callback_type_t type, uint16_t endpoint_id, uint32_t cluster_id,
                                         uint32_t attribute_id, esp_matter_attr_val_t *val, void *)
{
    if (type != esp_matter::attribute::PRE_UPDATE) {
        return ESP_OK;
    }

    const int index = endpoint_index_from_id(endpoint_id);
    if (index < 0) {
        return ESP_OK;
    }

    HvacState next = g_states[index];

    if (cluster_id == chip::app::Clusters::OnOff::Id &&
        attribute_id == chip::app::Clusters::OnOff::Attributes::OnOff::Id) {
        next.power = val->val.b;
        next.system_mode = normalize_system_mode_for_power(next.system_mode, next.power);
        next.fan_mode = normalized_fan_mode(next.fan_mode, next.power);
    } else if (cluster_id == chip::app::Clusters::Thermostat::Id &&
               attribute_id == chip::app::Clusters::Thermostat::Attributes::SystemMode::Id) {
        next.system_mode = static_cast<uint8_t>(val->val.u8);
        next.power = next.system_mode != static_cast<uint8_t>(chip::app::Clusters::Thermostat::SystemModeEnum::kOff);
        next.system_mode = normalize_system_mode_for_power(next.system_mode, next.power);
        next.fan_mode = normalized_fan_mode(next.fan_mode, next.power);
    } else if (cluster_id == chip::app::Clusters::Thermostat::Id &&
               attribute_id == chip::app::Clusters::Thermostat::Attributes::OccupiedCoolingSetpoint::Id) {
        next.cooling_setpoint = clamp_setpoint(val->val.i16);
        val->val.i16 = next.cooling_setpoint;
    } else if (cluster_id == chip::app::Clusters::Thermostat::Id &&
               attribute_id == chip::app::Clusters::Thermostat::Attributes::OccupiedHeatingSetpoint::Id) {
        next.heating_setpoint = clamp_setpoint(val->val.i16);
        val->val.i16 = next.heating_setpoint;
    } else if (cluster_id == chip::app::Clusters::FanControl::Id &&
               attribute_id == chip::app::Clusters::FanControl::Attributes::FanMode::Id) {
        next.fan_mode = normalized_fan_mode(static_cast<uint8_t>(val->val.u8), next.power);
        val->val.u8 = next.fan_mode;
    } else if (cluster_id == chip::app::Clusters::ModeSelect::Id &&
               attribute_id == chip::app::Clusters::ModeSelect::Attributes::CurrentMode::Id) {
        copy_string(g_config.hvacs[index].protocol, sizeof(g_config.hvacs[index].protocol), mode_to_protocol(val->val.u8));
        ESP_RETURN_ON_ERROR(save_config(), kTag, "Failed to persist protocol selection for %s", g_config.hvacs[index].id);
        ESP_LOGI(kTag, "Protocol mode changed for %s endpoint=%u mode=%u protocol=%s", g_config.hvacs[index].id, endpoint_id,
                 val->val.u8, g_config.hvacs[index].protocol);
    } else {
        return ESP_OK;
    }

    g_states[index] = next;
    refresh_local_temperature_from_source(static_cast<uint8_t>(index));
    sync_linked_matter_state(endpoint_id, next,
                             cluster_id == chip::app::Clusters::Thermostat::Id &&
                                 attribute_id == chip::app::Clusters::Thermostat::Attributes::SystemMode::Id,
                             cluster_id == chip::app::Clusters::OnOff::Id &&
                                 attribute_id == chip::app::Clusters::OnOff::Attributes::OnOff::Id);
    report_local_temperature(static_cast<uint8_t>(index));
    return dispatch_hvac_ir(static_cast<uint8_t>(index), "matter");
}

void temperature_sensor_task(void *)
{
    while (true) {
        bool detected = false;
        float temperature_c = 0.0f;

        if (g_ds18b20) {
            if (ds18b20_trigger_temperature_conversion(g_ds18b20) == ESP_OK &&
                ds18b20_get_temperature(g_ds18b20, &temperature_c) == ESP_OK) {
                detected = true;
                g_last_sensor_temp_c = temperature_c;
            }
        }

        const bool sensor_state_changed = (g_temp_sensor_detected != detected);
        g_temp_sensor_detected = detected;
        refresh_all_local_temperatures();
        report_all_local_temperatures();

        if (g_temp_sensor_detected) {
            ESP_LOGI(kTag, "DS18B20 temperature=%.2fC gpio=%d", g_last_sensor_temp_c, kDefaultTempSensorGpio);
        } else if (sensor_state_changed) {
            ESP_LOGW(kTag, "No DS18B20 detected on GPIO %d, using setpoint fallback for local temperature", kDefaultTempSensorGpio);
        }

        vTaskDelay(pdMS_TO_TICKS(kTempSensorPollMs));
    }
}

void init_temperature_sensor()
{
    onewire_bus_config_t bus_config = {
        .bus_gpio_num = kDefaultTempSensorGpio,
        .flags = {
            .en_pull_up = true,
        },
    };
    onewire_bus_rmt_config_t rmt_config = {
        .max_rx_bytes = 10,
    };

    esp_err_t err = onewire_new_bus_rmt(&bus_config, &rmt_config, &g_onewire_bus);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "Failed to initialize 1-Wire bus on GPIO %d: %s", kDefaultTempSensorGpio, esp_err_to_name(err));
        return;
    }

    onewire_device_iter_handle_t iter = nullptr;
    err = onewire_new_device_iter(g_onewire_bus, &iter);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "Failed to create DS18B20 iterator on GPIO %d: %s", kDefaultTempSensorGpio, esp_err_to_name(err));
        onewire_bus_del(g_onewire_bus);
        g_onewire_bus = nullptr;
        return;
    }

    onewire_device_t device;
    err = onewire_device_iter_get_next(iter, &device);
    if (err == ESP_OK) {
        ds18b20_config_t ds18b20_config = {};
        err = ds18b20_new_device(&device, &ds18b20_config, &g_ds18b20);
        if (err == ESP_OK) {
            ds18b20_set_resolution(g_ds18b20, DS18B20_RESOLUTION_12B);
            ESP_LOGI(kTag, "DS18B20 detected on GPIO %d address=%016llX", kDefaultTempSensorGpio, device.address);
        } else {
            ESP_LOGW(kTag, "1-Wire device on GPIO %d is not a DS18B20: %s", kDefaultTempSensorGpio, esp_err_to_name(err));
        }
    } else if (err == ESP_ERR_NOT_FOUND) {
        ESP_LOGW(kTag, "No DS18B20 detected on GPIO %d", kDefaultTempSensorGpio);
    } else {
        ESP_LOGW(kTag, "DS18B20 search failed on GPIO %d: %s", kDefaultTempSensorGpio, esp_err_to_name(err));
    }

    if (iter) {
        onewire_del_device_iter(iter);
    }

    if (!g_ds18b20) {
        onewire_bus_del(g_onewire_bus);
        g_onewire_bus = nullptr;
        ESP_LOGI(kTag, "Released 1-Wire bus on GPIO %d because no DS18B20 is active", kDefaultTempSensorGpio);
        return;
    }

    xTaskCreate(temperature_sensor_task, "temp_sensor", 4096, nullptr, 5, &g_temp_sensor_task);
}

void create_hvac_endpoints(esp_matter::node_t *node)
{
    for (uint8_t i = 0; i < g_config.hvac_count; ++i) {
        esp_matter::endpoint::room_air_conditioner::config_t config = {};
        config.on_off.on_off = g_states[i].power;
        config.thermostat.local_temperature = g_states[i].local_temperature;
        config.thermostat.features.cooling.occupied_cooling_setpoint = g_states[i].cooling_setpoint;
        config.thermostat.features.heating.occupied_heating_setpoint = g_states[i].heating_setpoint;
        config.thermostat.control_sequence_of_operation =
            static_cast<uint8_t>(chip::app::Clusters::Thermostat::ControlSequenceOfOperationEnum::kCoolingAndHeating);
        config.thermostat.feature_flags |= esp_matter::cluster::thermostat::feature::cooling::get_id();
        config.thermostat.feature_flags |= esp_matter::cluster::thermostat::feature::heating::get_id();
        config.thermostat.system_mode = g_states[i].system_mode;

        esp_matter::endpoint_t *endpoint =
            esp_matter::endpoint::room_air_conditioner::create(node, &config, esp_matter::ENDPOINT_FLAG_NONE, nullptr);
        if (!endpoint) {
            ESP_LOGE(kTag, "Failed to create HVAC endpoint %u", i);
            continue;
        }

        esp_matter::cluster::fan_control::config_t fan_control_config = {};
        fan_control_config.fan_mode = reported_fan_mode_for_state(g_states[i]);
        fan_control_config.fan_mode_sequence =
            static_cast<uint8_t>(chip::app::Clusters::FanControl::FanModeSequenceEnum::kOffLowMedHighAuto);
        fan_control_config.percent_setting = nullable<uint8_t>();
        fan_control_config.percent_current = 0;
        esp_matter::cluster_t *fan_cluster =
            esp_matter::cluster::fan_control::create(endpoint, &fan_control_config, esp_matter::CLUSTER_FLAG_SERVER);
        if (!fan_cluster) {
            ESP_LOGW(kTag, "Failed to create Fan Control cluster for endpoint %u", esp_matter::endpoint::get_id(endpoint));
        } else {
            esp_matter::cluster::fan_control::feature::fan_auto::add(fan_cluster);
        }

        esp_matter::cluster::mode_select::config_t mode_select_config = {};
        copy_string(mode_select_config.mode_select_description, sizeof(mode_select_config.mode_select_description), "Protocol");
        mode_select_config.current_mode = protocol_to_mode(g_config.hvacs[i].protocol);
        mode_select_config.delegate = &g_protocol_mode_manager;
        if (!esp_matter::cluster::mode_select::create(endpoint, &mode_select_config, esp_matter::CLUSTER_FLAG_SERVER)) {
            ESP_LOGW(kTag, "Failed to create Mode Select cluster for endpoint %u", esp_matter::endpoint::get_id(endpoint));
        }

        g_endpoint_ids[i] = esp_matter::endpoint::get_id(endpoint);
        ESP_LOGI(kTag, "Created HVAC endpoint %u for %s using protocol=%s emitter_gpio=%d", g_endpoint_ids[i],
                 g_config.hvacs[i].id, g_config.hvacs[i].protocol, g_config.hvacs[i].emitter_gpio);
    }
}

}  // namespace

extern "C" void app_main()
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    load_default_states();
    load_config();
    refresh_all_local_temperatures();

    esp_matter::node::config_t node_config;
    esp_matter::node_t *node = esp_matter::node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    ESP_ERROR_CHECK(node ? ESP_OK : ESP_FAIL);

    create_hvac_endpoints(node);

    err = esp_matter::start(app_event_cb);
    ESP_ERROR_CHECK(err);

    apply_all_thermostat_limits();
    init_temperature_sensor();

    log_onboarding_codes();

    start_http_server();
    ESP_LOGI(kTag, "HTTP backend ready at /api/config, /api/status, and /api/hvac/send");
    ESP_LOGI(kTag, "GPIO selection: ir_emitter=%d temp_sensor=%d", g_config.hvacs[0].emitter_gpio, kDefaultTempSensorGpio);
}
