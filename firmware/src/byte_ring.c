#include "remotebsp_embedded/byte_ring.h"

bool rbsp_byte_ring_init(rbsp_byte_ring_t* ring, uint8_t* storage,
                         size_t storage_size) {
    if (ring == NULL || storage == NULL || storage_size < 2U) {
        return false;
    }
    ring->storage = storage;
    ring->storage_size = storage_size;
    ring->head = 0U;
    ring->tail = 0U;
    return true;
}

void rbsp_byte_ring_clear(rbsp_byte_ring_t* ring) {
    if (ring == NULL) {
        return;
    }
    ring->head = 0U;
    ring->tail = 0U;
}

size_t rbsp_byte_ring_size(const rbsp_byte_ring_t* ring) {
    if (ring == NULL || ring->storage == NULL ||
        ring->storage_size < 2U) {
        return 0U;
    }
    const size_t head = ring->head;
    const size_t tail = ring->tail;
    return head >= tail ? head - tail
                        : ring->storage_size - tail + head;
}

size_t rbsp_byte_ring_free(const rbsp_byte_ring_t* ring) {
    if (ring == NULL || ring->storage == NULL ||
        ring->storage_size < 2U) {
        return 0U;
    }
    return ring->storage_size - 1U - rbsp_byte_ring_size(ring);
}

bool rbsp_byte_ring_push(rbsp_byte_ring_t* ring, uint8_t value) {
    if (ring == NULL || ring->storage == NULL ||
        ring->storage_size < 2U) {
        return false;
    }
    size_t next = ring->head + 1U;
    if (next == ring->storage_size) {
        next = 0U;
    }
    if (next == ring->tail) {
        return false;
    }
    ring->storage[ring->head] = value;
    ring->head = next;
    return true;
}

bool rbsp_byte_ring_pop(rbsp_byte_ring_t* ring, uint8_t* value) {
    if (ring == NULL || value == NULL || ring->storage == NULL ||
        ring->storage_size < 2U || ring->head == ring->tail) {
        return false;
    }
    *value = ring->storage[ring->tail];
    size_t next = ring->tail + 1U;
    if (next == ring->storage_size) {
        next = 0U;
    }
    ring->tail = next;
    return true;
}

bool rbsp_byte_ring_write_exact(rbsp_byte_ring_t* ring,
                                const uint8_t* data, size_t length) {
    if (length == 0U) {
        return true;
    }
    if (data == NULL || length > rbsp_byte_ring_free(ring)) {
        return false;
    }
    for (size_t index = 0U; index < length; ++index) {
        if (!rbsp_byte_ring_push(ring, data[index])) {
            return false;
        }
    }
    return true;
}

size_t rbsp_byte_ring_read(rbsp_byte_ring_t* ring, uint8_t* data,
                           size_t capacity) {
    if (ring == NULL || data == NULL) {
        return 0U;
    }
    size_t count = 0U;
    while (count < capacity &&
           rbsp_byte_ring_pop(ring, data + count)) {
        ++count;
    }
    return count;
}
