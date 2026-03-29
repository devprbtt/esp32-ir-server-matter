#include "ir_sender.h"

#include <ctype.h>
#include <string.h>

#include <driver/rmt_encoder.h>
#include <driver/rmt_tx.h>
#include <esp_check.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace {

static const char *kTag = "ir_sender";
static constexpr uint32_t kResolutionHz = 1000000;
static constexpr uint32_t kCarrierHz = 38000;
static constexpr size_t kDaikinStateLength = 35;
static constexpr size_t kDaikinSymbolCount = 292;
static constexpr size_t kGreeStateLength = 8;
static constexpr size_t kGreeSymbolCount = 70;
static constexpr size_t kMideaSymbolCount = 100;

static constexpr uint16_t kDaikinHdrMark = 3650;
static constexpr uint16_t kDaikinHdrSpace = 1623;
static constexpr uint16_t kDaikinBitMark = 428;
static constexpr uint16_t kDaikinZeroSpace = 428;
static constexpr uint16_t kDaikinOneSpace = 1280;
static constexpr uint16_t kDaikinGap = 29000;
static constexpr uint16_t kGreeHdrMark = 9000;
static constexpr uint16_t kGreeHdrSpace = 4500;
static constexpr uint16_t kGreeBitMark = 620;
static constexpr uint16_t kGreeOneSpace = 1600;
static constexpr uint16_t kGreeZeroSpace = 540;
static constexpr uint16_t kGreeMsgSpace = 19980;
static constexpr uint16_t kMideaBitMark = 560;
static constexpr uint16_t kMideaOneSpace = 1680;
static constexpr uint16_t kMideaZeroSpace = 560;
static constexpr uint16_t kMideaHdrMark = 4480;
static constexpr uint16_t kMideaHdrSpace = 4480;
static constexpr uint16_t kMideaMinGap = 5600;

static constexpr uint8_t kDaikinAuto = 0b000;
static constexpr uint8_t kDaikinDry = 0b010;
static constexpr uint8_t kDaikinCool = 0b011;
static constexpr uint8_t kDaikinHeat = 0b100;
static constexpr uint8_t kDaikinFan = 0b110;
static constexpr uint8_t kDaikinFanAuto = 0xA;
static constexpr uint8_t kDaikinSwingOff = 0x0;
static constexpr uint8_t kGreeAuto = 0;
static constexpr uint8_t kGreeCool = 1;
static constexpr uint8_t kGreeDry = 2;
static constexpr uint8_t kGreeFan = 3;
static constexpr uint8_t kGreeHeat = 4;
static constexpr uint8_t kGreeFanAuto = 0;
static constexpr uint8_t kGreeFanMin = 1;
static constexpr uint8_t kGreeFanMed = 2;
static constexpr uint8_t kGreeFanMax = 3;
static constexpr uint8_t kGreeSwingAuto = 0x1;
static constexpr uint8_t kGreeSwingHOff = 0x0;
static constexpr uint8_t kMideaACCool = 0;
static constexpr uint8_t kMideaACDry = 1;
static constexpr uint8_t kMideaACAuto = 2;
static constexpr uint8_t kMideaACHeat = 3;
static constexpr uint8_t kMideaACFan = 4;
static constexpr uint8_t kMideaACFanAuto = 0;
static constexpr uint8_t kMideaACFanLow = 1;
static constexpr uint8_t kMideaACFanMed = 2;
static constexpr uint8_t kMideaACFanHigh = 3;

struct EmitterRuntime {
    int gpio = -1;
    rmt_channel_handle_t channel = nullptr;
    rmt_encoder_handle_t encoder = nullptr;
};

EmitterRuntime g_emitter = {};
SemaphoreHandle_t g_emitters_mutex = nullptr;

SemaphoreHandle_t get_emitters_mutex()
{
    if (!g_emitters_mutex) {
        g_emitters_mutex = xSemaphoreCreateMutex();
    }
    return g_emitters_mutex;
}

