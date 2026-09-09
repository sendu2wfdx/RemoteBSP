#pragma once

#include "remotebsp/device_params/store.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RBSP_BOOT_EPOCH_JOURNAL_PAGE_COUNT 2U
#define RBSP_BOOT_EPOCH_JOURNAL_RECORD_SIZE 32U

typedef enum {
    RBSP_BOOT_EPOCH_JOURNAL_OK = 0,
    RBSP_BOOT_EPOCH_JOURNAL_ERROR_INVALID_BACKEND,
    RBSP_BOOT_EPOCH_JOURNAL_ERROR_CORRUPT,
    RBSP_BOOT_EPOCH_JOURNAL_ERROR_EXHAUSTED,
    RBSP_BOOT_EPOCH_JOURNAL_ERROR_IO,
    RBSP_BOOT_EPOCH_JOURNAL_ERROR_VERIFY,
} rbsp_boot_epoch_journal_error;

typedef struct {
    rbsp_device_param_backend backend;
    uint32_t page_size;
    uint64_t last_epoch;
    rbsp_boot_epoch_journal_error last_error;
} rbsp_boot_epoch_journal;

/*
 * 后端必须指向独立的两页区域，不能与设备参数、APP 或 Bootloader 共用。
 * 本接口不会自行推断或占用板卡 Flash 布局。
 */
bool rbsp_boot_epoch_journal_init(
    rbsp_boot_epoch_journal* journal,
    const rbsp_device_param_backend* backend);

/*
 * 持久提交下一非零启动代次并回读验证。只有返回 true 时 output_epoch 才可
 * 暴露给 TimeSync；失败时调用方必须保持跨板同步禁用。
 */
bool rbsp_boot_epoch_journal_advance(
    rbsp_boot_epoch_journal* journal, uint64_t* output_epoch);

#ifdef __cplusplus
}
#endif
