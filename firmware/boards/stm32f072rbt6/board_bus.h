#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "remotebsp_embedded/core.h"
#ifdef CONFIG_REMOTEBSP_BUS
bool rbsp_f072_bus_init(void);
bool rbsp_f072_bus_pin_reserved(uint16_t pin);
const rbsp_bus_resource_config_t* rbsp_f072_bus_resources(void);
uint8_t rbsp_f072_bus_resource_count(void);
bool rbsp_f072_bus_resource_status(uint8_t type, uint16_t instance, rbsp_resource_runtime_status_t* status);
rbsp_bus_transaction_status_t rbsp_f072_i2c_transfer(const rbsp_bus_resource_config_t*, uint32_t, uint16_t, const uint8_t*, uint16_t, uint8_t*, uint16_t, uint16_t*, uint16_t*);
rbsp_bus_transaction_status_t rbsp_f072_spi_transfer(const rbsp_bus_resource_config_t*, uint32_t, uint16_t, uint8_t, const uint8_t*, uint16_t, uint8_t*, uint16_t, uint16_t*, uint16_t*);
#endif
