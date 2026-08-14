#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * 三款 STM32 目标共用的板级波形后端。协议层只看见逻辑通道；这里负责把通道
 * 映射到定时器、引脚和 DMA。运行时资源清单完成后，映射表将由持久化配置生成。
 */
bool rbsp_board_waveform_init(void);

#if defined(CONFIG_REMOTEBSP_PWM)
bool rbsp_board_pwm_configure(uint8_t channel, uint32_t frequency_hz,
                              uint16_t duty, bool active_low);
bool rbsp_board_pwm_write(uint8_t channel, uint16_t duty);
bool rbsp_board_pwm_stop(uint8_t channel);
#endif

#if defined(CONFIG_REMOTEBSP_TIMED_BITSTREAM)
bool rbsp_board_timed_bitstream_configure(uint8_t channel,
                                          uint32_t bit_period_ns,
                                          uint32_t zero_high_ns,
                                          uint32_t one_high_ns,
                                          uint32_t reset_time_us);
bool rbsp_board_timed_bitstream_write(uint8_t channel,
                                      const uint8_t* data,
                                      uint16_t bit_count);
bool rbsp_board_timed_bitstream_busy(uint8_t channel);
bool rbsp_board_timed_bitstream_abort(uint8_t channel);
#endif
