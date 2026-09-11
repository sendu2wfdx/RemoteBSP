#include "remotebsp/device_params/boot_epoch.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TEST_PAGE_SIZE 1024U
#define TEST_REGION_SIZE (TEST_PAGE_SIZE * 2U)
#define TEST_SLOT_COUNT \
    (TEST_REGION_SIZE / RBSP_BOOT_EPOCH_JOURNAL_RECORD_SIZE)

typedef struct {
    uint8_t bytes[TEST_REGION_SIZE];
    uint32_t program_size;
    size_t remaining_bytes;
    bool limit_operations;
    uint32_t fail_after_program_call;
    uint32_t program_calls;
    uint32_t erase_count;
} memory_flash;

static const uint8_t* memory_map(void* context, uint32_t offset,
                                 size_t length) {
    const memory_flash* flash = context;
    if (offset > TEST_REGION_SIZE || length > TEST_REGION_SIZE - offset) {
        return NULL;
    }
    return flash->bytes + offset;
}

static bool consume_byte(memory_flash* flash) {
    if (!flash->limit_operations) {
        return true;
    }
    if (flash->remaining_bytes == 0U) {
        return false;
    }
    --flash->remaining_bytes;
    return true;
}

static bool memory_erase(void* context, uint32_t offset, size_t length) {
    memory_flash* flash = context;
    size_t index;
    if (length != TEST_PAGE_SIZE || offset % TEST_PAGE_SIZE != 0U ||
        offset > TEST_REGION_SIZE || length > TEST_REGION_SIZE - offset) {
        return false;
    }
    ++flash->erase_count;
    for (index = 0U; index < length; ++index) {
        if (!consume_byte(flash)) {
            return false;
        }
        flash->bytes[offset + index] = UINT8_MAX;
    }
    return true;
}

static bool memory_program(void* context, uint32_t offset,
                           const uint8_t* data, size_t length) {
    memory_flash* flash = context;
    size_t index;
    bool completed = true;
    if (data == NULL || offset % flash->program_size != 0U ||
        length % flash->program_size != 0U || offset > TEST_REGION_SIZE ||
        length > TEST_REGION_SIZE - offset) {
        return false;
    }
    ++flash->program_calls;
    for (index = 0U; index < length; ++index) {
        if (!consume_byte(flash)) {
            completed = false;
            break;
        }
        if ((flash->bytes[offset + index] & data[index]) != data[index]) {
            return false;
        }
        flash->bytes[offset + index] &= data[index];
    }
    if (completed && flash->fail_after_program_call == flash->program_calls) {
        flash->fail_after_program_call = 0U;
        return false;
    }
    return completed;
}

static void reset_flash(memory_flash* flash, uint32_t program_size) {
    memset(flash, 0, sizeof(*flash));
    memset(flash->bytes, UINT8_MAX, sizeof(flash->bytes));
    flash->program_size = program_size;
}

static rbsp_device_param_backend make_backend(memory_flash* flash) {
    rbsp_device_param_backend backend;
    memset(&backend, 0, sizeof(backend));
    backend.context = flash;
    backend.region_size = TEST_REGION_SIZE;
    backend.erase_size = TEST_PAGE_SIZE;
    backend.program_size = flash->program_size;
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

static uint32_t crc32_update(uint32_t crc, const uint8_t* data,
                             size_t length) {
    size_t index;
    for (index = 0U; index < length; ++index) {
        uint8_t bit;
        crc ^= data[index];
        for (bit = 0U; bit < 8U; ++bit) {
            const uint32_t mask = (uint32_t)-(int32_t)(crc & 1U);
            crc = (crc >> 1U) ^ (UINT32_C(0xEDB88320) & mask);
        }
    }
    return crc;
}

static void put_u32(uint8_t* data, uint32_t value) {
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8U);
    data[2] = (uint8_t)(value >> 16U);
    data[3] = (uint8_t)(value >> 24U);
}

static void put_u64(uint8_t* data, uint64_t value) {
    uint8_t index;
    for (index = 0U; index < 8U; ++index) {
        data[index] = (uint8_t)(value >> (index * 8U));
    }
}

static void encode_record(uint8_t record[32U], uint64_t epoch) {
    uint32_t crc;
    memset(record, UINT8_MAX, 32U);
    memcpy(record, "RBEP", 4U);
    record[4U] = 1U;
    record[5U] = 0U;
    record[6U] = 32U;
    record[7U] = 0U;
    put_u64(record + 8U, epoch);
    put_u64(record + 16U, ~epoch);
    crc = ~crc32_update(UINT32_MAX, record, 24U);
    put_u32(record + 24U, crc);
    memset(record + 28U, 0, 4U);
}

