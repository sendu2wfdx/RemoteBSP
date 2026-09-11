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

typedef struct {
    uint16_t id;
    uint8_t type;
    uint8_t flags;
    const uint8_t* value;
    uint16_t length;
} golden_record;

static void put_u16(uint8_t* output, uint16_t value) {
    output[0] = (uint8_t)value;
    output[1] = (uint8_t)(value >> 8U);
}

static void put_u32(uint8_t* output, uint32_t value) {
    output[0] = (uint8_t)value;
    output[1] = (uint8_t)(value >> 8U);
    output[2] = (uint8_t)(value >> 16U);
    output[3] = (uint8_t)(value >> 24U);
}

static uint32_t golden_crc_update(uint32_t crc, const uint8_t* data,
                                  size_t length) {
    size_t index;
    for (index = 0U; index < length; ++index) {
        uint8_t bit;
        crc ^= data[index];
        for (bit = 0U; bit < 8U; ++bit) {
            const uint32_t mask = (uint32_t)-(int32_t)(crc & 1U);
            crc = (crc >> 1U) ^ (0xEDB88320U & mask);
        }
    }
    return crc;
}

static void finish_adc_crc(uint8_t* value) {
    put_u32(value + 60U, ~golden_crc_update(UINT32_MAX, value, 60U));
}

static void test_adc_calibration_formats(void) {
    uint8_t value[RBSP_DEVICE_PARAM_ADC_CALIBRATION_V1_SIZE] = {0U};
    rbsp_device_param_adc_calibration legacy = {65536, -100, 3300000U};
    uint32_t crc;
    memcpy(value, "ADCC", 4U);
    value[4] = 1U; /* 格式版本 */
    value[5] = 1U; /* 增益偏移模式 */
    value[6] = 2U; /* 通道 */
    value[7] = 12U;
    put_u32(value + 8U, 0x05000003U);
    put_u32(value + 12U, 0U);
    put_u32(value + 16U, 3300000U);
    put_u32(value + 20U, 3300000U);
    put_u32(value + 24U, 65536U);
    put_u32(value + 28U, (uint32_t)-100);
    crc = ~golden_crc_update(UINT32_MAX, value, 60U);
    put_u32(value + 60U, crc);
    assert(rbsp_device_param_validate_value(
        RBSP_DEVICE_PARAM_ADC_CALIBRATION_BASE + 2U,
        value, sizeof(value)));
    assert(!rbsp_device_param_validate_value(
        RBSP_DEVICE_PARAM_ADC_CALIBRATION_BASE + 1U,
        value, sizeof(value)));
    value[12] ^= 1U;
    assert(!rbsp_device_param_validate_value(
        RBSP_DEVICE_PARAM_ADC_CALIBRATION_BASE + 2U,
        value, sizeof(value)));
    assert(rbsp_device_param_validate_value(
        RBSP_DEVICE_PARAM_ADC_CALIBRATION_BASE,
        (const uint8_t*)&legacy, sizeof(legacy)));

    /* 参考电压必须落在声明量程内，即使重新计算CRC也不能放行。 */
    memset(value, 0, sizeof(value));
    memcpy(value, "ADCC", 4U);
    value[4] = 1U; value[5] = 1U; value[6] = 2U; value[7] = 12U;
    put_u32(value + 8U, 0x05000003U);
    put_u32(value + 12U, 4000000U);
    put_u32(value + 16U, 5000000U);
    put_u32(value + 20U, 3300000U);
    put_u32(value + 24U, 65536U);
    finish_adc_crc(value);
    assert(!rbsp_device_param_validate_value(
        RBSP_DEVICE_PARAM_ADC_CALIBRATION_BASE + 2U,
        value, sizeof(value)));

    /* 两点模式锁定16位以内原始码、严格递增点和全零保留区。 */
    memset(value, 0, sizeof(value));
    memcpy(value, "ADCC", 4U);
    value[4] = 1U; value[5] = 2U; value[6] = 2U; value[7] = 12U;
    value[32] = 2U;
    put_u32(value + 8U, 0x05000003U);
    put_u32(value + 12U, 0U);
    put_u32(value + 16U, 3300000U);
    put_u32(value + 20U, 3300000U);
    put_u16(value + 36U, 10U); put_u32(value + 38U, 10000U);
    put_u16(value + 42U, 4090U); put_u32(value + 44U, 3290000U);
    finish_adc_crc(value);
    assert(rbsp_device_param_validate_value(
        RBSP_DEVICE_PARAM_ADC_CALIBRATION_BASE + 2U,
        value, sizeof(value)));
    value[33] = 1U;
    finish_adc_crc(value);
    assert(!rbsp_device_param_validate_value(
        RBSP_DEVICE_PARAM_ADC_CALIBRATION_BASE + 2U,
        value, sizeof(value)));
    value[33] = 0U;
    put_u16(value + 42U, 4096U);
    finish_adc_crc(value);
    assert(!rbsp_device_param_validate_value(
        RBSP_DEVICE_PARAM_ADC_CALIBRATION_BASE + 2U,
        value, sizeof(value)));
}

