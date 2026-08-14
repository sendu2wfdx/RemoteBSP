#pragma once

#include "remotebsp/device_params/store.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 把链接脚本保留的芯片末端区域包装为介质无关设备参数后端。
 * 上层只依赖 rbsp_device_param_backend，后续替换外部 EEPROM 时无需改协议。
 */
bool rbsp_device_parameter_flash_backend_init(
    rbsp_device_param_backend* backend);

#ifdef __cplusplus
}
#endif