static uint64_t advance_once(memory_flash* flash) {
    rbsp_boot_epoch_journal journal;
    rbsp_device_param_backend backend = make_backend(flash);
    uint64_t epoch = 0U;
    assert(rbsp_boot_epoch_journal_init(&journal, &backend));
    assert(rbsp_boot_epoch_journal_advance(&journal, &epoch));
    assert(epoch != 0U && journal.last_epoch == epoch);
    return epoch;
}

static void test_monotonic_rotation(uint32_t program_size) {
    memory_flash flash;
    uint64_t previous = 0U;
    uint32_t boot;
    reset_flash(&flash, program_size);
    for (boot = 0U; boot < TEST_SLOT_COUNT + 6U; ++boot) {
        const uint64_t epoch = advance_once(&flash);
        assert(epoch == previous + 1U);
        previous = epoch;
    }
    assert(flash.erase_count == 2U);
}

static void test_rapid_reboots(uint32_t program_size) {
    memory_flash flash;
    uint32_t boot;
    reset_flash(&flash, program_size);
    for (boot = 1U; boot <= 512U; ++boot) {
        assert(advance_once(&flash) == boot);
    }
    assert(flash.erase_count == 15U);
}

static void test_program_interruption(uint32_t program_size) {
    size_t cutoff;
    for (cutoff = 0U;
         cutoff < RBSP_BOOT_EPOCH_JOURNAL_RECORD_SIZE; ++cutoff) {
        memory_flash flash;
        rbsp_boot_epoch_journal journal;
        rbsp_device_param_backend backend;
        uint64_t epoch = UINT64_MAX;
        reset_flash(&flash, program_size);
        flash.limit_operations = true;
        flash.remaining_bytes = cutoff;
        backend = make_backend(&flash);
        assert(rbsp_boot_epoch_journal_init(&journal, &backend));
        assert(!rbsp_boot_epoch_journal_advance(&journal, &epoch));
        assert(epoch == 0U);
        flash.limit_operations = false;
        if (cutoff >= 28U) {
            assert(!rbsp_boot_epoch_journal_advance(&journal, &epoch));
            assert(journal.last_error ==
                   RBSP_BOOT_EPOCH_JOURNAL_ERROR_CORRUPT);
        } else {
            assert(advance_once(&flash) == 1U);
        }
    }
}

static void test_rotation_interruption(uint32_t program_size) {
    memory_flash base;
    size_t cutoff;
    uint32_t boot;
    uint32_t recovered = 0U;
    uint32_t failed_closed = 0U;
    reset_flash(&base, program_size);
    for (boot = 1U; boot <= TEST_SLOT_COUNT; ++boot) {
        assert(advance_once(&base) == boot);
    }
    for (cutoff = 0U;
         cutoff < TEST_PAGE_SIZE + RBSP_BOOT_EPOCH_JOURNAL_RECORD_SIZE;
         ++cutoff) {
        memory_flash flash = base;
        rbsp_boot_epoch_journal journal;
        rbsp_device_param_backend backend;
        uint64_t epoch = UINT64_MAX;
        flash.limit_operations = true;
        flash.remaining_bytes = cutoff;
        backend = make_backend(&flash);
        assert(rbsp_boot_epoch_journal_init(&journal, &backend));
        assert(!rbsp_boot_epoch_journal_advance(&journal, &epoch));
        assert(epoch == 0U);
        flash.limit_operations = false;
        if (rbsp_boot_epoch_journal_advance(&journal, &epoch)) {
            assert(epoch > TEST_SLOT_COUNT);
            ++recovered;
        } else {
            assert(epoch == 0U);
            assert(journal.last_error ==
                   RBSP_BOOT_EPOCH_JOURNAL_ERROR_CORRUPT);
            assert(!rbsp_boot_epoch_journal_advance(&journal, &epoch));
            ++failed_closed;
        }
    }
    assert(recovered != 0U);
    assert(failed_closed != 0U);
}

static void test_corruption_consumes_epoch(void) {
    const size_t offsets[] = {0U, 16U, 24U, 28U};
    size_t index;
    for (index = 0U; index < sizeof(offsets) / sizeof(offsets[0]); ++index) {
        memory_flash flash;
        reset_flash(&flash, 8U);
        rbsp_boot_epoch_journal journal;
        rbsp_device_param_backend backend;
        uint64_t epoch = UINT64_MAX;
        assert(advance_once(&flash) == 1U);
        flash.bytes[offsets[index]] ^= 0x01U;
        backend = make_backend(&flash);
        assert(rbsp_boot_epoch_journal_init(&journal, &backend));
        assert(!rbsp_boot_epoch_journal_advance(&journal, &epoch));
        assert(epoch == 0U);
        assert(journal.last_error == RBSP_BOOT_EPOCH_JOURNAL_ERROR_CORRUPT);
    }
}