static size_t make_golden_image(uint8_t* page, uint16_t version,
                                uint32_t generation,
                                const golden_record* records,
                                size_t record_count) {
    static const uint8_t marker[8] = {
        0x52U, 0x42U, 0x4EU, 0x56U, 0x43U, 0x4FU, 0x4DU, 0x31U};
    size_t offset = RBSP_DEVICE_PARAM_STORE_HEADER_SIZE;
    size_t index;
    uint32_t crc = UINT32_MAX;
    memset(page, 0xFF, TEST_PAGE_SIZE);
    memcpy(page, "RBNV", 4U);
    put_u16(page + 4U, version);
    put_u16(page + 6U, RBSP_DEVICE_PARAM_STORE_HEADER_SIZE);
    put_u16(page + 10U, (uint16_t)record_count);
    put_u32(page + 12U, generation);
    for (index = 0U; index < record_count; ++index) {
        const size_t padded =
            (8U + records[index].length + 3U) & ~(size_t)3U;
        put_u16(page + offset, records[index].id);
        page[offset + 2U] = records[index].type;
        page[offset + 3U] = records[index].flags;
        put_u16(page + offset + 4U, records[index].length);
        put_u16(page + offset + 6U, 0U);
        if (records[index].length > 0U) {
            memcpy(page + offset + 8U, records[index].value,
                   records[index].length);
        }
        offset += padded;
    }
    put_u16(page + 8U, (uint16_t)offset);
    memcpy(page + 24U, marker, sizeof(marker));
    crc = golden_crc_update(crc, page, 16U);
    crc = golden_crc_update(crc, page + 32U, offset - 32U);
    put_u32(page + 16U, ~crc);
    return offset;
}

static void reseal_golden_image(uint8_t* page, size_t length) {
    uint32_t crc = UINT32_MAX;
    crc = golden_crc_update(crc, page, 16U);
    crc = golden_crc_update(crc, page + 32U, length - 32U);
    put_u32(page + 16U, ~crc);
}

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
    backend.contract_version = RBSP_DEVICE_PARAM_BACKEND_CONTRACT_VERSION;
    backend.medium = RBSP_DEVICE_PARAM_MEDIUM_MOCK;
    backend.erased_value = RBSP_DEVICE_PARAM_BACKEND_ERASED_VALUE;
    backend.capability_flags =
        RBSP_DEVICE_PARAM_BACKEND_FLAG_ERASE_BEFORE_PROGRAM |
        RBSP_DEVICE_PARAM_BACKEND_FLAG_ONE_TO_ZERO_ONLY |
        RBSP_DEVICE_PARAM_BACKEND_FLAG_COMMIT_MARKER_LAST;
    return backend;
}

static bool memory_eeprom_program(void* context, uint32_t offset,
                                  const uint8_t* data, size_t length) {
    memory_flash* memory = context;
    if (memory->fail_next_program) {
        memory->fail_next_program = false;
        return false;
    }
    if (data == NULL || offset > TEST_REGION_SIZE ||
        length > TEST_REGION_SIZE - offset) {
        return false;
    }
    memcpy(memory->bytes + offset, data, length);
    return true;
}