void reset_emitter(EmitterRuntime &slot)
{
    if (slot.channel) {
        esp_err_t err = rmt_disable(slot.channel);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(kTag, "rmt_disable failed during emitter reset: %s", esp_err_to_name(err));
        }
    }
    if (slot.encoder) {
        esp_err_t err = rmt_del_encoder(slot.encoder);
        if (err != ESP_OK) {
            ESP_LOGW(kTag, "rmt_del_encoder failed during emitter reset: %s", esp_err_to_name(err));
        }
    }
    if (slot.channel) {
        esp_err_t err = rmt_del_channel(slot.channel);
        if (err != ESP_OK) {
            ESP_LOGW(kTag, "rmt_del_channel failed during emitter reset: %s", esp_err_to_name(err));
        }
    }
    slot.gpio = -1;
    slot.channel = nullptr;
    slot.encoder = nullptr;
}

bool protocol_equals(const char *lhs, const char *rhs)
{
    if (!lhs || !rhs) {
        return false;
    }
    while (*lhs != '\0' && *rhs != '\0') {
        if (tolower(static_cast<unsigned char>(*lhs)) != tolower(static_cast<unsigned char>(*rhs))) {
            return false;
        }
        ++lhs;
        ++rhs;
    }
    return *lhs == '\0' && *rhs == '\0';
}

uint8_t sum_bytes(const uint8_t *data, size_t length)
{
    uint8_t total = 0;
    for (size_t i = 0; i < length; ++i) {
        total = static_cast<uint8_t>(total + data[i]);
    }
    return total;
}

int centi_c_to_daikin_temp(int16_t centi_c)
{
    int temp_c = centi_c / 100;
    if (temp_c < 10) {
        temp_c = 10;
    }
    if (temp_c > 32) {
        temp_c = 32;
    }
    return temp_c;
}

uint8_t matter_mode_to_daikin(uint8_t system_mode)
{
    switch (system_mode) {
    case 0:
    case 1:
        return kDaikinAuto;
    case 3:
    case 6:
        return kDaikinCool;
    case 4:
    case 5:
        return kDaikinHeat;
    case 7:
        return kDaikinFan;
    case 8:
    case 9:
        return kDaikinDry;
    default:
        return kDaikinAuto;
    }
}

uint8_t matter_fan_to_daikin(uint8_t fan_mode)
{
    switch (fan_mode) {
    case 1:
        return 3;
    case 2:
        return 5;
    case 3:
    case 4:
        return 7;
    case 5:
    case 6:
        return kDaikinFanAuto;
    default:
        return kDaikinFanAuto;
    }
}

int centi_c_to_gree_temp(int16_t centi_c)
{
    int temp_c = centi_c / 100;
    if (temp_c < 16) {
        temp_c = 16;
    }
    if (temp_c > 30) {
        temp_c = 30;
    }
    return temp_c;
}

uint8_t matter_mode_to_gree(uint8_t system_mode)
{
    switch (system_mode) {
    case 0:
    case 1:
        return kGreeAuto;
    case 3:
    case 6:
        return kGreeCool;
    case 4:
    case 5:
        return kGreeHeat;
    case 7:
        return kGreeFan;
    case 8:
    case 9:
        return kGreeDry;
    default:
        return kGreeAuto;
    }
}

uint8_t matter_fan_to_gree(uint8_t fan_mode)
{
    switch (fan_mode) {
    case 1:
        return kGreeFanMin;
    case 2:
        return kGreeFanMed;
    case 3:
    case 4:
        return kGreeFanMax;
    case 5:
    case 6:
        return kGreeFanAuto;
    default:
        return kGreeFanAuto;
    }
}

uint8_t matter_mode_to_midea(uint8_t system_mode)
{
    switch (system_mode) {
    case 0:
    case 1:
        return kMideaACAuto;
    case 3:
    case 6:
        return kMideaACCool;
    case 4:
    case 5:
        return kMideaACHeat;
    case 7:
        return kMideaACFan;
    case 8:
    case 9:
        return kMideaACDry;
    default:
        return kMideaACAuto;
    }
}

uint8_t matter_fan_to_midea(uint8_t fan_mode)
{
    switch (fan_mode) {
    case 1:
        return kMideaACFanLow;
    case 2:
        return kMideaACFanMed;
    case 3:
    case 4:
        return kMideaACFanHigh;
    case 5:
    case 6:
        return kMideaACFanAuto;
    default:
        return kMideaACFanAuto;
    }
}

