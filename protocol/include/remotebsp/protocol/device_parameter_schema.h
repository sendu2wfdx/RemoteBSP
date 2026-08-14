#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RBSP_DEVICE_PARAM_SCHEMA_VERSION 1U
#define RBSP_DEVICE_PARAM_MAX_VALUE_SIZE 64U
#define RBSP_DEVICE_PARAM_ADC_CHANNEL_COUNT 16U

typedef enum {
    RBSP_DEVICE_PARAM_TYPE_BYTES = 1,
    RBSP_DEVICE_PARAM_TYPE_UTF8 = 2,
    RBSP_DEVICE_PARAM_TYPE_U32 = 3,
    RBSP_DEVICE_PARAM_TYPE_S32 = 4,
    RBSP_DEVICE_PARAM_TYPE_ADC_CALIBRATION = 5,
} rbsp_device_param_type;

enum {
    RBSP_DEVICE_PARAM_FLAG_FACTORY = 1U << 0,
    RBSP_DEVICE_PARAM_FLAG_MAINTENANCE_WRITE = 1U << 1,
    RBSP_DEVICE_PARAM_FLAG_WRITE_ONCE = 1U << 2,
    RBSP_DEVICE_PARAM_FLAG_APPLY_AFTER_RESTART = 1U << 3,
    RBSP_DEVICE_PARAM_FLAG_USER = 1U << 4,
};

typedef enum {
    RBSP_DEVICE_PARAM_SERIAL_NUMBER = 0x0001,
    RBSP_DEVICE_PARAM_DEVICE_UUID = 0x0002,
    RBSP_DEVICE_PARAM_HARDWARE_REVISION = 0x0003,
    RBSP_DEVICE_PARAM_MANUFACTURING_BATCH = 0x0004,
    RBSP_DEVICE_PARAM_MANUFACTURING_DATE = 0x0005,
    RBSP_DEVICE_PARAM_DEVICE_NAME = 0x0100,
    RBSP_DEVICE_PARAM_ADC_CALIBRATION_BASE = 0x1000,
} rbsp_device_param_id;

/* ADC校准值全部使用定点整数，避免不同工具链的浮点二进制差异。 */
typedef struct {
    int32_t gain_q16_16;
    int32_t offset_uv;
    uint32_t reference_uv;
} rbsp_device_param_adc_calibration;

typedef struct {
    uint16_t id;
    uint8_t type;
    uint8_t flags;
    uint16_t minimum_length;
    uint16_t maximum_length;
} rbsp_device_param_definition;

size_t rbsp_device_param_definition_count(void);
bool rbsp_device_param_definition_at(
    size_t index, rbsp_device_param_definition* definition);
bool rbsp_device_param_find_definition(
    uint16_t id, rbsp_device_param_definition* definition);
bool rbsp_device_param_validate_value(
    uint16_t id, const uint8_t* value, size_t length);

#ifdef __cplusplus
}
#endif