static rbsp_device_param_backend make_eeprom_backend(memory_flash* memory) {
    rbsp_device_param_backend backend;
    memset(&backend, 0, sizeof(backend));
    backend.context = memory;
    backend.region_size = TEST_REGION_SIZE;
    backend.erase_size = TEST_PAGE_SIZE;
    backend.program_size = 1U;
    backend.map = memory_map;
    backend.erase = memory_erase;
    backend.program = memory_eeprom_program;
    backend.contract_version = RBSP_DEVICE_PARAM_BACKEND_CONTRACT_VERSION;
    backend.medium = RBSP_DEVICE_PARAM_MEDIUM_EXTERNAL_EEPROM;
    backend.erased_value = RBSP_DEVICE_PARAM_BACKEND_ERASED_VALUE;
    backend.capability_flags = RBSP_DEVICE_PARAM_BACKEND_FLAG_BYTE_REWRITABLE |
        RBSP_DEVICE_PARAM_BACKEND_FLAG_COMMIT_MARKER_LAST;
    return backend;
}

static void test_external_eeprom_contract_and_fault_recovery(void) {
    memory_flash memory;
    rbsp_device_param_backend backend;
    rbsp_device_param_store store;
    rbsp_device_param_record record;
    uint8_t workspace[RBSP_DEVICE_PARAM_STORE_MAX_IMAGE_SIZE];
    static const uint8_t first[] = "EEPROM-A";
    static const uint8_t second[] = "EEPROM-B";
    memset(&memory, 0xFF, sizeof(memory));
    memory.fail_next_program = false;
    backend = make_eeprom_backend(&memory);
    assert(rbsp_device_param_store_init(&store, &backend));
    assert(rbsp_device_param_store_boot(&store));
    assert(rbsp_device_param_store_set_commit_budget(&store, 2U));
    assert(rbsp_device_param_store_set(&store, 0U,
        RBSP_DEVICE_PARAM_DEVICE_NAME, first, sizeof(first) - 1U,
        workspace, sizeof(workspace)));
    assert(store.generation == 1U);
    memory.fail_next_program = true;
    assert(!rbsp_device_param_store_set(&store, 1U,
        RBSP_DEVICE_PARAM_DEVICE_NAME, second, sizeof(second) - 1U,
        workspace, sizeof(workspace)));
    assert(rbsp_device_param_store_boot(&store));
    assert(store.generation == 1U);
    assert(rbsp_device_param_store_get(
        &store, RBSP_DEVICE_PARAM_DEVICE_NAME, &record));
    assert(record.length == sizeof(first) - 1U &&
           memcmp(record.value, first, record.length) == 0);
    assert(store.bad_page_mask != 0U);
    assert(!rbsp_device_param_store_set(&store, 1U,
        RBSP_DEVICE_PARAM_DEVICE_NAME, second, sizeof(second) - 1U,
        workspace, sizeof(workspace)));
    assert(store.last_error == RBSP_DEVICE_PARAM_STORE_ERROR_BAD_PAGE);

    /* EEPROM Mock重启后由持久generation恢复并执行预算门禁。 */
    assert(rbsp_device_param_store_init(&store, &backend));
    assert(rbsp_device_param_store_boot(&store));
    assert(rbsp_device_param_store_set_commit_budget(&store, 1U));
    assert(!rbsp_device_param_store_set_commit_budget(&store, 2U));
    assert(!rbsp_device_param_store_set(&store, 1U,
        RBSP_DEVICE_PARAM_DEVICE_NAME, second, sizeof(second) - 1U,
        workspace, sizeof(workspace)));
    assert(store.last_error == RBSP_DEVICE_PARAM_STORE_ERROR_BUDGET_EXHAUSTED);

    backend.contract_version = 0U;
    assert(!rbsp_device_param_store_init(&store, &backend));
    backend = make_eeprom_backend(&memory);
    backend.capability_flags |= RBSP_DEVICE_PARAM_BACKEND_FLAG_ONE_TO_ZERO_ONLY;
    assert(!rbsp_device_param_store_init(&store, &backend));
}

