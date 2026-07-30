#pragma once

#if defined(STM32F103xB)
#include "stm32f1xx_hal.h"
#elif defined(STM32G431xx)
#include "stm32g4xx_hal.h"
#else
#error "USB Device 配置仅支持当前 RemoteBSP STM32 目标"
#endif

#include <stdint.h>
#include <string.h>

#define USBD_MAX_NUM_INTERFACES 2U
#define USBD_MAX_NUM_CONFIGURATION 1U
#define USBD_MAX_STR_DESC_SIZ 64U
#define USBD_SELF_POWERED 0U
#define USBD_MAX_POWER 50U
#define USBD_DEBUG_LEVEL 0U
#define USBD_LPM_ENABLED 0U
#define USBD_SUPPORT_USER_STRING_DESC 0U
#define USBD_CLASS_USER_STRING_DESC 0U
#define USBD_CLASS_BOS_ENABLED 0U

#define USBD_malloc (void*)USBD_static_malloc
#define USBD_free USBD_static_free
#define USBD_memset memset
#define USBD_memcpy memcpy
#define USBD_Delay HAL_Delay

#define USBD_UsrLog(...) ((void)0U)
#define USBD_ErrLog(...) ((void)0U)
#define USBD_DbgLog(...) ((void)0U)

void* USBD_static_malloc(uint32_t size);
void USBD_static_free(void* memory);
