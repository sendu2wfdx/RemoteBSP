#include "remotebsp/device_params/store.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TEST_PAGE_SIZE 2048U
#define TEST_REGION_SIZE (TEST_PAGE_SIZE * 2U)

typedef struct {
    uint8_t bytes[TEST_REGION_SIZE];
    bool fail_next_program;
} memory_flash;

static const uint8_t* memory_map(void* context, uint32_t offset,
                                 size_t length) {
    const memory_flash* flash = context;
    if (offset > TEST_REGION_SIZE || length > TEST_REGION_SIZE - offset) {
        return NULL;
    }
    return flash->bytes + offset;
}

static bool memory_erase(void* context, uint32_t offset, size_t length) {
    memory_flash* flash = context;
    if (length != TEST_PAGE_SIZE || offset % TEST_PAGE_SIZE != 0U ||
        offset > TEST_REGION_SIZE || length > TEST_REGION_SIZE - offset) {
        return false;
    }
    memset(flash->bytes + offset, 0xFF, length);
    return true;
}

static bool memory_program(void* context, uint32_t offset,
                           const uint8_t* data, size_t length) {
    memory_flash* flash = context;
    size_t index;
    if (flash->fail_next_program) {
        flash->fail_next_program = false;
        return false;
    }
    if (data == NULL || offset % 8U != 0U || length % 8U != 0U ||
        offset > TEST_REGION_SIZE || length > TEST_REGION_SIZE - offset) {
        return false;
    }
    for (index = 0U; index < length; ++index) {
        if ((flash->bytes[offset + index] & data[index]) != data[index]) {
            return false;
        }
        flash->bytes[offset + index] &= data[index];
    }
    return true;
}

static rbsp_device_param_backend make_backend(memory_flash* flash) {
    rbsp_device_param_backend backend;
    memset(&backend, 0, sizeof(backend));
    backend.context = flash;
    backend.region_size = TEST_REGION_SIZE;
    backend.erase_size = TEST_PAGE_SIZE;
    backend.program_size = 8U;
    backend.map = memory_map;
    backend.erase = memory_erase;
    backend.program = memory_program;
    return backend;
}

int main(void) {
    memory_flash flash;
    rbsp_device_param_store store;
    rbsp_device_param_backend backend;
    rbsp_device_param_record record;
    uint8_t workspace[RBSP_DEVICE_PARAM_STORE_MAX_IMAGE_SIZE];
    static const uint8_t serial[] = "RBSP-2026-00001";
    static const uint8_t revision[] = "V1.0";
    static const uint8_t uuid[16] = {
        0x10U, 0x11U, 0x12U, 0x13U, 0x14U, 0x15U, 0x16U, 0x17U,
        0x18U, 0x19U, 0x1AU, 0x1BU, 0x1CU, 0x1DU, 0x1EU, 0x1FU};

    memset(&flash, 0xFF, sizeof(flash));
    flash.fail_next_program = false;
    backend = make_backend(&flash);
    assert(rbsp_device_param_store_init(&store, &backend));
    assert(rbsp_device_param_store_boot(&store));
    assert(store.active_page == RBSP_DEVICE_PARAM_STORE_NO_PAGE);
    assert(store.generation == 0U);

    assert(rbsp_device_param_store_set(
        &store, 0U, RBSP_DEVICE_PARAM_SERIAL_NUMBER,
        serial, sizeof(serial) - 1U, workspace, sizeof(workspace)));
    assert(store.generation == 1U);
    assert(rbsp_device_param_store_get(
        &store, RBSP_DEVICE_PARAM_SERIAL_NUMBER, &record));
    assert(record.length == sizeof(serial) - 1U);
    assert(memcmp(record.value, serial, record.length) == 0);

    assert(!rbsp_device_param_store_set(
        &store, 0U, RBSP_DEVICE_PARAM_HARDWARE_REVISION,
        revision, sizeof(revision) - 1U, workspace, sizeof(workspace)));
    assert(store.last_error ==
           RBSP_DEVICE_PARAM_STORE_ERROR_STALE_GENERATION);

    assert(rbsp_device_param_store_set(
        &store, 1U, RBSP_DEVICE_PARAM_HARDWARE_REVISION,
        revision, sizeof(revision) - 1U, workspace, sizeof(workspace)));
    assert(store.generation == 2U);
    assert(store.record_count == 2U);

    assert(rbsp_device_param_store_set(
        &store, 2U, RBSP_DEVICE_PARAM_DEVICE_UUID,
        uuid, sizeof(uuid), workspace, sizeof(workspace)));
    assert(!rbsp_device_param_store_set(
        &store, 3U, RBSP_DEVICE_PARAM_DEVICE_UUID,
        uuid, sizeof(uuid), workspace, sizeof(workspace)));
    assert(store.last_error == RBSP_DEVICE_PARAM_STORE_ERROR_WRITE_ONCE);

    flash.fail_next_program = true;
    assert(!rbsp_device_param_store_set(
        &store, 3U, RBSP_DEVICE_PARAM_DEVICE_NAME,
        (const uint8_t*)"test", 4U, workspace, sizeof(workspace)));
    assert(rbsp_device_param_store_boot(&store));
    assert(store.generation == 3U);
    assert(rbsp_device_param_store_get(
        &store, RBSP_DEVICE_PARAM_SERIAL_NUMBER, &record));

    puts("设备参数Flash模拟存储测试通过");
    return 0;
}