void fill_daikin_state(const hvac_ir_command_t *command, uint8_t state[kDaikinStateLength])
{
    static const uint8_t kBaseState[kDaikinStateLength] = {
        0x11, 0xDA, 0x27, 0x00, 0xC5, 0x00, 0x00, 0x00, 0x11, 0xDA, 0x27, 0x00,
        0x42, 0x00, 0x00, 0x00, 0x11, 0xDA, 0x27, 0x00, 0x00, 0x49, 0x1E, 0x00,
        0xB0, 0x00, 0x00, 0x06, 0x60, 0x00, 0x00, 0xC0, 0x00, 0x00, 0x00,
    };

    memcpy(state, kBaseState, sizeof(kBaseState));

    const uint8_t mode = matter_mode_to_daikin(command->system_mode);
    const uint8_t fan = matter_fan_to_daikin(command->fan_mode);
    const int target_c = centi_c_to_daikin_temp(command->target_temperature_centi_c);

    state[21] = static_cast<uint8_t>((state[21] & 0x88) | (command->power ? 0x01 : 0x00) | ((mode & 0x07) << 4));
    state[22] = static_cast<uint8_t>(target_c * 2);
    state[24] = static_cast<uint8_t>((fan << 4) | kDaikinSwingOff);
    state[25] = static_cast<uint8_t>((state[25] & 0xF0) | kDaikinSwingOff);

    state[7] = sum_bytes(state, 7);
    state[15] = sum_bytes(state + 8, 7);
    state[34] = sum_bytes(state + 16, 18);
}

uint8_t calc_kelvinator_style_checksum(const uint8_t *block, size_t length)
{
    uint8_t sum = 10;
    for (size_t i = 0; i < 4 && i < length - 1; ++i) {
        sum = static_cast<uint8_t>(sum + (block[i] & 0x0F));
    }
    for (size_t i = 4; i < length - 1; ++i) {
        sum = static_cast<uint8_t>(sum + (block[i] >> 4));
    }
    return sum & 0x0F;
}

void fill_gree_state(const hvac_ir_command_t *command, uint8_t state[kGreeStateLength])
{
    memset(state, 0, kGreeStateLength);

    uint8_t mode = matter_mode_to_gree(command->system_mode);
    uint8_t fan = matter_fan_to_gree(command->fan_mode);
    int target_c = centi_c_to_gree_temp(command->target_temperature_centi_c);

    if (mode == kGreeAuto) {
        target_c = 25;
    }
    if (mode == kGreeDry) {
        fan = kGreeFanMin;
    }

    state[0] = static_cast<uint8_t>((mode & 0x07) | ((command->power ? 1U : 0U) << 3) | ((fan & 0x03) << 4) |
                                    (1U << 6));
    state[1] = static_cast<uint8_t>(target_c - 16);
    const bool model_a = command->model != 1;
    state[2] = static_cast<uint8_t>((1U << 5) | ((command->power && model_a) ? (1U << 6) : 0U));
    state[3] = 0x50;
    state[4] = static_cast<uint8_t>(kGreeSwingAuto | (kGreeSwingHOff << 4));
    state[5] = 0x20;
    state[6] = 0x00;
    state[7] = static_cast<uint8_t>(calc_kelvinator_style_checksum(state, kGreeStateLength) << 4);
}

uint8_t reverse_bits8(uint8_t value)
{
    value = static_cast<uint8_t>(((value & 0xF0U) >> 4) | ((value & 0x0FU) << 4));
    value = static_cast<uint8_t>(((value & 0xCCU) >> 2) | ((value & 0x33U) << 2));
    value = static_cast<uint8_t>(((value & 0xAAU) >> 1) | ((value & 0x55U) << 1));
    return value;
}

uint8_t celsius_to_midea_fahrenheit(int16_t centi_c)
{
    int fahrenheit = ((centi_c * 9) + 16000 + 250) / 500;
    if (fahrenheit < 62) {
        fahrenheit = 62;
    }
    if (fahrenheit > 86) {
        fahrenheit = 86;
    }
    return static_cast<uint8_t>(fahrenheit);
}

uint8_t calc_midea_checksum(const uint8_t state[6])
{
    uint16_t sum = 0;
    for (uint8_t i = 0; i < 5; ++i) {
        sum = static_cast<uint16_t>(sum + reverse_bits8(state[i]));
    }
    return reverse_bits8(static_cast<uint8_t>(256 - (sum & 0xFF)));
}

