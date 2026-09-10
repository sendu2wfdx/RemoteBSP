#include "remotebsp_embedded/core.h"

#ifdef CONFIG_REMOTEBSP_BUS
#if defined(RBSP_F072_BUS_ENABLED)
#include "stm32f0xx_hal.h"
#define rbsp_g431_bus_init rbsp_f072_bus_init
#define rbsp_g431_bus_pin_reserved rbsp_f072_bus_pin_reserved
#define rbsp_g431_bus_resources rbsp_f072_bus_resources
#define rbsp_g431_bus_resource_count rbsp_f072_bus_resource_count
#define rbsp_g431_bus_resource_status rbsp_f072_bus_resource_status
#define rbsp_g431_i2c_transfer rbsp_f072_i2c_transfer
#define rbsp_g431_spi_transfer rbsp_f072_spi_transfer
#define CONFIG_G431_BUS_I2C1_PB6_PB7 CONFIG_F072_BUS_I2C1_PB6_PB7
#define CONFIG_G431_BUS_I2C1_DEVICE_ADDRESS CONFIG_F072_BUS_I2C1_DEVICE_ADDRESS
#define CONFIG_G431_BUS_SPI1_PA5_PA6_PA7_CS_PA15 CONFIG_F072_BUS_SPI1_PA5_PA6_PA7_CS_PA4
#elif defined(RBSP_F103_BUS_ENABLED)
#include "stm32f1xx_hal.h"
#define rbsp_g431_bus_init rbsp_f103_bus_init
#define rbsp_g431_bus_pin_reserved rbsp_f103_bus_pin_reserved
#define rbsp_g431_bus_resources rbsp_f103_bus_resources
#define rbsp_g431_bus_resource_count rbsp_f103_bus_resource_count
#define rbsp_g431_bus_resource_status rbsp_f103_bus_resource_status
#define rbsp_g431_i2c_transfer rbsp_f103_i2c_transfer
#define rbsp_g431_spi_transfer rbsp_f103_spi_transfer
#define CONFIG_G431_BUS_I2C1_PB6_PB7 CONFIG_F103_BUS_I2C1_PB6_PB7
#define CONFIG_G431_BUS_I2C1_DEVICE_ADDRESS CONFIG_F103_BUS_I2C1_DEVICE_ADDRESS
#define CONFIG_G431_BUS_SPI1_PA5_PA6_PA7_CS_PA15 CONFIG_F103_BUS_SPI1_PA5_PA6_PA7_CS_PA4
#else
#include "stm32g4xx_hal.h"
#endif

enum {
    I2C1_BUS_ID = 0x0B000001U,
    I2C1_DEVICE_ID = 0x0C000001U,
    SPI1_BUS_ID = 0x0D000001U,
    SPI1_DEVICE_ID = 0x0E000001U,
    SPI2_BUS_ID = 0x0D000002U,
    SPI2_DEVICE_ID = 0x0E000002U,
};

static I2C_HandleTypeDef i2c1;
static SPI_HandleTypeDef spi1;
#if !defined(RBSP_F072_BUS_ENABLED) && !defined(RBSP_F103_BUS_ENABLED)
static SPI_HandleTypeDef spi2;
#endif
static bool i2c1_backend_failed;
static bool spi1_backend_failed;
#if !defined(RBSP_F072_BUS_ENABLED) && !defined(RBSP_F103_BUS_ENABLED)
static bool spi2_backend_failed;
#endif

/*
 * 依据 RM0440 I2C_TIMINGR 与 AN4235 的 Fast-mode 约束计算：
 * PCLK1=170MHz，PRESC=4 得 tPRESC=29.412ns；SCLL=55、SCLH=25，
 * 原始低/高电平分别为 1.647us/0.765us；SCLDEL=15 得 470.6ns，覆盖
 * 300ns 上升时间与 100ns 数据建立时间。计入模拟滤波器最小 50ns 和
 * 两级同步器后，最快周期仍不短于 2.5us。这里有意留出低电平裕量，
 * 最终频率会随器件滤波延迟和板级上升时间略低于 400kHz。
 */
