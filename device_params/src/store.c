#include "remotebsp/device_params/store.h"

#include <string.h>

#define RBSP_PARAM_MAGIC_0 ((uint8_t)'R')
#define RBSP_PARAM_MAGIC_1 ((uint8_t)'B')
#define RBSP_PARAM_MAGIC_2 ((uint8_t)'N')
#define RBSP_PARAM_MAGIC_3 ((uint8_t)'V')
#define RBSP_PARAM_FORMAT_VERSION 1U
#define RBSP_PARAM_RECORD_HEADER_SIZE 8U

static const uint8_t commit_marker[8] = {
    0x52U, 0x42U, 0x4EU, 0x56U, 0x43U, 0x4FU, 0x4DU, 0x31U,
};

static uint16_t read_u16(const uint8_t* data) {
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8U);
}

static uint32_t read_u32(const uint8_t* data) {
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8U) |
           ((uint32_t)data[2] << 16U) | ((uint32_t)data[3] << 24U);
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

static uint32_t crc32_update(uint32_t crc, const uint8_t* data,
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

static uint32_t image_crc(const uint8_t* image, size_t length) {
    uint32_t crc = UINT32_MAX;
    crc = crc32_update(crc, image, 16U);
    if (length > RBSP_DEVICE_PARAM_STORE_HEADER_SIZE) {
        crc = crc32_update(crc,
                           image + RBSP_DEVICE_PARAM_STORE_HEADER_SIZE,
                           length - RBSP_DEVICE_PARAM_STORE_HEADER_SIZE);
    }
    return ~crc;
}

static size_t aligned_size(size_t value, size_t alignment) {
    return (value + alignment - 1U) & ~(alignment - 1U);
}

static bool generation_newer(uint32_t left, uint32_t right) {
    return (int32_t)(left - right) > 0;
}

static const uint8_t* page_map(const rbsp_device_param_store* store,
                               uint8_t page) {
    if (page >= RBSP_DEVICE_PARAM_STORE_PAGE_COUNT) {
        return NULL;
    }
    return store->backend.map(
        store->backend.context, (uint32_t)page * store->page_size,
        store->page_size);
}

static bool parse_record(const uint8_t* image, uint16_t image_length,
                         size_t target_index, uint16_t target_id,
                         bool by_id, rbsp_device_param_record* record) {
    size_t offset = RBSP_DEVICE_PARAM_STORE_HEADER_SIZE;
    size_t index = 0U;
    while (offset < image_length) {
        uint16_t length;
        size_t padded;
        if (image_length - offset < RBSP_PARAM_RECORD_HEADER_SIZE) {
            return false;
        }
        length = read_u16(image + offset + 4U);
        padded = aligned_size(RBSP_PARAM_RECORD_HEADER_SIZE + length, 4U);
        if (padded > image_length - offset ||
            length > RBSP_DEVICE_PARAM_MAX_VALUE_SIZE) {
            return false;
        }
        if ((by_id && read_u16(image + offset) == target_id) ||
            (!by_id && index == target_index)) {
            if (record != NULL) {
                record->id = read_u16(image + offset);
                record->type = image[offset + 2U];
                record->flags = image[offset + 3U];
                record->length = length;
                record->value = image + offset + RBSP_PARAM_RECORD_HEADER_SIZE;
            }
            return true;
        }
        offset += padded;
        ++index;
    }
    return false;
}

static bool page_valid(const rbsp_device_param_store* store, uint8_t page,
                       uint32_t* generation, uint16_t* length,
                       uint16_t* record_count) {
    const uint8_t* image = page_map(store, page);
    size_t index;
    if (image == NULL || image[0] != RBSP_PARAM_MAGIC_0 ||
        image[1] != RBSP_PARAM_MAGIC_1 || image[2] != RBSP_PARAM_MAGIC_2 ||
        image[3] != RBSP_PARAM_MAGIC_3 ||
        read_u16(image + 4U) != RBSP_PARAM_FORMAT_VERSION ||
        read_u16(image + 6U) != RBSP_DEVICE_PARAM_STORE_HEADER_SIZE ||
        memcmp(image + 24U, commit_marker, sizeof(commit_marker)) != 0) {
        return false;
    }
    *length = read_u16(image + 8U);
    *record_count = read_u16(image + 10U);
    *generation = read_u32(image + 12U);
    if (*length < RBSP_DEVICE_PARAM_STORE_HEADER_SIZE ||
        *length > store->page_size ||
        *length > RBSP_DEVICE_PARAM_STORE_MAX_IMAGE_SIZE ||
        *record_count > RBSP_DEVICE_PARAM_STORE_MAX_RECORDS ||
        image_crc(image, *length) != read_u32(image + 16U)) {
        return false;
    }
    for (index = 0U; index < *record_count; ++index) {
        rbsp_device_param_record record;
        rbsp_device_param_definition definition;
        size_t previous;
        if (!parse_record(image, *length, index, 0U, false, &record)) {
            return false;
        }
        /*
         * v1允许较新schema写入的未知记录。它们保持为不透明字节，下一次
         * 提交会原样复制；一旦本版本认识该ID，仍执行完整类型和值校验。
         */
        if (rbsp_device_param_find_definition(record.id, &definition) &&
            (definition.type != record.type ||
             definition.flags != record.flags ||
             !rbsp_device_param_validate_value(
                 record.id, record.value, record.length))) {
            return false;
        }
        for (previous = 0U; previous < index; ++previous) {
            rbsp_device_param_record other;
            if (!parse_record(image, *length, previous, 0U, false, &other) ||
                other.id == record.id) {
                return false;
            }
        }
    }
    return !parse_record(image, *length, *record_count, 0U, false, NULL);
}

bool rbsp_device_param_store_init(
    rbsp_device_param_store* store,
    const rbsp_device_param_backend* backend) {
    if (store == NULL || backend == NULL || backend->map == NULL ||
        backend->erase == NULL || backend->program == NULL ||
        backend->erase_size == 0U || backend->program_size == 0U ||
        (backend->program_size & (backend->program_size - 1U)) != 0U ||
        backend->region_size != backend->erase_size * 2U ||
        backend->erase_size < RBSP_DEVICE_PARAM_STORE_MAX_IMAGE_SIZE ||
        (24U % backend->program_size) != 0U ||
        (32U % backend->program_size) != 0U ||
        backend->contract_version != RBSP_DEVICE_PARAM_BACKEND_CONTRACT_VERSION ||
        backend->erased_value != RBSP_DEVICE_PARAM_BACKEND_ERASED_VALUE ||
        backend->medium < RBSP_DEVICE_PARAM_MEDIUM_INTERNAL_FLASH ||
        backend->medium > RBSP_DEVICE_PARAM_MEDIUM_MOCK ||
        (backend->capability_flags & RBSP_DEVICE_PARAM_BACKEND_FLAG_COMMIT_MARKER_LAST) == 0U ||
        (((backend->capability_flags & RBSP_DEVICE_PARAM_BACKEND_FLAG_ONE_TO_ZERO_ONLY) != 0U) ==
         ((backend->capability_flags & RBSP_DEVICE_PARAM_BACKEND_FLAG_BYTE_REWRITABLE) != 0U))) {
        return false;
    }
    memset(store, 0, sizeof(*store));
    store->backend = *backend;
    store->page_size = backend->erase_size;
    store->active_page = RBSP_DEVICE_PARAM_STORE_NO_PAGE;
    store->commit_budget = RBSP_DEVICE_PARAM_STORE_DEFAULT_COMMIT_BUDGET;
    return true;
}

bool rbsp_device_param_store_set_commit_budget(
    rbsp_device_param_store* store, uint32_t commit_budget) {
    if (store == NULL || commit_budget == 0U ||
        commit_budget < store->generation ||
        commit_budget > store->commit_budget) return false;
    store->commit_budget = commit_budget;
    return true;
}

bool rbsp_device_param_store_health_get(
    const rbsp_device_param_store* store,
    rbsp_device_param_store_health* health) {
    if (store == NULL || health == NULL) return false;
    health->persistent_commits = store->generation;
    health->commit_budget = store->commit_budget;
    health->write_attempts = store->write_attempts;
    health->successful_commits = store->successful_commits;
    health->io_failures = store->io_failures;
    health->bad_page_mask = store->bad_page_mask;
    return true;
}

bool rbsp_device_param_store_boot(rbsp_device_param_store* store) {
    uint8_t page;
    bool found = false;
    if (store == NULL) {
        return false;
    }
    store->active_page = RBSP_DEVICE_PARAM_STORE_NO_PAGE;
    store->generation = 0U;
    store->image_length = 0U;
    store->record_count = 0U;
    for (page = 0U; page < RBSP_DEVICE_PARAM_STORE_PAGE_COUNT; ++page) {
        uint32_t generation;
        uint16_t length;
        uint16_t record_count;
        if (!page_valid(store, page, &generation, &length, &record_count)) {
            continue;
        }
        if (!found || generation_newer(generation, store->generation)) {
            store->active_page = page;
            store->generation = generation;
            store->image_length = length;
            store->record_count = record_count;
            found = true;
        }
    }
    store->last_error = RBSP_DEVICE_PARAM_STORE_OK;
    return true;
}

bool rbsp_device_param_store_get(
    const rbsp_device_param_store* store, uint16_t id,
    rbsp_device_param_record* record) {
    const uint8_t* image;
    if (store == NULL || store->active_page == RBSP_DEVICE_PARAM_STORE_NO_PAGE) {
        return false;
    }
    image = page_map(store, store->active_page);
    return image != NULL && parse_record(
        image, store->image_length, 0U, id, true, record);
}

bool rbsp_device_param_store_resolve(
    const rbsp_device_param_store* store, uint16_t id,
    rbsp_device_param_record* record,
    rbsp_device_param_value_source* source) {
    rbsp_device_param_definition definition;
    static const uint8_t empty_value = 0U;
    if (record == NULL || source == NULL ||
        !rbsp_device_param_find_definition(id, &definition)) {
        return false;
    }
    if (rbsp_device_param_store_get(store, id, record)) {
        *source = RBSP_DEVICE_PARAM_VALUE_PERSISTED;
        return true;
    }
    record->id = id;
    record->type = definition.type;
    record->flags = definition.flags;
    record->length = 0U;
    record->value = &empty_value;
    if (definition.minimum_length == 0U) {
        *source = RBSP_DEVICE_PARAM_VALUE_DEFAULTED;
    } else {
        record->value = NULL;
        *source = RBSP_DEVICE_PARAM_VALUE_ABSENT;
    }
    return true;
}

bool rbsp_device_param_store_record_at(
    const rbsp_device_param_store* store, size_t index,
    rbsp_device_param_record* record) {
    const uint8_t* image;
    if (store == NULL || index >= store->record_count ||
        store->active_page == RBSP_DEVICE_PARAM_STORE_NO_PAGE) {
        return false;
    }
    image = page_map(store, store->active_page);
    return image != NULL && parse_record(
        image, store->image_length, index, 0U, false, record);
}

static bool append_record(uint8_t* image, size_t capacity, size_t* offset,
                          uint16_t id, uint8_t type, uint8_t flags,
                          const uint8_t* value, size_t length) {
    const size_t size = aligned_size(
        RBSP_PARAM_RECORD_HEADER_SIZE + length, 4U);
    if (size > capacity - *offset) {
        return false;
    }
    write_u16(image + *offset, id);
    image[*offset + 2U] = type;
    image[*offset + 3U] = flags;
    write_u16(image + *offset + 4U, (uint16_t)length);
    write_u16(image + *offset + 6U, 0U);
    if (length > 0U) {
        memcpy(image + *offset + RBSP_PARAM_RECORD_HEADER_SIZE,
               value, length);
    }
    *offset += size;
    return true;
}

static bool append_opaque_record(uint8_t* image, size_t capacity,
                                 size_t* offset,
                                 const rbsp_device_param_record* record) {
    const size_t size = aligned_size(
        RBSP_PARAM_RECORD_HEADER_SIZE + record->length, 4U);
    const uint8_t* raw = record->value - RBSP_PARAM_RECORD_HEADER_SIZE;
    if (size > capacity - *offset) {
        return false;
    }
    memcpy(image + *offset, raw, size);
    *offset += size;
    return true;
}

bool rbsp_device_param_store_set(
    rbsp_device_param_store* store, uint32_t expected_generation,
    uint16_t id, const uint8_t* value, size_t length,
    uint8_t* workspace, size_t workspace_size) {
    rbsp_device_param_definition definition;
    rbsp_device_param_record existing;
    const bool exists = rbsp_device_param_store_get(store, id, &existing);
    const uint8_t target_page =
        store != NULL && store->active_page == 0U ? 1U : 0U;
    size_t offset = RBSP_DEVICE_PARAM_STORE_HEADER_SIZE;
    size_t index;
    uint16_t record_count = 0U;
    size_t programmed_length;
    if (store == NULL || workspace == NULL ||
        workspace_size < RBSP_DEVICE_PARAM_STORE_MAX_IMAGE_SIZE ||
        expected_generation != store->generation) {
        if (store != NULL) {
            store->last_error = RBSP_DEVICE_PARAM_STORE_ERROR_STALE_GENERATION;
        }
        return false;
    }
    if (!rbsp_device_param_find_definition(id, &definition) ||
        !rbsp_device_param_validate_value(id, value, length)) {
        store->last_error = RBSP_DEVICE_PARAM_STORE_ERROR_INVALID_VALUE;
        return false;
    }
    if (exists &&
        (definition.flags & RBSP_DEVICE_PARAM_FLAG_WRITE_ONCE) != 0U) {
        store->last_error = RBSP_DEVICE_PARAM_STORE_ERROR_WRITE_ONCE;
        return false;
    }
    if (store->generation >= store->commit_budget) {
        store->last_error = RBSP_DEVICE_PARAM_STORE_ERROR_BUDGET_EXHAUSTED;
        return false;
    }
    if ((store->bad_page_mask & (uint8_t)(1U << target_page)) != 0U) {
        store->last_error = RBSP_DEVICE_PARAM_STORE_ERROR_BAD_PAGE;
        return false;
    }
    memset(workspace, 0xFF, RBSP_DEVICE_PARAM_STORE_MAX_IMAGE_SIZE);
    workspace[0] = RBSP_PARAM_MAGIC_0;
    workspace[1] = RBSP_PARAM_MAGIC_1;
    workspace[2] = RBSP_PARAM_MAGIC_2;
    workspace[3] = RBSP_PARAM_MAGIC_3;
    write_u16(workspace + 4U, RBSP_PARAM_FORMAT_VERSION);
    write_u16(workspace + 6U, RBSP_DEVICE_PARAM_STORE_HEADER_SIZE);
    write_u32(workspace + 12U, store->generation + 1U);

    for (index = 0U; index < store->record_count; ++index) {
        rbsp_device_param_record record;
        rbsp_device_param_definition stored_definition;
        if (!rbsp_device_param_store_record_at(store, index, &record)) {
            store->last_error = RBSP_DEVICE_PARAM_STORE_ERROR_CORRUPT;
            return false;
        }
        if (record.id == id) {
            continue;
        }
        if ((!rbsp_device_param_find_definition(
                 record.id, &stored_definition) &&
             !append_opaque_record(
                 workspace, RBSP_DEVICE_PARAM_STORE_MAX_IMAGE_SIZE,
                 &offset, &record)) ||
            (rbsp_device_param_find_definition(
                 record.id, &stored_definition) &&
             !append_record(workspace,
                            RBSP_DEVICE_PARAM_STORE_MAX_IMAGE_SIZE,
                            &offset, record.id, record.type, record.flags,
                            record.value, record.length))) {
            store->last_error = RBSP_DEVICE_PARAM_STORE_ERROR_NO_SPACE;
            return false;
        }
        ++record_count;
    }
    if (!append_record(workspace, RBSP_DEVICE_PARAM_STORE_MAX_IMAGE_SIZE,
                       &offset, id, definition.type, definition.flags,
                       value, length)) {
        store->last_error = RBSP_DEVICE_PARAM_STORE_ERROR_NO_SPACE;
        return false;
    }
    ++record_count;
    if (record_count > RBSP_DEVICE_PARAM_STORE_MAX_RECORDS ||
        offset > store->page_size) {
        store->last_error = RBSP_DEVICE_PARAM_STORE_ERROR_NO_SPACE;
        return false;
    }
    write_u16(workspace + 8U, (uint16_t)offset);
    write_u16(workspace + 10U, record_count);
    write_u32(workspace + 16U, image_crc(workspace, offset));

    ++store->write_attempts;
    if (!store->backend.erase(store->backend.context,
                              (uint32_t)target_page * store->page_size,
                              store->page_size)) {
        ++store->io_failures;
        store->bad_page_mask |= (uint8_t)(1U << target_page);
        store->last_error = RBSP_DEVICE_PARAM_STORE_ERROR_IO;
        return false;
    }
    if (!store->backend.program(store->backend.context,
                                (uint32_t)target_page * store->page_size,
                                workspace, 24U)) {
        ++store->io_failures;
        store->bad_page_mask |= (uint8_t)(1U << target_page);
        store->last_error = RBSP_DEVICE_PARAM_STORE_ERROR_IO;
        return false;
    }
    programmed_length = aligned_size(
        offset - RBSP_DEVICE_PARAM_STORE_HEADER_SIZE,
        store->backend.program_size);
    if (programmed_length > 0U &&
        !store->backend.program(
            store->backend.context,
            (uint32_t)target_page * store->page_size +
                RBSP_DEVICE_PARAM_STORE_HEADER_SIZE,
            workspace + RBSP_DEVICE_PARAM_STORE_HEADER_SIZE,
            programmed_length)) {
        ++store->io_failures;
        store->bad_page_mask |= (uint8_t)(1U << target_page);
        store->last_error = RBSP_DEVICE_PARAM_STORE_ERROR_IO;
        return false;
    }
    if (!store->backend.program(
            store->backend.context,
            (uint32_t)target_page * store->page_size + 24U,
            commit_marker, sizeof(commit_marker))) {
        ++store->io_failures;
        store->bad_page_mask |= (uint8_t)(1U << target_page);
        store->last_error = RBSP_DEVICE_PARAM_STORE_ERROR_IO;
        return false;
    }
    if (!rbsp_device_param_store_boot(store) ||
        store->active_page != target_page) {
        ++store->io_failures;
        store->bad_page_mask |= (uint8_t)(1U << target_page);
        store->last_error = RBSP_DEVICE_PARAM_STORE_ERROR_CORRUPT;
        return false;
    }
    ++store->successful_commits;
    return true;
}