static void test_torn_slot_is_absorbed_without_epoch_reuse(void) {
    memory_flash flash;
    rbsp_boot_epoch_journal journal;
    rbsp_device_param_backend backend;
    uint64_t epoch = UINT64_MAX;
    reset_flash(&flash, 8U);
    flash.limit_operations = true;
    flash.remaining_bytes = 1U;
    backend = make_backend(&flash);
    assert(rbsp_boot_epoch_journal_init(&journal, &backend));
    assert(!rbsp_boot_epoch_journal_advance(&journal, &epoch));
    flash.limit_operations = false;
    assert(advance_once(&flash) == 1U);
    assert(advance_once(&flash) == 2U);
    assert(advance_once(&flash) == 3U);

    flash.bytes[2U * RBSP_BOOT_EPOCH_JOURNAL_RECORD_SIZE + 24U] ^= 0x01U;
    flash.bytes[3U * RBSP_BOOT_EPOCH_JOURNAL_RECORD_SIZE + 24U] ^= 0x01U;
    assert(rbsp_boot_epoch_journal_init(&journal, &backend));
    assert(!rbsp_boot_epoch_journal_advance(&journal, &epoch));
    assert(epoch == 0U);
    assert(journal.last_error == RBSP_BOOT_EPOCH_JOURNAL_ERROR_CORRUPT);
}

static void test_uncertain_program_and_duplicate(void) {
    memory_flash flash;
    rbsp_boot_epoch_journal journal;
    rbsp_device_param_backend backend;
    uint64_t epoch = UINT64_MAX;
    reset_flash(&flash, 8U);
    flash.fail_after_program_call = 2U;
    backend = make_backend(&flash);
    assert(rbsp_boot_epoch_journal_init(&journal, &backend));
    journal.last_epoch = UINT64_MAX;
    assert(!rbsp_boot_epoch_journal_advance(&journal, &epoch));
    assert(epoch == 0U);
    assert(journal.last_epoch == 0U);
    assert(advance_once(&flash) == 2U);

    memcpy(flash.bytes + 2U * RBSP_BOOT_EPOCH_JOURNAL_RECORD_SIZE,
           flash.bytes + RBSP_BOOT_EPOCH_JOURNAL_RECORD_SIZE,
           RBSP_BOOT_EPOCH_JOURNAL_RECORD_SIZE);
    assert(rbsp_boot_epoch_journal_init(&journal, &backend));
    assert(!rbsp_boot_epoch_journal_advance(&journal, &epoch));
    assert(journal.last_error == RBSP_BOOT_EPOCH_JOURNAL_ERROR_CORRUPT);
}

static void test_exhaustion_and_backend_validation(void) {
    memory_flash flash;
    rbsp_boot_epoch_journal journal;
    rbsp_device_param_backend backend;
    uint64_t epoch = UINT64_MAX;
    reset_flash(&flash, 8U);
    encode_record(flash.bytes, UINT64_MAX);
    backend = make_backend(&flash);
    assert(rbsp_boot_epoch_journal_init(&journal, &backend));
    assert(!rbsp_boot_epoch_journal_advance(&journal, &epoch));
    assert(epoch == 0U);
    assert(journal.last_error == RBSP_BOOT_EPOCH_JOURNAL_ERROR_EXHAUSTED);

    backend.region_size -= 8U;
    assert(!rbsp_boot_epoch_journal_init(&journal, &backend));
    assert(journal.last_error ==
           RBSP_BOOT_EPOCH_JOURNAL_ERROR_INVALID_BACKEND);
}

int main(void) {
    test_monotonic_rotation(2U);
    test_monotonic_rotation(8U);
    test_rapid_reboots(2U);
    test_rapid_reboots(8U);
    test_program_interruption(2U);
    test_program_interruption(8U);
    test_rotation_interruption(2U);
    test_rotation_interruption(8U);
    test_corruption_consumes_epoch();
    test_torn_slot_is_absorbed_without_epoch_reuse();
    test_uncertain_program_and_duplicate();
    test_exhaustion_and_backend_validation();
    puts("启动代次Flash日志故障注入测试通过");
    return 0;
}
