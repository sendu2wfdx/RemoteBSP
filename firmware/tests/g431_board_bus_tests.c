#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "board_bus.h"
#include "stm32g4xx_hal.h"

I2C_TypeDef rbsp_test_i2c1;
SPI_TypeDef rbsp_test_spi1, rbsp_test_spi2;
GPIO_TypeDef rbsp_test_gpioa, rbsp_test_gpiob;

static uint32_t tick, i2c_error, i2c_busy_reads;
static HAL_StatusTypeDef i2c_init_result, i2c_tx_result, i2c_rx_result;
static unsigned i2c_seq_tx_calls, i2c_seq_rx_calls, i2c_tx_calls;
static unsigned i2c_rx_calls, i2c_deinit_calls, i2c_recovery_pulses;
static HAL_StatusTypeDef spi_result[16], spi_init_result;
static uint8_t spi_rx[16], spi_tx[16];
static unsigned spi_calls, cs_low, cs_high, spi_abort, spi_deinit;

static void reset_stub(void) {
    tick = i2c_error = i2c_busy_reads = 0U;
    i2c_init_result = i2c_tx_result = i2c_rx_result = HAL_OK;
    i2c_seq_tx_calls = i2c_seq_rx_calls = i2c_tx_calls = 0U;
    i2c_rx_calls = i2c_deinit_calls = i2c_recovery_pulses = 0U;
    spi_init_result = HAL_OK;
    memset(spi_result, 0, sizeof(spi_result));
    memset(spi_rx, 0, sizeof(spi_rx));
    memset(spi_tx, 0, sizeof(spi_tx));
    spi_calls = cs_low = cs_high = spi_abort = spi_deinit = 0U;
}

uint32_t HAL_GetTick(void) { return tick++; }
HAL_StatusTypeDef HAL_I2C_Init(I2C_HandleTypeDef* h) { (void)h; return i2c_init_result; }
HAL_StatusTypeDef HAL_I2C_DeInit(I2C_HandleTypeDef* h) { (void)h; ++i2c_deinit_calls; return HAL_OK; }
HAL_StatusTypeDef HAL_I2CEx_ConfigAnalogFilter(I2C_HandleTypeDef* h, uint32_t v) { (void)h; (void)v; return HAL_OK; }
HAL_StatusTypeDef HAL_I2CEx_ConfigDigitalFilter(I2C_HandleTypeDef* h, uint32_t v) { (void)h; (void)v; return HAL_OK; }
HAL_StatusTypeDef HAL_I2C_Master_Seq_Transmit_IT(I2C_HandleTypeDef* h, uint16_t a, uint8_t* d, uint16_t n, uint32_t f) { (void)h; (void)a; (void)d; (void)n; (void)f; ++i2c_seq_tx_calls; return i2c_tx_result; }
HAL_StatusTypeDef HAL_I2C_Master_Seq_Receive_IT(I2C_HandleTypeDef* h, uint16_t a, uint8_t* d, uint16_t n, uint32_t f) { (void)h; (void)a; (void)d; (void)n; (void)f; ++i2c_seq_rx_calls; return i2c_rx_result; }
HAL_StatusTypeDef HAL_I2C_Master_Transmit(I2C_HandleTypeDef* h, uint16_t a, uint8_t* d, uint16_t n, uint32_t t) { (void)h; (void)a; (void)d; (void)n; (void)t; ++i2c_tx_calls; return i2c_tx_result; }
HAL_StatusTypeDef HAL_I2C_Master_Receive(I2C_HandleTypeDef* h, uint16_t a, uint8_t* d, uint16_t n, uint32_t t) { (void)h; (void)a; (void)d; (void)n; (void)t; ++i2c_rx_calls; return i2c_rx_result; }
uint32_t HAL_I2C_GetState(I2C_HandleTypeDef* h) { (void)h; if (i2c_busy_reads) { --i2c_busy_reads; return 1U; } return HAL_I2C_STATE_READY; }
uint32_t HAL_I2C_GetError(I2C_HandleTypeDef* h) { (void)h; return i2c_error; }
void HAL_I2C_EV_IRQHandler(I2C_HandleTypeDef* h) { (void)h; }
void HAL_I2C_ER_IRQHandler(I2C_HandleTypeDef* h) { (void)h; }
HAL_StatusTypeDef HAL_SPI_Init(SPI_HandleTypeDef* h) { (void)h; return spi_init_result; }
HAL_StatusTypeDef HAL_SPI_DeInit(SPI_HandleTypeDef* h) { (void)h; ++spi_deinit; return HAL_OK; }
HAL_StatusTypeDef HAL_SPI_Abort(SPI_HandleTypeDef* h) { (void)h; ++spi_abort; return HAL_OK; }
HAL_StatusTypeDef HAL_SPI_TransmitReceive(SPI_HandleTypeDef* h, uint8_t* tx, uint8_t* rx, uint16_t n, uint32_t t) { (void)h; (void)n; (void)t; unsigned i = spi_calls++; spi_tx[i] = *tx; *rx = spi_rx[i]; return spi_result[i]; }
void HAL_GPIO_Init(GPIO_TypeDef* p, GPIO_InitTypeDef* i) { (void)p; (void)i; }
void HAL_GPIO_WritePin(GPIO_TypeDef* p, uint16_t pin, uint32_t state) { (void)p; if (p == GPIOB && pin == GPIO_PIN_6 && state == GPIO_PIN_RESET) ++i2c_recovery_pulses; if (state == GPIO_PIN_RESET) ++cs_low; else ++cs_high; }
void HAL_NVIC_SetPriority(int i, uint32_t p, uint32_t s) { (void)i; (void)p; (void)s; }
void HAL_NVIC_EnableIRQ(int i) { (void)i; }

