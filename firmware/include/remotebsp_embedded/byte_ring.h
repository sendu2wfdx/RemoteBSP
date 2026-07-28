#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * 单生产者、单消费者字节环形缓冲。为区分空和满，内部始终保留一个空槽，
 * 因此可用容量为 storage_size - 1。
 *
 * 本模块不自行屏蔽中断。主循环和中断同时访问同一个缓冲时，调用方应在需要
 * “检查剩余空间并整段写入”这类原子操作时建立临界区。
 */
typedef struct {
    uint8_t* storage;
    size_t storage_size;
    volatile size_t head;
    volatile size_t tail;
} rbsp_byte_ring_t;

bool rbsp_byte_ring_init(rbsp_byte_ring_t* ring, uint8_t* storage,
                         size_t storage_size);
void rbsp_byte_ring_clear(rbsp_byte_ring_t* ring);
size_t rbsp_byte_ring_size(const rbsp_byte_ring_t* ring);
size_t rbsp_byte_ring_free(const rbsp_byte_ring_t* ring);
bool rbsp_byte_ring_push(rbsp_byte_ring_t* ring, uint8_t value);
bool rbsp_byte_ring_pop(rbsp_byte_ring_t* ring, uint8_t* value);
bool rbsp_byte_ring_write_exact(rbsp_byte_ring_t* ring,
                                const uint8_t* data, size_t length);
size_t rbsp_byte_ring_read(rbsp_byte_ring_t* ring, uint8_t* data,
                           size_t capacity);