void fill_midea_state(const hvac_ir_command_t *command, uint8_t state[6])
{
    const uint8_t mode = matter_mode_to_midea(command->system_mode);
    const uint8_t fan = matter_fan_to_midea(command->fan_mode);
    const uint8_t temp_f = celsius_to_midea_fahrenheit(command->target_temperature_centi_c);

    state[0] = 0xA1;
    state[1] = static_cast<uint8_t>(0x80 | ((command->power ? 1U : 0U) << 1) | ((fan & 0x03U) << 3) | (mode & 0x07U));
    state[2] = static_cast<uint8_t>(0x60 | (temp_f - 62));
    state[3] = 0xFF;
    state[4] = 0xFF;
    state[5] = calc_midea_checksum(state);
}

void append_symbol(rmt_symbol_word_t *symbols, size_t *offset, size_t capacity, uint16_t mark_us, uint16_t space_us)
{
    if (*offset >= capacity) {
        return;
    }
    symbols[*offset] = {
        .duration0 = mark_us,
        .level0 = 1,
        .duration1 = space_us,
        .level1 = 0,
    };
    ++(*offset);
}

void append_bits_lsb_first(rmt_symbol_word_t *symbols, size_t *offset, size_t capacity, const uint8_t *data, size_t length)
{
    for (size_t i = 0; i < length; ++i) {
        for (uint8_t bit = 0; bit < 8; ++bit) {
            const bool one = ((data[i] >> bit) & 0x1U) != 0;
            append_symbol(symbols, offset, capacity, kDaikinBitMark, one ? kDaikinOneSpace : kDaikinZeroSpace);
        }
    }
}

void append_bits_msb_first(rmt_symbol_word_t *symbols, size_t *offset, size_t capacity, const uint8_t *data, size_t length,
                           uint16_t mark_us, uint16_t one_space_us, uint16_t zero_space_us)
{
    for (size_t i = 0; i < length; ++i) {
        for (int bit = 7; bit >= 0; --bit) {
            const bool one = ((data[i] >> bit) & 0x1U) != 0;
            append_symbol(symbols, offset, capacity, mark_us, one ? one_space_us : zero_space_us);
        }
    }
}

size_t build_daikin_symbols(const uint8_t state[kDaikinStateLength], rmt_symbol_word_t *symbols, size_t capacity)
{
    size_t offset = 0;

    for (uint8_t i = 0; i < 5; ++i) {
        append_symbol(symbols, &offset, capacity, kDaikinBitMark, kDaikinZeroSpace);
    }
    append_symbol(symbols, &offset, capacity, kDaikinBitMark, static_cast<uint16_t>(kDaikinZeroSpace + kDaikinGap));

    append_symbol(symbols, &offset, capacity, kDaikinHdrMark, kDaikinHdrSpace);
    append_bits_lsb_first(symbols, &offset, capacity, state, 8);
    append_symbol(symbols, &offset, capacity, kDaikinBitMark, static_cast<uint16_t>(kDaikinZeroSpace + kDaikinGap));

    append_symbol(symbols, &offset, capacity, kDaikinHdrMark, kDaikinHdrSpace);
    append_bits_lsb_first(symbols, &offset, capacity, state + 8, 8);
    append_symbol(symbols, &offset, capacity, kDaikinBitMark, static_cast<uint16_t>(kDaikinZeroSpace + kDaikinGap));

    append_symbol(symbols, &offset, capacity, kDaikinHdrMark, kDaikinHdrSpace);
    append_bits_lsb_first(symbols, &offset, capacity, state + 16, 19);
    append_symbol(symbols, &offset, capacity, kDaikinBitMark, static_cast<uint16_t>(kDaikinZeroSpace + kDaikinGap));

    return offset;
}

size_t build_gree_symbols(const uint8_t state[kGreeStateLength], rmt_symbol_word_t *symbols, size_t capacity)
{
    size_t offset = 0;

    append_symbol(symbols, &offset, capacity, kGreeHdrMark, kGreeHdrSpace);
    append_bits_lsb_first(symbols, &offset, capacity, state, 4);

    append_symbol(symbols, &offset, capacity, kGreeBitMark, kGreeZeroSpace);
    append_symbol(symbols, &offset, capacity, kGreeBitMark, kGreeOneSpace);
    append_symbol(symbols, &offset, capacity, kGreeBitMark, kGreeZeroSpace);
    append_symbol(symbols, &offset, capacity, kGreeBitMark, kGreeMsgSpace);

    append_bits_lsb_first(symbols, &offset, capacity, state + 4, 4);
    append_symbol(symbols, &offset, capacity, kGreeBitMark, kGreeMsgSpace);

    return offset;
}

