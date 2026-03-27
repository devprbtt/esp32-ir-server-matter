#pragma once

#include <esp_err.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    const char *protocol;
    int16_t model;
    int emitter_gpio;
    bool power;
    uint8_t system_mode;
    uint8_t fan_mode;
    int16_t target_temperature_centi_c;
} hvac_ir_command_t;

bool ir_sender_is_protocol_supported(const char *protocol);
esp_err_t ir_sender_send(const hvac_ir_command_t *command);