#if !defined(RBSP_F072_BUS_ENABLED) && !defined(RBSP_F103_BUS_ENABLED)
enum {
    I2C1_TIMING_PRESC = 4U,
    I2C1_TIMING_SCLDEL = 15U,
    I2C1_TIMING_SDADEL = 2U,
    I2C1_TIMING_SCLH = 25U,
    I2C1_TIMING_SCLL = 55U,
    I2C1_TIMING = (I2C1_TIMING_PRESC << 28U) |
                  (I2C1_TIMING_SCLDEL << 20U) |
                  (I2C1_TIMING_SDADEL << 16U) |
                  (I2C1_TIMING_SCLH << 8U) | I2C1_TIMING_SCLL,
};
_Static_assert((I2C1_TIMING_SCLL + 1U) * (I2C1_TIMING_PRESC + 1U) *
                   1000000000ULL >= 1300ULL * 170000000ULL,
               "I2C Fast-mode低电平时间不足");
_Static_assert((I2C1_TIMING_SCLDEL + 1U) * (I2C1_TIMING_PRESC + 1U) *
                   1000000000ULL >= 400ULL * 170000000ULL,
               "I2C Fast-mode数据建立裕量不足");
_Static_assert((((I2C1_TIMING_SCLL + 1U) +
                 (I2C1_TIMING_SCLH + 1U)) *
                    (I2C1_TIMING_PRESC + 1U) + 22U) * 400000ULL >=
                   170000000ULL,
               "I2C Fast-mode最快周期超过400kHz");
#endif

static const rbsp_bus_resource_config_t resources[] = {
#ifdef CONFIG_G431_BUS_I2C1_PB6_PB7
    {I2C1_BUS_ID, 0U,
#if defined(RBSP_F072_BUS_ENABLED) || defined(RBSP_F103_BUS_ENABLED)
     100000U,
#else
     400000U,
#endif
     CONFIG_REMOTEBSP_BUS_MIN_TIMEOUT_US,
     CONFIG_REMOTEBSP_BUS_MAX_TIMEOUT_US, 1000U,
     CONFIG_REMOTEBSP_BUS_MAX_TRANSFER_BYTES, 1U, 0U, 1U,
     RBSP_BUS_I2C_BUS,
     RBSP_BUS_CONTRACT_I2C_REPEATED_START | RBSP_BUS_CONTRACT_I2C_RECOVERY,
     0U, 0U},
    {I2C1_DEVICE_ID, I2C1_BUS_ID,
#if defined(RBSP_F072_BUS_ENABLED) || defined(RBSP_F103_BUS_ENABLED)
     100000U,
#else
     400000U,
#endif
     CONFIG_REMOTEBSP_BUS_MIN_TIMEOUT_US, CONFIG_REMOTEBSP_BUS_MAX_TIMEOUT_US,
     1000U, CONFIG_REMOTEBSP_BUS_MAX_TRANSFER_BYTES, 1U,
     CONFIG_G431_BUS_I2C1_DEVICE_ADDRESS, 1U, RBSP_BUS_I2C_DEVICE,
     RBSP_BUS_CONTRACT_I2C_REPEATED_START | RBSP_BUS_CONTRACT_I2C_RECOVERY,
     0U, 0U},
#endif
#ifdef CONFIG_G431_BUS_SPI1_PA5_PA6_PA7_CS_PA15
    {SPI1_BUS_ID, 0U,
#if defined(RBSP_F072_BUS_ENABLED)
     6000000U,
#elif defined(RBSP_F103_BUS_ENABLED)
     9000000U,
#else
     21250000U,
#endif
     CONFIG_REMOTEBSP_BUS_MIN_TIMEOUT_US,
     CONFIG_REMOTEBSP_BUS_MAX_TIMEOUT_US, 1000U,
     CONFIG_REMOTEBSP_BUS_MAX_TRANSFER_BYTES, 1U, 0U, 1U,
     RBSP_BUS_SPI_BUS,
     RBSP_BUS_CONTRACT_SPI_FULL_DUPLEX |
         RBSP_BUS_CONTRACT_SPI_KEEP_CHIP_SELECT,
     0U, 8U},
    {SPI1_DEVICE_ID, SPI1_BUS_ID,
#if defined(RBSP_F072_BUS_ENABLED)
     6000000U,
#elif defined(RBSP_F103_BUS_ENABLED)
     9000000U,
#else
     21250000U,
#endif
     CONFIG_REMOTEBSP_BUS_MIN_TIMEOUT_US, CONFIG_REMOTEBSP_BUS_MAX_TIMEOUT_US,
     1000U, CONFIG_REMOTEBSP_BUS_MAX_TRANSFER_BYTES, 1U,
#if defined(RBSP_F072_BUS_ENABLED) || defined(RBSP_F103_BUS_ENABLED)
     4U,
#else
     15U,
#endif
     1U,
     RBSP_BUS_SPI_DEVICE,
     RBSP_BUS_CONTRACT_SPI_FULL_DUPLEX |
         RBSP_BUS_CONTRACT_SPI_KEEP_CHIP_SELECT,
     0U, 8U},
#endif
#ifdef CONFIG_G431_BUS_SPI2_PB13_PB14_PB15_CS_PB12
    {SPI2_BUS_ID, 0U, 21250000U, CONFIG_REMOTEBSP_BUS_MIN_TIMEOUT_US,
     CONFIG_REMOTEBSP_BUS_MAX_TIMEOUT_US, 1000U,
     CONFIG_REMOTEBSP_BUS_MAX_TRANSFER_BYTES, 1U, 0U, 2U,
     RBSP_BUS_SPI_BUS,
     RBSP_BUS_CONTRACT_SPI_FULL_DUPLEX |
         RBSP_BUS_CONTRACT_SPI_KEEP_CHIP_SELECT,
     0U, 8U},
    {SPI2_DEVICE_ID, SPI2_BUS_ID, 21250000U,
     CONFIG_REMOTEBSP_BUS_MIN_TIMEOUT_US, CONFIG_REMOTEBSP_BUS_MAX_TIMEOUT_US,
     1000U, CONFIG_REMOTEBSP_BUS_MAX_TRANSFER_BYTES, 1U, 28U, 2U,
     RBSP_BUS_SPI_DEVICE,
     RBSP_BUS_CONTRACT_SPI_FULL_DUPLEX |
         RBSP_BUS_CONTRACT_SPI_KEEP_CHIP_SELECT,
     0U, 8U},
#endif
};

