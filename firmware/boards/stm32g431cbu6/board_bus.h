#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "remotebsp_embedded/core.h"

#ifdef CONFIG_REMOTEBSP_BUS
bool rbsp_g431_bus_init(void);
bool rbsp_g431_bus_pin_reserved(uint16_t encoded_pin);
const rbsp_bus_resource_config_t* rbsp_g431_bus_resources(void);
uint8_t rbsp_g431_bus_resource_count(void);
bool rbsp_g431_bus_resource_status(
    uint8_t resource_type, uint16_t instance,
    rbsp_resource_runtime_status_t* status);
rbsp_bus_transaction_status_t rbsp_g431_i2c_transfer(
    const rbsp_bus_resource_config_t* device, uint32_t timeout_us,
    uint16_t flags, const uint8_t* write_data, uint16_t write_length,
    uint8_t* read_data, uint16_t read_length, uint16_t* transmitted,
    uint16_t* received);
rbsp_bus_transaction_status_t rbsp_g431_spi_transfer(
    const rbsp_bus_resource_config_t* device, uint32_t timeout_us,
    uint16_t flags, uint8_t dummy_byte, const uint8_t* transmit_data,
    uint16_t transmit_length, uint8_t* receive_data, uint16_t receive_length,
    uint16_t* transmitted, uint16_t* received);
#endif
