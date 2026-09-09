#include "remotebsp/device_params/boot_epoch.h"

#include <limits.h>
#include <string.h>

#define RBSP_BOOT_EPOCH_MAGIC_0 ((uint8_t)'R')
#define RBSP_BOOT_EPOCH_MAGIC_1 ((uint8_t)'B')
#define RBSP_BOOT_EPOCH_MAGIC_2 ((uint8_t)'E')
#define RBSP_BOOT_EPOCH_MAGIC_3 ((uint8_t)'P')
#define RBSP_BOOT_EPOCH_FORMAT_VERSION 1U
#define RBSP_BOOT_EPOCH_BODY_SIZE 24U
#define RBSP_BOOT_EPOCH_FINAL_SIZE 8U

static const uint8_t commit_marker[4] = {
    0x00U, 0x00U, 0x00U, 0x00U,
};

typedef struct {
    bool found;
    bool duplicate;
    bool ambiguous_committed;
    uint64_t maximum_epoch;
    uint8_t maximum_page;
    uint16_t maximum_slot;
    uint16_t first_erased[RBSP_BOOT_EPOCH_JOURNAL_PAGE_COUNT];
    bool page_has_valid[RBSP_BOOT_EPOCH_JOURNAL_PAGE_COUNT];
} journal_scan;

static uint16_t read_u16(const uint8_t* data) {
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8U);
}

static uint32_t read_u32(const uint8_t* data) {
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8U) |
           ((uint32_t)data[2] << 16U) | ((uint32_t)data[3] << 24U);
}

static uint64_t read_u64(const uint8_t* data) {
    uint64_t value = 0U;
    uint8_t index;
    for (index = 0U; index < 8U; ++index) {
        value |= (uint64_t)data[index] << (index * 8U);
    }
    return value;
}

static void write_u16(uint8_t* data, uint16_t value) {
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8U);
}

static void write_u32(uint8_t* data, uint32_t value) {
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8U);
    data[2] = (uint8_t)(value >> 16U);
    data[3] = (uint8_t)(value >> 24U);
}