static uint32_t timeout_ms(uint32_t timeout_us) {
    const uint32_t value = (timeout_us + 999U) / 1000U;
    return value == 0U ? 1U : value;
}

static uint32_t remaining_ms(uint32_t started, uint32_t budget_ms) {
    const uint32_t elapsed = (uint32_t)(HAL_GetTick() - started);
    return elapsed >= budget_ms ? 0U : budget_ms - elapsed;
}

static rbsp_bus_transaction_status_t hal_status(HAL_StatusTypeDef status,
                                                 uint32_t error) {
    if (status == HAL_OK) return RBSP_BUS_TRANSACTION_OK;
    if (status == HAL_TIMEOUT) return RBSP_BUS_TRANSACTION_TIMEOUT;
    if (status == HAL_BUSY) return RBSP_BUS_TRANSACTION_BUSY;
    if ((error & HAL_I2C_ERROR_AF) != 0U) return RBSP_BUS_TRANSACTION_NACK;
    return RBSP_BUS_TRANSACTION_FAULT;
}

#ifdef CONFIG_G431_BUS_I2C1_PB6_PB7
static bool i2c1_configure(void) {
    i2c1.Instance = I2C1;
#if defined(RBSP_F103_BUS_ENABLED)
    i2c1.Init.ClockSpeed = 100000U;
    i2c1.Init.DutyCycle = I2C_DUTYCYCLE_2;
#elif defined(RBSP_F072_BUS_ENABLED)
    i2c1.Init.Timing = 0x2000090EU;
#else
    i2c1.Init.Timing = I2C1_TIMING;
#endif
    i2c1.Init.OwnAddress1 = 0U;
    i2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
    i2c1.Init.DualAddressMode =
#if defined(RBSP_F072_BUS_ENABLED)
        I2C_DUALADDRESS_DISABLED;
#else
        I2C_DUALADDRESS_DISABLE;
#endif
    i2c1.Init.OwnAddress2 = 0U;
#if !defined(RBSP_F103_BUS_ENABLED)
    i2c1.Init.OwnAddress2Masks = I2C_OA2_NOMASK;
#endif
    i2c1.Init.GeneralCallMode =
#if defined(RBSP_F072_BUS_ENABLED)
        I2C_GENERALCALL_DISABLED;
#else
        I2C_GENERALCALL_DISABLE;
#endif
    i2c1.Init.NoStretchMode =
#if defined(RBSP_F072_BUS_ENABLED)
        I2C_NOSTRETCH_DISABLED;
#else
        I2C_NOSTRETCH_DISABLE;
#endif
    return HAL_I2C_Init(&i2c1) == HAL_OK
#if !defined(RBSP_F103_BUS_ENABLED)
           &&
           HAL_I2CEx_ConfigAnalogFilter(&i2c1,
#if defined(RBSP_F072_BUS_ENABLED)
                                        I2C_ANALOGFILTER_ENABLED
#else
                                        I2C_ANALOGFILTER_ENABLE
#endif
                                        ) == HAL_OK &&
           HAL_I2CEx_ConfigDigitalFilter(&i2c1, 0U) == HAL_OK
#endif
           ;
}

