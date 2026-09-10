#ifndef REMOTEBSP_STM32_HEALTH_HAL_H
#define REMOTEBSP_STM32_HEALTH_HAL_H

#include "remotebsp_embedded/core.h"

#include <stdbool.h>

/* 启动时推进掉电保持的生产者代际；失败时仅禁用健康快照。 */
bool rbsp_stm32_health_init(void);

/* 只上报公共板层能够证明的字段；CPU/ISR/栈指标保持 unavailable。 */
bool rbsp_stm32_health_sample(rbsp_mcu_health_sample_t* sample);

#endif