static void write_u64(uint8_t* data, uint64_t value) {
    uint8_t index;
    for (index = 0U; index < 8U; ++index) {
        data[index] = (uint8_t)(value >> (index * 8U));
    }
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

static uint32_t record_crc(const uint8_t* record) {
    return ~crc32_update(UINT32_MAX, record, RBSP_BOOT_EPOCH_BODY_SIZE);
}

static bool bytes_erased(const uint8_t* data, size_t length) {
    size_t index;
    for (index = 0U; index < length; ++index) {
        if (data[index] != UINT8_MAX) {
            return false;
        }
    }
    return true;
}

static bool record_payload_valid(const uint8_t* record, uint64_t* epoch) {
    const uint64_t value = read_u64(record + 8U);
    if (record[0] != RBSP_BOOT_EPOCH_MAGIC_0 ||
        record[1] != RBSP_BOOT_EPOCH_MAGIC_1 ||
        record[2] != RBSP_BOOT_EPOCH_MAGIC_2 ||
        record[3] != RBSP_BOOT_EPOCH_MAGIC_3 ||
        read_u16(record + 4U) != RBSP_BOOT_EPOCH_FORMAT_VERSION ||
        read_u16(record + 6U) != RBSP_BOOT_EPOCH_JOURNAL_RECORD_SIZE ||
        value == 0U || read_u64(record + 16U) != ~value ||
        read_u32(record + 24U) != record_crc(record)) {
        return false;
    }
    if (epoch != NULL) {
        *epoch = value;
    }
    return true;
}

static bool record_has_commit_marker(const uint8_t* record) {
    return memcmp(record + 28U, commit_marker, sizeof(commit_marker)) == 0;
}

static bool record_valid(const uint8_t* record, uint64_t* epoch) {
    return record_payload_valid(record, epoch) &&
           record_has_commit_marker(record);
}

static bool scan_journal(const rbsp_boot_epoch_journal* journal,
                         journal_scan* scan) {
    const uint16_t slots = (uint16_t)(
        journal->page_size / RBSP_BOOT_EPOCH_JOURNAL_RECORD_SIZE);
    uint8_t page;
    memset(scan, 0, sizeof(*scan));
    for (page = 0U; page < RBSP_BOOT_EPOCH_JOURNAL_PAGE_COUNT; ++page) {
        const uint8_t* image = journal->backend.map(
            journal->backend.context, (uint32_t)page * journal->page_size,
            journal->page_size);
        uint16_t slot;
        scan->first_erased[page] = UINT16_MAX;
        if (image == NULL) {
            return false;
        }
        for (slot = 0U; slot < slots; ++slot) {
            const uint8_t* record = image +
                (uint32_t)slot * RBSP_BOOT_EPOCH_JOURNAL_RECORD_SIZE;
            uint64_t epoch;
            if (bytes_erased(record, RBSP_BOOT_EPOCH_JOURNAL_RECORD_SIZE)) {
                if (scan->first_erased[page] == UINT16_MAX) {
                    scan->first_erased[page] = slot;
                }
                continue;
            }
            if (!record_valid(record, &epoch)) {
                continue;
            }
            scan->page_has_valid[page] = true;
            if (scan->found && epoch == scan->maximum_epoch) {
                scan->duplicate = true;
            }
            if (!scan->found || epoch > scan->maximum_epoch) {
                scan->found = true;
                scan->maximum_epoch = epoch;
                scan->maximum_page = page;
                scan->maximum_slot = slot;
                scan->duplicate = false;
            }
        }
    }
    for (page = 0U; page < RBSP_BOOT_EPOCH_JOURNAL_PAGE_COUNT; ++page) {
        const uint8_t* image = journal->backend.map(
            journal->backend.context, (uint32_t)page * journal->page_size,
            journal->page_size);
        uint16_t slot;
        if (image == NULL) {
            return false;
        }
        for (slot = 0U; slot < slots; ++slot) {
            const uint8_t* record = image +
                (uint32_t)slot * RBSP_BOOT_EPOCH_JOURNAL_RECORD_SIZE;
            uint64_t ignored_epoch;
            bool known_older = false;
            if (bytes_erased(record, RBSP_BOOT_EPOCH_JOURNAL_RECORD_SIZE) ||
                record_valid(record, &ignored_epoch) ||
                (!record_has_commit_marker(record) &&
                 !record_payload_valid(record, &ignored_epoch))) {
                continue;
            }
            if (scan->found && page == scan->maximum_page &&
                slot < scan->maximum_slot) {
                /* 同页较早槽已经被最大有效记录吸收。 */
                known_older = true;
            } else if (scan->found && page != scan->maximum_page &&
                       (scan->first_erased[scan->maximum_page] != UINT16_MAX ||
                        scan->page_has_valid[page])) {
                /*
                 * 当前页尚可追加，或另一页仍有旧有效记录时，另一页只可能
                 * 来自上一轮。无法证明此前后关系时宁可永久 fail-closed。
                 */
                known_older = true;
            }
            if (!known_older) {
                scan->ambiguous_committed = true;
            }
        }
    }
    return true;
}

bool rbsp_boot_epoch_journal_init(
    rbsp_boot_epoch_journal* journal,
    const rbsp_device_param_backend* backend) {
    if (journal == NULL || backend == NULL || backend->map == NULL ||
        backend->erase == NULL || backend->program == NULL ||
        backend->erase_size < RBSP_BOOT_EPOCH_JOURNAL_RECORD_SIZE ||
        backend->erase_size > UINT32_MAX /
            RBSP_BOOT_EPOCH_JOURNAL_PAGE_COUNT ||
        backend->erase_size / RBSP_BOOT_EPOCH_JOURNAL_RECORD_SIZE >
            UINT16_MAX ||
        backend->program_size == 0U || backend->program_size > 8U ||
        (backend->program_size & (backend->program_size - 1U)) != 0U ||
        backend->region_size !=
            backend->erase_size * RBSP_BOOT_EPOCH_JOURNAL_PAGE_COUNT ||
        (backend->erase_size % RBSP_BOOT_EPOCH_JOURNAL_RECORD_SIZE) != 0U ||
        (RBSP_BOOT_EPOCH_BODY_SIZE % backend->program_size) != 0U ||
        (RBSP_BOOT_EPOCH_FINAL_SIZE % backend->program_size) != 0U) {
        if (journal != NULL) {
            memset(journal, 0, sizeof(*journal));
            journal->last_error =
                RBSP_BOOT_EPOCH_JOURNAL_ERROR_INVALID_BACKEND;
        }
        return false;
    }
    memset(journal, 0, sizeof(*journal));
    journal->backend = *backend;
    journal->page_size = backend->erase_size;
    return true;
}

bool rbsp_boot_epoch_journal_advance(
    rbsp_boot_epoch_journal* journal, uint64_t* output_epoch) {
    journal_scan scan;
    uint64_t next_epoch;
    uint8_t target_page;
    uint16_t target_slot;
    uint32_t target_offset;
    uint8_t record[RBSP_BOOT_EPOCH_JOURNAL_RECORD_SIZE];
    const uint8_t* written;
    if (output_epoch != NULL) {
        *output_epoch = 0U;
    }
    if (journal == NULL || output_epoch == NULL ||
        journal->backend.map == NULL) {
        if (journal != NULL) {
            journal->last_error =
                RBSP_BOOT_EPOCH_JOURNAL_ERROR_INVALID_BACKEND;
        }
        return false;
    }
    journal->last_epoch = 0U;
    if (!scan_journal(journal, &scan)) {
        journal->last_error = RBSP_BOOT_EPOCH_JOURNAL_ERROR_IO;
        return false;
    }
    if (scan.duplicate || scan.ambiguous_committed) {
        journal->last_error = RBSP_BOOT_EPOCH_JOURNAL_ERROR_CORRUPT;
        return false;
    }
    if (scan.maximum_epoch == UINT64_MAX) {
        journal->last_error = RBSP_BOOT_EPOCH_JOURNAL_ERROR_EXHAUSTED;
        return false;
    }
    next_epoch = scan.maximum_epoch + 1U;
    target_page = scan.found ? scan.maximum_page : 0U;
    target_slot = scan.first_erased[target_page];
    if (target_slot == UINT16_MAX) {
        target_page = (uint8_t)(target_page ^ 1U);
        if (!journal->backend.erase(
                journal->backend.context,
                (uint32_t)target_page * journal->page_size,
                journal->page_size)) {
            journal->last_error = RBSP_BOOT_EPOCH_JOURNAL_ERROR_IO;
            return false;
        }
        target_slot = 0U;
    }
    target_offset = (uint32_t)target_page * journal->page_size +
                    (uint32_t)target_slot *
                        RBSP_BOOT_EPOCH_JOURNAL_RECORD_SIZE;
    memset(record, UINT8_MAX, sizeof(record));
    record[0] = RBSP_BOOT_EPOCH_MAGIC_0;
    record[1] = RBSP_BOOT_EPOCH_MAGIC_1;
    record[2] = RBSP_BOOT_EPOCH_MAGIC_2;
    record[3] = RBSP_BOOT_EPOCH_MAGIC_3;
    write_u16(record + 4U, RBSP_BOOT_EPOCH_FORMAT_VERSION);
    write_u16(record + 6U, RBSP_BOOT_EPOCH_JOURNAL_RECORD_SIZE);
    write_u64(record + 8U, next_epoch);
    write_u64(record + 16U, ~next_epoch);
    write_u32(record + 24U, record_crc(record));
    memcpy(record + 28U, commit_marker, sizeof(commit_marker));
    if (!journal->backend.program(
            journal->backend.context, target_offset, record,
            RBSP_BOOT_EPOCH_BODY_SIZE) ||
        !journal->backend.program(
            journal->backend.context,
            target_offset + RBSP_BOOT_EPOCH_BODY_SIZE,
            record + RBSP_BOOT_EPOCH_BODY_SIZE,
            RBSP_BOOT_EPOCH_FINAL_SIZE)) {
        journal->last_error = RBSP_BOOT_EPOCH_JOURNAL_ERROR_IO;
        return false;
    }
    written = journal->backend.map(
        journal->backend.context, target_offset,
        RBSP_BOOT_EPOCH_JOURNAL_RECORD_SIZE);
    if (written == NULL || !record_valid(written, &journal->last_epoch) ||
        journal->last_epoch != next_epoch) {
        journal->last_epoch = 0U;
        journal->last_error = RBSP_BOOT_EPOCH_JOURNAL_ERROR_VERIFY;
        return false;
    }
    journal->last_error = RBSP_BOOT_EPOCH_JOURNAL_OK;
    *output_epoch = next_epoch;
    return true;
}