static bool i2c1_recover(void) {
    (void)HAL_I2C_DeInit(&i2c1);
    GPIO_InitTypeDef pin = {0};
    pin.Pin = GPIO_PIN_6 | GPIO_PIN_7;
    pin.Mode = GPIO_MODE_OUTPUT_OD;
    pin.Pull = GPIO_PULLUP;
    pin.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOB, &pin);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_6 | GPIO_PIN_7, GPIO_PIN_SET);
    for (uint8_t pulse = 0U; pulse < 9U; ++pulse) {
        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_6, GPIO_PIN_RESET);
        for (volatile uint32_t delay = 0U; delay < 64U; ++delay) {}
        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_6, GPIO_PIN_SET);
        for (volatile uint32_t delay = 0U; delay < 64U; ++delay) {}
    }
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_7, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_6, GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_7, GPIO_PIN_SET);
    pin.Pin = GPIO_PIN_6 | GPIO_PIN_7;
    pin.Mode = GPIO_MODE_AF_OD;
#if defined(RBSP_F072_BUS_ENABLED)
    pin.Alternate = GPIO_AF1_I2C1;
#elif !defined(RBSP_F103_BUS_ENABLED)
    pin.Alternate = GPIO_AF4_I2C1;
#endif
    HAL_GPIO_Init(GPIOB, &pin);
    return i2c1_configure();
}

static HAL_StatusTypeDef i2c1_wait_ready(uint32_t wait_ms) {
    const uint32_t started = HAL_GetTick();
    while (HAL_I2C_GetState(&i2c1) != HAL_I2C_STATE_READY) {
        if ((uint32_t)(HAL_GetTick() - started) >= wait_ms) {
            return HAL_TIMEOUT;
        }
    }
    return HAL_I2C_GetError(&i2c1) == HAL_I2C_ERROR_NONE ? HAL_OK : HAL_ERROR;
}
#endif

static bool spi_configure(SPI_HandleTypeDef* handle, SPI_TypeDef* instance) {
    handle->Instance = instance;
    handle->Init.Mode = SPI_MODE_MASTER;
    handle->Init.Direction = SPI_DIRECTION_2LINES;
    handle->Init.DataSize = SPI_DATASIZE_8BIT;
    handle->Init.CLKPolarity = SPI_POLARITY_LOW;
    handle->Init.CLKPhase = SPI_PHASE_1EDGE;
    handle->Init.NSS = SPI_NSS_SOFT;
    handle->Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_8;
    handle->Init.FirstBit = SPI_FIRSTBIT_MSB;
    handle->Init.TIMode = SPI_TIMODE_DISABLE;
    handle->Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
    handle->Init.CRCPolynomial = 7U;
#if !defined(RBSP_F072_BUS_ENABLED) && !defined(RBSP_F103_BUS_ENABLED)
    handle->Init.CRCLength = SPI_CRC_LENGTH_DATASIZE;
    handle->Init.NSSPMode = SPI_NSS_PULSE_DISABLE;
#endif
    return HAL_SPI_Init(handle) == HAL_OK;
}