size_t build_midea_symbols(const uint8_t state[6], rmt_symbol_word_t *symbols, size_t capacity)
{
    size_t offset = 0;
    uint8_t inverted[6] = {};

    for (uint8_t i = 0; i < 6; ++i) {
        inverted[i] = static_cast<uint8_t>(~state[i]);
    }

    append_symbol(symbols, &offset, capacity, kMideaHdrMark, kMideaHdrSpace);
    append_bits_msb_first(symbols, &offset, capacity, state, 6, kMideaBitMark, kMideaOneSpace, kMideaZeroSpace);
    append_symbol(symbols, &offset, capacity, kMideaBitMark, kMideaMinGap);

    append_symbol(symbols, &offset, capacity, kMideaHdrMark, kMideaHdrSpace);
    append_bits_msb_first(symbols, &offset, capacity, inverted, 6, kMideaBitMark, kMideaOneSpace, kMideaZeroSpace);
    append_symbol(symbols, &offset, capacity, kMideaBitMark, kMideaMinGap);

    return offset;
}

EmitterRuntime *get_or_create_emitter(int gpio)
{
    if (gpio < 0) {
        return nullptr;
    }

    if (g_emitter.gpio == gpio) {
        if (!g_emitter.channel || !g_emitter.encoder) {
            ESP_LOGW(kTag, "Emitter for GPIO %d is incomplete, recreating it", gpio);
            reset_emitter(g_emitter);
        } else {
            return &g_emitter;
        }
    }

    if (g_emitter.gpio >= 0 && g_emitter.gpio != gpio) {
        ESP_LOGI(kTag, "Reconfiguring IR emitter from GPIO %d to GPIO %d", g_emitter.gpio, gpio);
        reset_emitter(g_emitter);
    }

    EmitterRuntime &slot = g_emitter;
    slot.gpio = gpio;
    rmt_tx_channel_config_t channel_config = {
        .gpio_num = static_cast<gpio_num_t>(gpio),
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = kResolutionHz,
        .mem_block_symbols = 512,
        .trans_queue_depth = 4,
        .flags = {
            .invert_out = false,
            .with_dma = false,
            .io_loop_back = false,
            .io_od_mode = false,
        },
    };
    esp_err_t err = rmt_new_tx_channel(&channel_config, &slot.channel);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "rmt_new_tx_channel failed: %s", esp_err_to_name(err));
        reset_emitter(slot);
        return nullptr;
    }

    rmt_copy_encoder_config_t encoder_config = {};
    err = rmt_new_copy_encoder(&encoder_config, &slot.encoder);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "rmt_new_copy_encoder failed: %s", esp_err_to_name(err));
        reset_emitter(slot);
        return nullptr;
    }

    rmt_carrier_config_t carrier_config = {
        .frequency_hz = kCarrierHz,
        .duty_cycle = 0.33f,
        .flags = {
            .polarity_active_low = false,
        },
    };
    err = rmt_apply_carrier(slot.channel, &carrier_config);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "rmt_apply_carrier failed: %s", esp_err_to_name(err));
        reset_emitter(slot);
        return nullptr;
    }

    err = rmt_enable(slot.channel);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "rmt_enable failed: %s", esp_err_to_name(err));
        reset_emitter(slot);
        return nullptr;
    }

    ESP_LOGI(kTag, "Configured IR emitter on GPIO %d", gpio);
    return &slot;
}