static const rbsp_bus_resource_config_t* resource(uint8_t kind, uint8_t controller) {
    const rbsp_bus_resource_config_t* all = rbsp_g431_bus_resources();
    for (uint8_t i = 0; i < rbsp_g431_bus_resource_count(); ++i)
        if (all[i].kind == kind && all[i].controller == controller) return &all[i];
    return NULL;
}

int main(void) {
    reset_stub();
    assert(rbsp_g431_bus_init());
    const rbsp_bus_resource_config_t* i2c = resource(RBSP_BUS_I2C_DEVICE, 1U);
    const rbsp_bus_resource_config_t* spi = resource(RBSP_BUS_SPI_DEVICE, 1U);
    assert(i2c && spi);
    assert(i2c->flags == (RBSP_BUS_CONTRACT_I2C_REPEATED_START |
                          RBSP_BUS_CONTRACT_I2C_RECOVERY));
    assert(spi->flags == (RBSP_BUS_CONTRACT_SPI_FULL_DUPLEX |
                          RBSP_BUS_CONTRACT_SPI_KEEP_CHIP_SELECT));

    uint8_t tx[] = {0x11, 0x22}; uint8_t rx[4] = {0}; uint16_t sent, got;

    /* 四种 flag 组合：RepeatedStart 只选择总线序列，Recovery 只授权故障恢复。 */
    const uint16_t combinations[] = {
        0U,
        RBSP_BUS_CONTRACT_I2C_REPEATED_START,
        RBSP_BUS_CONTRACT_I2C_RECOVERY,
        RBSP_BUS_CONTRACT_I2C_REPEATED_START |
            RBSP_BUS_CONTRACT_I2C_RECOVERY,
    };
    for (unsigned index = 0U; index < 4U; ++index) {
        reset_stub();
        assert(rbsp_g431_i2c_transfer(i2c, 10000U, combinations[index],
                                     tx, 1U, rx, 1U, &sent, &got) ==
               RBSP_BUS_TRANSACTION_OK);
        assert(sent == 1U && got == 1U && i2c_deinit_calls == 0U &&
               i2c_recovery_pulses == 0U);
        if ((combinations[index] &
             RBSP_BUS_CONTRACT_I2C_REPEATED_START) != 0U) {
            assert(i2c_seq_tx_calls == 1U && i2c_seq_rx_calls == 1U);
            assert(i2c_tx_calls == 0U && i2c_rx_calls == 0U);
        } else {
            assert(i2c_tx_calls == 1U && i2c_rx_calls == 1U);
            assert(i2c_seq_tx_calls == 0U && i2c_seq_rx_calls == 0U);
        }
    }

    reset_stub();
    i2c_busy_reads = 20U;
    assert(rbsp_g431_i2c_transfer(
               i2c, 1000U, RBSP_BUS_CONTRACT_I2C_REPEATED_START,
               tx, 1U, rx, 1U, &sent, &got) ==
           RBSP_BUS_TRANSACTION_TIMEOUT);
    assert(sent == 0U && got == 0U);

    /* 未授权恢复时原样返回 NACK，不切换引脚、不重新初始化。 */
    reset_stub();
    i2c_tx_result = HAL_ERROR; i2c_error = HAL_I2C_ERROR_AF;
    assert(rbsp_g431_i2c_transfer(i2c, 1000U, 0U, tx, 1U, rx, 0U, &sent, &got) == RBSP_BUS_TRANSACTION_NACK);
    assert(i2c_deinit_calls == 0U && i2c_recovery_pulses == 0U);

    /* 授权恢复时保留原始 NACK，同时完整执行九个 SCL 脉冲和重配置。 */
    reset_stub();
    i2c_tx_result = HAL_ERROR; i2c_error = HAL_I2C_ERROR_AF;
    assert(rbsp_g431_i2c_transfer(
               i2c, 1000U, RBSP_BUS_CONTRACT_I2C_RECOVERY,
               tx, 1U, rx, 0U, &sent, &got) == RBSP_BUS_TRANSACTION_NACK);
    assert(i2c_deinit_calls == 1U && i2c_recovery_pulses == 9U);

    /* 恢复重配失败必须锁存，后续事务不可再次碰触故障后端。 */
    reset_stub();
    i2c_tx_result = HAL_ERROR; i2c_init_result = HAL_ERROR;
    assert(rbsp_g431_i2c_transfer(
               i2c, 1000U, RBSP_BUS_CONTRACT_I2C_RECOVERY,
               tx, 1U, rx, 0U, &sent, &got) == RBSP_BUS_TRANSACTION_FAULT);
    assert(i2c_deinit_calls == 1U && i2c_recovery_pulses == 9U);
    i2c_tx_result = HAL_OK; i2c_init_result = HAL_OK;
    assert(rbsp_g431_i2c_transfer(i2c, 1000U, 0U, tx, 1U, rx, 0U, &sent, &got) == RBSP_BUS_TRANSACTION_FAULT);
    rbsp_resource_runtime_status_t status = {0};
    assert(rbsp_g431_bus_resource_status(11U, 1U, &status) && status.backend_failed);

    reset_stub();
    for (unsigned i = 0; i < 4; ++i) spi_rx[i] = (uint8_t)(0xA0U + i);
    assert(rbsp_g431_spi_transfer(spi, 10000U, 0U, 0xEEU, tx, 2U, rx, 4U, &sent, &got) == RBSP_BUS_TRANSACTION_OK);
    assert(sent == 2U && got == 4U && spi_calls == 4U);
    assert(spi_tx[0] == 0x11U && spi_tx[1] == 0x22U && spi_tx[2] == 0xEEU && spi_tx[3] == 0xEEU);
    assert(rx[0] == 0xA0U && rx[3] == 0xA3U && cs_low == 1U && cs_high == 1U);

    reset_stub();
    assert(rbsp_g431_spi_transfer(spi, 10000U, 0U, 0xEEU, tx, 2U, rx, 1U, &sent, &got) == RBSP_BUS_TRANSACTION_OK);
    assert(sent == 2U && got == 1U && spi_calls == 2U);

    /* 向上取整后的 1ms 是整个事务预算，不是每字节重新计时。 */
    reset_stub();
    assert(rbsp_g431_spi_transfer(spi, 1U, 0U, 0U, tx, 1U, rx, 1U, &sent, &got) == RBSP_BUS_TRANSACTION_TIMEOUT);
    assert(spi_calls == 0U && sent == 0U && got == 0U && cs_high == 1U);

    reset_stub(); spi_result[1] = HAL_BUSY;
    assert(rbsp_g431_spi_transfer(spi, 10000U, 0U, 0U, tx, 2U, rx, 2U, &sent, &got) == RBSP_BUS_TRANSACTION_BUSY);
    assert(sent == 0U && got == 0U && cs_low == 1U && cs_high == 1U);
    assert(spi_abort == 1U && spi_deinit == 1U);

    /* 恢复时重新初始化失败也要锁存，避免后续事务访问坏后端。 */
    reset_stub(); spi_result[0] = HAL_ERROR; spi_init_result = HAL_ERROR;
    assert(rbsp_g431_spi_transfer(spi, 10000U, 0U, 0U, tx, 1U, rx, 1U, &sent, &got) == RBSP_BUS_TRANSACTION_FAULT);
    status = (rbsp_resource_runtime_status_t){0};
    assert(rbsp_g431_bus_resource_status(13U, 1U, &status) && status.backend_failed);
    unsigned calls_after_fault = spi_calls;
    spi_init_result = HAL_OK; spi_result[0] = HAL_OK;
    assert(rbsp_g431_spi_transfer(spi, 10000U, 0U, 0U, tx, 1U, rx, 1U, &sent, &got) == RBSP_BUS_TRANSACTION_FAULT);
    assert(spi_calls == calls_after_fault);
    puts("G431 单板 I2C/SPI HAL 主机桩测试通过");
    return 0;
}