bool rbsp_g431_bus_init(void) {
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    GPIO_InitTypeDef pin = {0};
#ifdef CONFIG_G431_BUS_I2C1_PB6_PB7
    __HAL_RCC_I2C1_CLK_ENABLE();
    pin.Pin = GPIO_PIN_6 | GPIO_PIN_7;
    pin.Mode = GPIO_MODE_AF_OD;
    pin.Pull = GPIO_PULLUP;
    pin.Speed = GPIO_SPEED_FREQ_HIGH;
#if defined(RBSP_F072_BUS_ENABLED)
    pin.Alternate = GPIO_AF1_I2C1;
#elif !defined(RBSP_F103_BUS_ENABLED)
    pin.Alternate = GPIO_AF4_I2C1;
#endif
    HAL_GPIO_Init(GPIOB, &pin);
    if (!i2c1_configure()) {
        i2c1_backend_failed = true;
        return false;
    }
    i2c1_backend_failed = false;
#if defined(RBSP_F072_BUS_ENABLED)
    HAL_NVIC_SetPriority(I2C1_IRQn, 2U, 0U);
    HAL_NVIC_EnableIRQ(I2C1_IRQn);
#else
    HAL_NVIC_SetPriority(I2C1_EV_IRQn, 6U, 0U);
    HAL_NVIC_SetPriority(I2C1_ER_IRQn, 6U, 0U);
    HAL_NVIC_EnableIRQ(I2C1_EV_IRQn);
    HAL_NVIC_EnableIRQ(I2C1_ER_IRQn);
#endif
#endif
#ifdef CONFIG_G431_BUS_SPI1_PA5_PA6_PA7_CS_PA15
    __HAL_RCC_SPI1_CLK_ENABLE();
#if defined(RBSP_F103_BUS_ENABLED)
    pin.Pin = GPIO_PIN_5 | GPIO_PIN_7;
    pin.Mode = GPIO_MODE_AF_PP;
    pin.Pull = GPIO_NOPULL;
    pin.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOA, &pin);
    pin.Pin = GPIO_PIN_6;
    pin.Mode = GPIO_MODE_INPUT;
    HAL_GPIO_Init(GPIOA, &pin);
#else
    pin.Pin = GPIO_PIN_5 | GPIO_PIN_6 | GPIO_PIN_7;
    pin.Mode = GPIO_MODE_AF_PP;
    pin.Pull = GPIO_NOPULL;
    pin.Speed = GPIO_SPEED_FREQ_HIGH;
#if defined(RBSP_F072_BUS_ENABLED)
    pin.Alternate = GPIO_AF0_SPI1;
#elif !defined(RBSP_F103_BUS_ENABLED)
    pin.Alternate = GPIO_AF5_SPI1;
#endif
    HAL_GPIO_Init(GPIOA, &pin);
#endif
    pin.Pin =
#if defined(RBSP_F072_BUS_ENABLED) || defined(RBSP_F103_BUS_ENABLED)
        GPIO_PIN_4;
#else
        GPIO_PIN_15;
#endif
    pin.Mode = GPIO_MODE_OUTPUT_PP;
    HAL_GPIO_WritePin(GPIOA,
#if defined(RBSP_F072_BUS_ENABLED) || defined(RBSP_F103_BUS_ENABLED)
                      GPIO_PIN_4,
#else
                      GPIO_PIN_15,
#endif
                      GPIO_PIN_SET);
    HAL_GPIO_Init(GPIOA, &pin);
    if (!spi_configure(&spi1, SPI1)) {
        spi1_backend_failed = true;
        return false;
    }
    spi1_backend_failed = false;
#endif
#ifdef CONFIG_G431_BUS_SPI2_PB13_PB14_PB15_CS_PB12
    __HAL_RCC_SPI2_CLK_ENABLE();
    pin.Pin = GPIO_PIN_13 | GPIO_PIN_14 | GPIO_PIN_15;
    pin.Mode = GPIO_MODE_AF_PP;
    pin.Pull = GPIO_NOPULL;
    pin.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    pin.Alternate = GPIO_AF5_SPI2;
    HAL_GPIO_Init(GPIOB, &pin);
    pin.Pin = GPIO_PIN_12;
    pin.Mode = GPIO_MODE_OUTPUT_PP;
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_12, GPIO_PIN_SET);
    HAL_GPIO_Init(GPIOB, &pin);
    if (!spi_configure(&spi2, SPI2)) {
        spi2_backend_failed = true;
        return false;
    }
    spi2_backend_failed = false;
#endif
    return true;
}

bool rbsp_g431_bus_pin_reserved(uint16_t pin) {
#ifdef CONFIG_G431_BUS_I2C1_PB6_PB7
    if (pin == 22U || pin == 23U) return true;
#endif
#ifdef CONFIG_G431_BUS_SPI1_PA5_PA6_PA7_CS_PA15
    if (pin == 5U || pin == 6U || pin == 7U || pin ==
#if defined(RBSP_F072_BUS_ENABLED) || defined(RBSP_F103_BUS_ENABLED)
        4U
#else
        15U
#endif
    ) return true;
#endif
#ifdef CONFIG_G431_BUS_SPI2_PB13_PB14_PB15_CS_PB12
    if (pin >= 28U && pin <= 31U) return true;
#endif
    return false;
}

