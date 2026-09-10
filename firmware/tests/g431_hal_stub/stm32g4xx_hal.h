#pragma once

#include <stdint.h>

typedef enum { HAL_OK = 0, HAL_ERROR = 1, HAL_BUSY = 2, HAL_TIMEOUT = 3 } HAL_StatusTypeDef;
typedef struct { uint32_t unused; } I2C_TypeDef;
typedef struct { uint32_t unused; } SPI_TypeDef;
typedef struct { uint32_t unused; } GPIO_TypeDef;
typedef struct {
    uint32_t Timing, OwnAddress1, AddressingMode, DualAddressMode;
    uint32_t OwnAddress2, OwnAddress2Masks, GeneralCallMode, NoStretchMode;
} I2C_InitTypeDef;
typedef struct { I2C_TypeDef* Instance; I2C_InitTypeDef Init; } I2C_HandleTypeDef;
typedef struct {
    uint32_t Mode, Direction, DataSize, CLKPolarity, CLKPhase, NSS;
    uint32_t BaudRatePrescaler, FirstBit, TIMode, CRCCalculation;
    uint32_t CRCPolynomial, CRCLength, NSSPMode;
} SPI_InitTypeDef;
typedef struct { SPI_TypeDef* Instance; SPI_InitTypeDef Init; } SPI_HandleTypeDef;
typedef struct { uint32_t Pin, Mode, Pull, Speed, Alternate; } GPIO_InitTypeDef;

extern I2C_TypeDef rbsp_test_i2c1;
extern SPI_TypeDef rbsp_test_spi1, rbsp_test_spi2;
extern GPIO_TypeDef rbsp_test_gpioa, rbsp_test_gpiob;
#define I2C1 (&rbsp_test_i2c1)
#define SPI1 (&rbsp_test_spi1)
#define SPI2 (&rbsp_test_spi2)
#define GPIOA (&rbsp_test_gpioa)
#define GPIOB (&rbsp_test_gpiob)

#define GPIO_PIN_5 (1U << 5)
#define GPIO_PIN_6 (1U << 6)
#define GPIO_PIN_7 (1U << 7)
#define GPIO_PIN_12 (1U << 12)
#define GPIO_PIN_13 (1U << 13)
#define GPIO_PIN_14 (1U << 14)
#define GPIO_PIN_15 (1U << 15)
#define GPIO_PIN_RESET 0U
#define GPIO_PIN_SET 1U
#define GPIO_MODE_OUTPUT_OD 1U
#define GPIO_MODE_AF_OD 2U
#define GPIO_MODE_AF_PP 3U
#define GPIO_MODE_OUTPUT_PP 4U
#define GPIO_PULLUP 1U
#define GPIO_NOPULL 0U
#define GPIO_SPEED_FREQ_HIGH 2U
#define GPIO_SPEED_FREQ_VERY_HIGH 3U
#define GPIO_AF4_I2C1 4U
#define GPIO_AF5_SPI1 5U
#define GPIO_AF5_SPI2 5U
#define I2C_ADDRESSINGMODE_7BIT 1U
#define I2C_DUALADDRESS_DISABLE 0U
#define I2C_OA2_NOMASK 0U
#define I2C_GENERALCALL_DISABLE 0U
#define I2C_NOSTRETCH_DISABLE 0U
#define I2C_ANALOGFILTER_ENABLE 1U
#define I2C_FIRST_FRAME 1U
#define I2C_LAST_FRAME 2U
#define HAL_I2C_STATE_READY 0U
#define HAL_I2C_ERROR_NONE 0U
#define HAL_I2C_ERROR_AF 4U
#define SPI_MODE_MASTER 1U
#define SPI_DIRECTION_2LINES 2U
#define SPI_DATASIZE_8BIT 8U
#define SPI_POLARITY_LOW 0U
#define SPI_PHASE_1EDGE 0U
#define SPI_NSS_SOFT 1U
#define SPI_BAUDRATEPRESCALER_8 8U
#define SPI_FIRSTBIT_MSB 0U
#define SPI_TIMODE_DISABLE 0U
#define SPI_CRCCALCULATION_DISABLE 0U
#define SPI_CRC_LENGTH_DATASIZE 0U
#define SPI_NSS_PULSE_DISABLE 0U
#define I2C1_EV_IRQn 1
#define I2C1_ER_IRQn 2

#define __HAL_RCC_GPIOA_CLK_ENABLE() ((void)0)
#define __HAL_RCC_GPIOB_CLK_ENABLE() ((void)0)
#define __HAL_RCC_I2C1_CLK_ENABLE() ((void)0)
#define __HAL_RCC_SPI1_CLK_ENABLE() ((void)0)
#define __HAL_RCC_SPI2_CLK_ENABLE() ((void)0)

uint32_t HAL_GetTick(void);
HAL_StatusTypeDef HAL_I2C_Init(I2C_HandleTypeDef* handle);
HAL_StatusTypeDef HAL_I2C_DeInit(I2C_HandleTypeDef* handle);
HAL_StatusTypeDef HAL_I2CEx_ConfigAnalogFilter(I2C_HandleTypeDef* handle, uint32_t value);
HAL_StatusTypeDef HAL_I2CEx_ConfigDigitalFilter(I2C_HandleTypeDef* handle, uint32_t value);
HAL_StatusTypeDef HAL_I2C_Master_Seq_Transmit_IT(I2C_HandleTypeDef*, uint16_t, uint8_t*, uint16_t, uint32_t);
HAL_StatusTypeDef HAL_I2C_Master_Seq_Receive_IT(I2C_HandleTypeDef*, uint16_t, uint8_t*, uint16_t, uint32_t);
HAL_StatusTypeDef HAL_I2C_Master_Transmit(I2C_HandleTypeDef*, uint16_t, uint8_t*, uint16_t, uint32_t);
HAL_StatusTypeDef HAL_I2C_Master_Receive(I2C_HandleTypeDef*, uint16_t, uint8_t*, uint16_t, uint32_t);
uint32_t HAL_I2C_GetState(I2C_HandleTypeDef* handle);
uint32_t HAL_I2C_GetError(I2C_HandleTypeDef* handle);
void HAL_I2C_EV_IRQHandler(I2C_HandleTypeDef* handle);
void HAL_I2C_ER_IRQHandler(I2C_HandleTypeDef* handle);
HAL_StatusTypeDef HAL_SPI_Init(SPI_HandleTypeDef* handle);
HAL_StatusTypeDef HAL_SPI_DeInit(SPI_HandleTypeDef* handle);
HAL_StatusTypeDef HAL_SPI_Abort(SPI_HandleTypeDef* handle);
HAL_StatusTypeDef HAL_SPI_TransmitReceive(SPI_HandleTypeDef*, uint8_t*, uint8_t*, uint16_t, uint32_t);
void HAL_GPIO_Init(GPIO_TypeDef*, GPIO_InitTypeDef*);
void HAL_GPIO_WritePin(GPIO_TypeDef*, uint16_t, uint32_t);
void HAL_NVIC_SetPriority(int, uint32_t, uint32_t);
void HAL_NVIC_EnableIRQ(int);