static void test_endurance_budget_and_bad_page_isolation(void) {
    memory_flash flash;
    rbsp_device_param_backend backend;
    rbsp_device_param_store store;
    rbsp_device_param_store rebooted;
    rbsp_device_param_store_health health;
    uint8_t workspace[RBSP_DEVICE_PARAM_STORE_MAX_IMAGE_SIZE];
    static const uint8_t first[] = "budget-a";
    static const uint8_t second[] = "budget-b";
    memset(&flash, 0xFF, sizeof(flash));
    flash.fail_next_program = false;
    backend = make_backend(&flash);
    assert(rbsp_device_param_store_init(&store, &backend));
    assert(rbsp_device_param_store_boot(&store));
    assert(rbsp_device_param_store_set_commit_budget(&store, 1U));
    assert(rbsp_device_param_store_set(&store, 0U,
        RBSP_DEVICE_PARAM_DEVICE_NAME, first, sizeof(first) - 1U,
        workspace, sizeof(workspace)));
    assert(!rbsp_device_param_store_set(&store, 1U,
        RBSP_DEVICE_PARAM_DEVICE_NAME, second, sizeof(second) - 1U,
        workspace, sizeof(workspace)));
    assert(store.last_error == RBSP_DEVICE_PARAM_STORE_ERROR_BUDGET_EXHAUSTED);
    assert(rbsp_device_param_store_health_get(&store, &health));
    assert(health.persistent_commits == 1U && health.successful_commits == 1U &&
           health.write_attempts == 1U);

    /* 新实例从介质generation恢复预算事实，但运行时坏页状态不被伪造为持久证据。 */
    assert(rbsp_device_param_store_init(&rebooted, &backend));
    assert(rbsp_device_param_store_boot(&rebooted));
    assert(rbsp_device_param_store_set_commit_budget(&rebooted, 2U));
    flash.fail_next_program = true;
    assert(!rbsp_device_param_store_set(&rebooted, 1U,
        RBSP_DEVICE_PARAM_DEVICE_NAME, second, sizeof(second) - 1U,
        workspace, sizeof(workspace)));
    assert(rebooted.last_error == RBSP_DEVICE_PARAM_STORE_ERROR_IO);
    assert(rebooted.bad_page_mask == (uint8_t)(1U << 1U));
    assert(!rbsp_device_param_store_set(&rebooted, 1U,
        RBSP_DEVICE_PARAM_DEVICE_NAME, second, sizeof(second) - 1U,
        workspace, sizeof(workspace)));
    assert(rebooted.last_error == RBSP_DEVICE_PARAM_STORE_ERROR_BAD_PAGE);
    assert(rbsp_device_param_store_boot(&rebooted));
    assert(rebooted.generation == 1U && rebooted.bad_page_mask != 0U);
}