const rbsp_bus_resource_config_t* rbsp_g431_bus_resources(void) {
    return resources;
}

uint8_t rbsp_g431_bus_resource_count(void) {
    return (uint8_t)(sizeof(resources) / sizeof(resources[0]));
}

bool rbsp_g431_bus_resource_status(
    uint8_t resource_type, uint16_t instance,
    rbsp_resource_runtime_status_t* status) {
    if (status == NULL) return false;
#ifdef CONFIG_G431_BUS_I2C1_PB6_PB7
    if (instance == 1U && (resource_type == 11U || resource_type == 12U)) {
        status->backend_failed = i2c1_backend_failed;
        return true;
    }
#endif
#ifdef CONFIG_G431_BUS_SPI1_PA5_PA6_PA7_CS_PA15
    if (instance == 1U && (resource_type == 13U || resource_type == 14U)) {
        status->backend_failed = spi1_backend_failed;
        return true;
    }
#endif
#ifdef CONFIG_G431_BUS_SPI2_PB13_PB14_PB15_CS_PB12
    if (instance == 2U && (resource_type == 13U || resource_type == 14U)) {
        status->backend_failed = spi2_backend_failed;
        return true;
    }
#endif
    return false;
}

rbsp_bus_transaction_status_t rbsp_g431_i2c_transfer(
    const rbsp_bus_resource_config_t* device, uint32_t timeout_us,
    uint16_t flags, const uint8_t* write_data, uint16_t write_length,
    uint8_t* read_data, uint16_t read_length, uint16_t* transmitted,
    uint16_t* received) {
    *transmitted = 0U;
    *received = 0U;
#ifdef CONFIG_G431_BUS_I2C1_PB6_PB7
    if (device->controller != 1U) return RBSP_BUS_TRANSACTION_FAULT;
    if (i2c1_backend_failed) return RBSP_BUS_TRANSACTION_FAULT;
    HAL_StatusTypeDef result = HAL_OK;
    const uint32_t started = HAL_GetTick();
    const uint32_t budget_ms = timeout_ms(timeout_us);
    const uint16_t address = (uint16_t)(device->device_value << 1U);
    if (write_length != 0U && read_length != 0U) {
        if ((flags & RBSP_BUS_CONTRACT_I2C_REPEATED_START) != 0U) {
            result = HAL_I2C_Master_Seq_Transmit_IT(&i2c1, address,
                (uint8_t*)write_data, write_length, I2C_FIRST_FRAME);
            if (result == HAL_OK) result = i2c1_wait_ready(budget_ms);
            if (result == HAL_OK) {
                *transmitted = write_length;
                const uint32_t remaining = remaining_ms(started, budget_ms);
                if (remaining == 0U) {
                    result = HAL_TIMEOUT;
                } else {
                    result = HAL_I2C_Master_Seq_Receive_IT(
                        &i2c1, address, read_data, read_length,
                        I2C_LAST_FRAME);
                    if (result == HAL_OK) result = i2c1_wait_ready(remaining);
                }
            }
        } else {
            result = HAL_I2C_Master_Transmit(&i2c1, address,
                (uint8_t*)write_data, write_length, budget_ms);
            if (result == HAL_OK) {
                *transmitted = write_length;
                const uint32_t remaining = remaining_ms(started, budget_ms);
                if (remaining == 0U) {
                    result = HAL_TIMEOUT;
                } else {
                    result = HAL_I2C_Master_Receive(
                        &i2c1, address, read_data, read_length, remaining);
                }
            }
        }
    } else if (write_length != 0U) {
        result = HAL_I2C_Master_Transmit(&i2c1, address,
            (uint8_t*)write_data, write_length, budget_ms);
    } else if (read_length != 0U) {
        result = HAL_I2C_Master_Receive(&i2c1, address, read_data,
            read_length, budget_ms);
    }
    if (result == HAL_OK) {
        *transmitted = write_length;
        *received = read_length;
        return RBSP_BUS_TRANSACTION_OK;
    }
    const rbsp_bus_transaction_status_t status =
        hal_status(result, HAL_I2C_GetError(&i2c1));
    *transmitted = 0U;
    *received = 0U;
    if ((flags & RBSP_BUS_CONTRACT_I2C_RECOVERY) != 0U) {
        if (!i2c1_recover()) {
            i2c1_backend_failed = true;
            return RBSP_BUS_TRANSACTION_FAULT;
        }
    }
    return status;
#else
    (void)device; (void)timeout_us; (void)write_data; (void)write_length;
    (void)read_data; (void)read_length;
    return RBSP_BUS_TRANSACTION_FAULT;
#endif
}

