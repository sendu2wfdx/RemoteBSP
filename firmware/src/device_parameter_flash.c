#include "remotebsp_embedded/device_parameter_flash.h"

#include "remotebsp_config.h"

#include <stdint.h>
#include <string.h>

#if defined(CONFIG_BOARD_STM32F072RBT6)
#include "stm32f0xx_hal.h"
#elif defined(CONFIG_BOARD_STM32F103CBT6)
#include "stm32f1xx_hal.h"
#elif defined(CONFIG_BOARD_STM32G431CBU6)
#include "stm32g4xx_hal.h"
#else
#error "设备参数Flash后端尚未支持当前MCU"
#endif

extern uint8_t __rbsp_device_param_flash_start__;
extern uint8_t __rbsp_device_param_flash_end__;

static uintptr_t region_start(void) {
    return (uintptr_t)&__rbsp_device_param_flash_start__;
}

static size_t region_size(void) {
    return (size_t)((uintptr_t)&__rbsp_device_param_flash_end__ -
                    region_start());
}

static const uint8_t* flash_map(void* context, uint32_t offset,
                                size_t length) {
    const size_t capacity = region_size();
    (void)context;
    if (offset > capacity || length > capacity - offset) {
        return NULL;
    }
    return (const uint8_t*)(region_start() + offset);
}

static bool flash_erase(void* context, uint32_t offset, size_t length) {
    FLASH_EraseInitTypeDef erase;
    uint32_t page_error = 0U;
    HAL_StatusTypeDef result;
    const uintptr_t address = region_start() + offset;
    (void)context;
    if (offset > region_size() || length == 0U ||
        length > region_size() - offset ||
        (address % FLASH_PAGE_SIZE) != 0U ||
        (length % FLASH_PAGE_SIZE) != 0U) {
        return false;
    }
    memset(&erase, 0, sizeof(erase));
    erase.TypeErase = FLASH_TYPEERASE_PAGES;
#if defined(CONFIG_BOARD_STM32G431CBU6)
    erase.Banks = FLASH_BANK_1;
    erase.Page = (uint32_t)((address - FLASH_BASE) / FLASH_PAGE_SIZE);
    erase.NbPages = (uint32_t)(length / FLASH_PAGE_SIZE);
#else
    erase.PageAddress = (uint32_t)address;
    erase.NbPages = (uint32_t)(length / FLASH_PAGE_SIZE);
#endif
    if (HAL_FLASH_Unlock() != HAL_OK) {
        return false;
    }
    result = HAL_FLASHEx_Erase(&erase, &page_error);
    (void)HAL_FLASH_Lock();
    return result == HAL_OK;
}

static bool flash_program(void* context, uint32_t offset,
                          const uint8_t* data, size_t length) {
    uintptr_t address = region_start() + offset;
    size_t consumed = 0U;
    HAL_StatusTypeDef result = HAL_OK;
    (void)context;
    if (data == NULL || offset > region_size() || length == 0U ||
        length > region_size() - offset) {
        return false;
    }
#if defined(CONFIG_BOARD_STM32G431CBU6)
    if ((address % 8U) != 0U || (length % 8U) != 0U) {
        return false;
    }
#else
    if ((address % 2U) != 0U || (length % 2U) != 0U) {
        return false;
    }
#endif
    if (HAL_FLASH_Unlock() != HAL_OK) {
        return false;
    }
    while (consumed < length && result == HAL_OK) {
#if defined(CONFIG_BOARD_STM32G431CBU6)
        uint64_t value;
        memcpy(&value, data + consumed, sizeof(value));
        result = HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD,
                                   (uint32_t)address, value);
        address += sizeof(value);
        consumed += sizeof(value);
#else
        uint16_t value;
        memcpy(&value, data + consumed, sizeof(value));
        result = HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD,
                                   (uint32_t)address, value);
        address += sizeof(value);
        consumed += sizeof(value);
#endif
    }
    (void)HAL_FLASH_Lock();
    return result == HAL_OK;
}

bool rbsp_device_parameter_flash_backend_init(
    rbsp_device_param_backend* backend) {
    if (backend == NULL || region_size() != 2U * FLASH_PAGE_SIZE ||
        FLASH_PAGE_SIZE < RBSP_DEVICE_PARAM_STORE_MAX_IMAGE_SIZE) {
        return false;
    }
    memset(backend, 0, sizeof(*backend));
    backend->region_size = (uint32_t)region_size();
    backend->erase_size = FLASH_PAGE_SIZE;
#if defined(CONFIG_BOARD_STM32G431CBU6)
    backend->program_size = 8U;
#else
    backend->program_size = 2U;
#endif
    backend->map = flash_map;
    backend->erase = flash_erase;
    backend->program = flash_program;
    return true;
}