esp_err_t send_daikin(const hvac_ir_command_t *command)
{
    EmitterRuntime *emitter = get_or_create_emitter(command->emitter_gpio);
    ESP_RETURN_ON_FALSE(emitter, ESP_ERR_NOT_FOUND, kTag, "emitter unavailable");

    uint8_t state[kDaikinStateLength] = {};
    rmt_symbol_word_t symbols[kDaikinSymbolCount] = {};
    fill_daikin_state(command, state);
    const size_t symbol_count = build_daikin_symbols(state, symbols, kDaikinSymbolCount);

    rmt_transmit_config_t transmit_config = {
        .loop_count = 0,
        .flags = {
            .eot_level = 0,
            .queue_nonblocking = false,
        },
    };

    ESP_RETURN_ON_ERROR(rmt_transmit(emitter->channel, emitter->encoder, symbols, symbol_count * sizeof(rmt_symbol_word_t),
                                     &transmit_config),
                        kTag, "rmt_transmit failed");
    ESP_RETURN_ON_ERROR(rmt_tx_wait_all_done(emitter->channel, -1), kTag, "rmt_tx_wait_all_done failed");
    return ESP_OK;
}

esp_err_t send_gree(const hvac_ir_command_t *command)
{
    EmitterRuntime *emitter = get_or_create_emitter(command->emitter_gpio);
    ESP_RETURN_ON_FALSE(emitter, ESP_ERR_NOT_FOUND, kTag, "emitter unavailable");

    uint8_t state[kGreeStateLength] = {};
    rmt_symbol_word_t symbols[kGreeSymbolCount] = {};
    fill_gree_state(command, state);
    const size_t symbol_count = build_gree_symbols(state, symbols, kGreeSymbolCount);

    rmt_transmit_config_t transmit_config = {
        .loop_count = 0,
        .flags = {
            .eot_level = 0,
            .queue_nonblocking = false,
        },
    };

    ESP_RETURN_ON_ERROR(rmt_transmit(emitter->channel, emitter->encoder, symbols, symbol_count * sizeof(rmt_symbol_word_t),
                                     &transmit_config),
                        kTag, "rmt_transmit failed");
    ESP_RETURN_ON_ERROR(rmt_tx_wait_all_done(emitter->channel, -1), kTag, "rmt_tx_wait_all_done failed");
    return ESP_OK;
}

esp_err_t send_midea(const hvac_ir_command_t *command)
{
    EmitterRuntime *emitter = get_or_create_emitter(command->emitter_gpio);
    ESP_RETURN_ON_FALSE(emitter, ESP_ERR_NOT_FOUND, kTag, "emitter unavailable");

    uint8_t state[6] = {};
    rmt_symbol_word_t symbols[kMideaSymbolCount] = {};
    fill_midea_state(command, state);
    const size_t symbol_count = build_midea_symbols(state, symbols, kMideaSymbolCount);

    rmt_transmit_config_t transmit_config = {
        .loop_count = 0,
        .flags = {
            .eot_level = 0,
            .queue_nonblocking = false,
        },
    };

    ESP_RETURN_ON_ERROR(rmt_transmit(emitter->channel, emitter->encoder, symbols, symbol_count * sizeof(rmt_symbol_word_t),
                                     &transmit_config),
                        kTag, "rmt_transmit failed");
    ESP_RETURN_ON_ERROR(rmt_tx_wait_all_done(emitter->channel, -1), kTag, "rmt_tx_wait_all_done failed");
    return ESP_OK;
}

}  // namespace

bool ir_sender_is_protocol_supported(const char *protocol)
{
    return protocol_equals(protocol, "daikin") || protocol_equals(protocol, "gree") || protocol_equals(protocol, "midea");
}

esp_err_t ir_sender_send(const hvac_ir_command_t *command)
{
    if (!command || !command->protocol) {
        return ESP_ERR_INVALID_ARG;
    }
    SemaphoreHandle_t mutex = get_emitters_mutex();
    if (!mutex) {
        ESP_LOGE(kTag, "Failed to create emitter mutex");
        return ESP_ERR_NO_MEM;
    }
    if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(kTag, "Failed to lock emitter mutex");
        return ESP_FAIL;
    }

    esp_err_t err = ESP_ERR_NOT_SUPPORTED;
    if (protocol_equals(command->protocol, "daikin")) {
        err = send_daikin(command);
    } else if (protocol_equals(command->protocol, "gree")) {
        err = send_gree(command);
    } else if (protocol_equals(command->protocol, "midea")) {
        err = send_midea(command);
    } else {
        ESP_LOGW(kTag, "Unsupported protocol: %s", command->protocol);
    }
    xSemaphoreGive(mutex);
    return err;
}