rbsp_bus_transaction_status_t rbsp_g431_spi_transfer(
    const rbsp_bus_resource_config_t* device, uint32_t timeout_us,
    uint16_t flags, uint8_t dummy_byte, const uint8_t* transmit_data,
    uint16_t transmit_length, uint8_t* receive_data, uint16_t receive_length,
    uint16_t* transmitted, uint16_t* received) {
    (void)flags;
    SPI_HandleTypeDef* handle = NULL;
    GPIO_TypeDef* cs_port = NULL;
    uint16_t cs_pin = 0U;
#ifdef CONFIG_G431_BUS_SPI1_PA5_PA6_PA7_CS_PA15
    if (device->controller == 1U && !spi1_backend_failed) {
        handle = &spi1; cs_port = GPIOA;
#if defined(RBSP_F072_BUS_ENABLED) || defined(RBSP_F103_BUS_ENABLED)
        cs_pin = GPIO_PIN_4;
#else
        cs_pin = GPIO_PIN_15;
#endif
    }
#endif
#ifdef CONFIG_G431_BUS_SPI2_PB13_PB14_PB15_CS_PB12
    if (device->controller == 2U && !spi2_backend_failed) {
        handle = &spi2; cs_port = GPIOB; cs_pin = GPIO_PIN_12;
    }
#endif
    if (handle == NULL) return RBSP_BUS_TRANSACTION_FAULT;
    const uint16_t length = transmit_length > receive_length ? transmit_length : receive_length;
    *transmitted = 0U;
    *received = 0U;
    HAL_GPIO_WritePin(cs_port, cs_pin, GPIO_PIN_RESET);
    HAL_StatusTypeDef result = HAL_OK;
    const uint32_t started = HAL_GetTick();
    const uint32_t budget_ms = timeout_ms(timeout_us);
    for (uint16_t index = 0U; index < length && result == HAL_OK; ++index) {
        const uint32_t remaining = remaining_ms(started, budget_ms);
        if (remaining == 0U) {
            result = HAL_TIMEOUT;
            break;
        }
        uint8_t tx = index < transmit_length ? transmit_data[index] : dummy_byte;
        uint8_t rx = 0U;
        result = HAL_SPI_TransmitReceive(handle, &tx, &rx, 1U,
                                         remaining);
        if (result == HAL_OK) {
            if (index < transmit_length) ++*transmitted;
            if (index < receive_length) receive_data[(*received)++] = rx;
        }
    }
    HAL_GPIO_WritePin(cs_port, cs_pin, GPIO_PIN_SET);
    if (result != HAL_OK) {
        *transmitted = 0U;
        *received = 0U;
        (void)HAL_SPI_Abort(handle);
        (void)HAL_SPI_DeInit(handle);
        if (!spi_configure(handle, handle->Instance)) {
#ifdef CONFIG_G431_BUS_SPI1_PA5_PA6_PA7_CS_PA15
            if (handle == &spi1) spi1_backend_failed = true;
#endif
#ifdef CONFIG_G431_BUS_SPI2_PB13_PB14_PB15_CS_PB12
            if (handle == &spi2) spi2_backend_failed = true;
#endif
        }
    }
    return hal_status(result, 0U);
}

#ifdef CONFIG_G431_BUS_I2C1_PB6_PB7
#if defined(RBSP_F072_BUS_ENABLED)
void I2C1_IRQHandler(void) {
    HAL_I2C_EV_IRQHandler(&i2c1);
    HAL_I2C_ER_IRQHandler(&i2c1);
}
#else
void I2C1_EV_IRQHandler(void) {
    HAL_I2C_EV_IRQHandler(&i2c1);
}

void I2C1_ER_IRQHandler(void) {
    HAL_I2C_ER_IRQHandler(&i2c1);
}
#endif
#endif
#endif
