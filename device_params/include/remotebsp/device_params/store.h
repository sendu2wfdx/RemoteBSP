#pragma once

#include "remotebsp/protocol/device_parameter_schema.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RBSP_DEVICE_PARAM_STORE_PAGE_COUNT 2U
#define RBSP_DEVICE_PARAM_STORE_HEADER_SIZE 32U
#define RBSP_DEVICE_PARAM_STORE_MAX_IMAGE_SIZE 512U
#define RBSP_DEVICE_PARAM_STORE_MAX_RECORDS 32U
#define RBSP_DEVICE_PARAM_STORE_NO_PAGE 0xFFU

typedef enum {
    RBSP_DEVICE_PARAM_STORE_OK = 0,
    RBSP_DEVICE_PARAM_STORE_ERROR_INVALID_BACKEND,
    RBSP_DEVICE_PARAM_STORE_ERROR_CORRUPT,
    RBSP_DEVICE_PARAM_STORE_ERROR_NOT_FOUND,
    RBSP_DEVICE_PARAM_STORE_ERROR_INVALID_VALUE,
    RBSP_DEVICE_PARAM_STORE_ERROR_NO_SPACE,
    RBSP_DEVICE_PARAM_STORE_ERROR_IO,
    RBSP_DEVICE_PARAM_STORE_ERROR_STALE_GENERATION,
    RBSP_DEVICE_PARAM_STORE_ERROR_WRITE_ONCE,
} rbsp_device_param_store_error;

typedef const uint8_t* (*rbsp_device_param_map_fn)(
    void* context, uint32_t offset, size_t length);
typedef bool (*rbsp_device_param_erase_fn)(
    void* context, uint32_t offset, size_t length);
typedef bool (*rbsp_device_param_program_fn)(
    void* context, uint32_t offset, const uint8_t* data, size_t length);

typedef struct {
    void* context;
    uint32_t region_size;
    uint32_t erase_size;
    uint32_t program_size;
    rbsp_device_param_map_fn map;
    rbsp_device_param_erase_fn erase;
    rbsp_device_param_program_fn program;
} rbsp_device_param_backend;

typedef struct {
    uint16_t id;
    uint8_t type;
    uint8_t flags;
    uint16_t length;
    const uint8_t* value;
} rbsp_device_param_record;

typedef struct {
    rbsp_device_param_backend backend;
    uint32_t page_size;
    uint32_t generation;
    uint16_t image_length;
    uint16_t record_count;
    uint8_t active_page;
    rbsp_device_param_store_error last_error;
} rbsp_device_param_store;

typedef enum {
    RBSP_DEVICE_PARAM_VALUE_ABSENT = 0,
    RBSP_DEVICE_PARAM_VALUE_DEFAULTED,
    RBSP_DEVICE_PARAM_VALUE_PERSISTED,
} rbsp_device_param_value_source;

bool rbsp_device_param_store_init(
    rbsp_device_param_store* store,
    const rbsp_device_param_backend* backend);
bool rbsp_device_param_store_boot(rbsp_device_param_store* store);

bool rbsp_device_param_store_get(
    const rbsp_device_param_store* store, uint16_t id,
    rbsp_device_param_record* record);
/*
 * 读取持久值或schema默认值。返回true表示id属于当前schema；未知id返回false。
 * DEFAULTED只存在于读取结果，不写Flash，也不推进generation。
 */
bool rbsp_device_param_store_resolve(
    const rbsp_device_param_store* store, uint16_t id,
    rbsp_device_param_record* record,
    rbsp_device_param_value_source* source);
bool rbsp_device_param_store_record_at(
    const rbsp_device_param_store* store, size_t index,
    rbsp_device_param_record* record);

bool rbsp_device_param_store_set(
    rbsp_device_param_store* store, uint32_t expected_generation,
    uint16_t id, const uint8_t* value, size_t length,
    uint8_t* workspace, size_t workspace_size);

#ifdef __cplusplus
}
#endif
