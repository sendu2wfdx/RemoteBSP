#pragma once

#include "remotebsp/device_params/store.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef bool (*rbsp_eeprom_read_fn)(void*, uint32_t, uint8_t*, size_t);
typedef bool (*rbsp_eeprom_write_fn)(void*, uint32_t, const uint8_t*, size_t);
typedef bool (*rbsp_eeprom_fill_fn)(void*, uint32_t, size_t, uint8_t);

typedef struct {
    void* io_context;
    uint32_t region_size;
    uint32_t page_size;
    uint32_t program_size;
    rbsp_eeprom_read_fn read;
    rbsp_eeprom_write_fn write;
    rbsp_eeprom_fill_fn erase_or_fill;
    uint8_t* mirror;
    size_t mirror_size;
} rbsp_device_parameter_eeprom_config;

typedef struct {
    rbsp_device_parameter_eeprom_config config;
    bool synchronized;
} rbsp_device_parameter_eeprom_adapter;

/* 纯介质适配器；不包含I2C、SPI或任何器件协议。 */
bool rbsp_device_parameter_eeprom_backend_init(
    rbsp_device_parameter_eeprom_adapter* adapter,
    rbsp_device_param_backend* backend,
    const rbsp_device_parameter_eeprom_config* config);

/* 仅在Kconfig选择外部EEPROM时由具体板级实现提供。 */
bool rbsp_board_device_parameter_eeprom_config(
    rbsp_device_parameter_eeprom_config* config);