static void test_compatibility_golden_images(void) {
    memory_flash flash;
    rbsp_device_param_store store;
    rbsp_device_param_backend backend;
    rbsp_device_param_record record;
    rbsp_device_param_value_source source;
    uint8_t workspace[RBSP_DEVICE_PARAM_STORE_MAX_IMAGE_SIZE];
    uint8_t opaque_record[12];
    static const uint8_t old_name[] = "A";
    static const uint8_t unknown_tail[] = {0xDEU, 0xADU, 0xBEU};
    static const uint8_t new_name[] = "new-name";
    const golden_record old_records[] = {
        {RBSP_DEVICE_PARAM_DEVICE_NAME, RBSP_DEVICE_PARAM_TYPE_UTF8,
         RBSP_DEVICE_PARAM_FLAG_USER |
             RBSP_DEVICE_PARAM_FLAG_MAINTENANCE_WRITE,
         old_name, sizeof(old_name) - 1U},
        {0x7F01U, 0x91U, 0xA5U, unknown_tail, sizeof(unknown_tail)},
    };
    const golden_record short_record[] = {
        {RBSP_DEVICE_PARAM_SERIAL_NUMBER, RBSP_DEVICE_PARAM_TYPE_UTF8,
         RBSP_DEVICE_PARAM_FLAG_FACTORY |
             RBSP_DEVICE_PARAM_FLAG_MAINTENANCE_WRITE |
             RBSP_DEVICE_PARAM_FLAG_APPLY_AFTER_RESTART,
         NULL, 0U},
    };

    memset(&flash, 0xFF, sizeof(flash));
    flash.fail_next_program = false;
    backend = make_backend(&flash);
    assert(rbsp_device_param_store_init(&store, &backend));

    /* 旧schema的v1镜像可以缺少后来定义的字段，未知尾记录保持不透明。 */
    {
        const size_t image_length =
            make_golden_image(flash.bytes, 1U, 7U, old_records, 2U);
        /* 未知记录的保留字段和对齐填充也必须逐字节保留。 */
        flash.bytes[50U] = 0x5AU;
        flash.bytes[51U] = 0xC3U;
        flash.bytes[55U] = 0x69U;
        reseal_golden_image(flash.bytes, image_length);
        memcpy(opaque_record, flash.bytes + 44U, sizeof(opaque_record));
    }
    assert(rbsp_device_param_store_boot(&store));
    assert(store.generation == 7U && store.record_count == 2U);
    assert(rbsp_device_param_store_get(&store, 0x7F01U, &record));
    assert(record.type == 0x91U && record.flags == 0xA5U);
    assert(record.length == sizeof(unknown_tail));
    assert(memcmp(record.value, unknown_tail, sizeof(unknown_tail)) == 0);

    /* 默认值只在解析结果中出现，不改变镜像和代次。 */
    assert(rbsp_device_param_store_resolve(
        &store, RBSP_DEVICE_PARAM_MANUFACTURING_BATCH, &record, &source));
    assert(source == RBSP_DEVICE_PARAM_VALUE_DEFAULTED && record.length == 0U);
    assert(store.generation == 7U && store.record_count == 2U);
    assert(rbsp_device_param_store_resolve(
        &store, RBSP_DEVICE_PARAM_DEVICE_UUID, &record, &source));
    assert(source == RBSP_DEVICE_PARAM_VALUE_ABSENT && record.value == NULL);
    assert(rbsp_device_param_store_resolve(
        &store, RBSP_DEVICE_PARAM_DEVICE_NAME, &record, &source));
    assert(source == RBSP_DEVICE_PARAM_VALUE_PERSISTED);

    /* 已知字段更新后，未知记录的头和值逐字节保留。 */
    assert(rbsp_device_param_store_set(
        &store, 7U, RBSP_DEVICE_PARAM_DEVICE_NAME,
        new_name, sizeof(new_name) - 1U, workspace, sizeof(workspace)));
    assert(store.generation == 8U);
    assert(rbsp_device_param_store_get(&store, 0x7F01U, &record));
    assert(record.type == 0x91U && record.flags == 0xA5U &&
           record.length == sizeof(unknown_tail));
    assert(memcmp(record.value, unknown_tail, sizeof(unknown_tail)) == 0);
    assert(memcmp(flash.bytes + TEST_PAGE_SIZE + 32U, opaque_record,
                  sizeof(opaque_record)) == 0);

    /* 未来存储格式失败关闭，不把未知布局误读成v1。 */
    memset(&flash, 0xFF, sizeof(flash));
    make_golden_image(flash.bytes, 2U, 9U, old_records, 2U);
    assert(rbsp_device_param_store_boot(&store));
    assert(store.active_page == RBSP_DEVICE_PARAM_STORE_NO_PAGE);

    /* 已知字段短于当前最小长度时整页无效。 */
    memset(&flash, 0xFF, sizeof(flash));
    make_golden_image(flash.bytes, 1U, 10U, short_record, 1U);
    assert(rbsp_device_param_store_boot(&store));
    assert(store.active_page == RBSP_DEVICE_PARAM_STORE_NO_PAGE);

    /* 新页CRC损坏时回退到旧的完整镜像。 */
    memset(&flash, 0xFF, sizeof(flash));
    make_golden_image(flash.bytes, 1U, 10U, old_records, 2U);
    make_golden_image(flash.bytes + TEST_PAGE_SIZE, 1U, 11U,
                      old_records, 2U);
    flash.bytes[TEST_PAGE_SIZE + 32U] ^= 0x01U;
    assert(rbsp_device_param_store_boot(&store));
    assert(store.active_page == 0U && store.generation == 10U);

    /* RFC1982式32位代次比较允许UINT32_MAX之后回绕到0。 */
    memset(&flash, 0xFF, sizeof(flash));
    make_golden_image(flash.bytes, 1U, UINT32_MAX, old_records, 2U);
    make_golden_image(flash.bytes + TEST_PAGE_SIZE, 1U, 0U,
                      old_records, 2U);
    assert(rbsp_device_param_store_boot(&store));
    assert(store.active_page == 1U && store.generation == 0U);
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

    test_compatibility_golden_images();
    test_adc_calibration_formats();
    test_external_eeprom_contract_and_fault_recovery();
    test_endurance_budget_and_bad_page_isolation();

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
