#include "remotebsp_embedded/device_parameter_eeprom.h"

#include <string.h>

static bool range_valid(const rbsp_device_parameter_eeprom_adapter* adapter,
                        uint32_t offset, size_t length) {
    return offset <= adapter->config.region_size &&
           length <= (size_t)(adapter->config.region_size - offset);
}

static bool synchronize(rbsp_device_parameter_eeprom_adapter* adapter) {
    adapter->synchronized = adapter->config.read(
        adapter->config.io_context, 0U, adapter->config.mirror,
        adapter->config.region_size);
    if (!adapter->synchronized) {
        memset(adapter->config.mirror, 0, adapter->config.region_size);
    }
    return adapter->synchronized;
}

static const uint8_t* map_bytes(void* context, uint32_t offset, size_t length) {
    rbsp_device_parameter_eeprom_adapter* adapter = context;
    if (adapter == NULL || !adapter->synchronized ||
        !range_valid(adapter, offset, length)) {
        return NULL;
    }
    return adapter->config.mirror + offset;
}

static bool fill_bytes(void* context, uint32_t offset, size_t length) {
    rbsp_device_parameter_eeprom_adapter* adapter = context;
    if (adapter == NULL || !range_valid(adapter, offset, length) ||
        !adapter->config.erase_or_fill(adapter->config.io_context, offset,
                                       length, 0xFFU)) {
        if (adapter != NULL) (void)synchronize(adapter);
        return false;
    }
    if (!synchronize(adapter)) return false;
    for (size_t i = 0; i < length; ++i) {
        if (adapter->config.mirror[offset + i] != 0xFFU) return false;
    }
    return true;
}

static bool program_bytes(void* context, uint32_t offset,
                          const uint8_t* data, size_t length) {
    rbsp_device_parameter_eeprom_adapter* adapter = context;
    if (adapter == NULL || data == NULL || !range_valid(adapter, offset, length) ||
        !adapter->config.write(adapter->config.io_context, offset, data, length)) {
        if (adapter != NULL) (void)synchronize(adapter);
        return false;
    }
    if (!synchronize(adapter)) return false;
    return memcmp(adapter->config.mirror + offset, data, length) == 0;
}

bool rbsp_device_parameter_eeprom_backend_init(
    rbsp_device_parameter_eeprom_adapter* adapter,
    rbsp_device_param_backend* backend,
    const rbsp_device_parameter_eeprom_config* config) {
    if (adapter == NULL || backend == NULL || config == NULL ||
        config->read == NULL || config->write == NULL ||
        config->erase_or_fill == NULL || config->mirror == NULL ||
        config->page_size < RBSP_DEVICE_PARAM_STORE_MAX_IMAGE_SIZE ||
        config->region_size != config->page_size * 2U ||
        config->mirror_size != config->region_size ||
        config->program_size == 0U || config->program_size > 8U ||
        (config->program_size & (config->program_size - 1U)) != 0U) {
        return false;
    }
    memset(adapter, 0, sizeof(*adapter));
    adapter->config = *config;
    if (!synchronize(adapter)) return false;
    memset(backend, 0, sizeof(*backend));
    backend->context = adapter;
    backend->region_size = config->region_size;
    backend->erase_size = config->page_size;
    backend->program_size = config->program_size;
    backend->map = map_bytes;
    backend->erase = fill_bytes;
    backend->program = program_bytes;
    backend->contract_version = RBSP_DEVICE_PARAM_BACKEND_CONTRACT_VERSION;
    backend->medium = RBSP_DEVICE_PARAM_MEDIUM_EXTERNAL_EEPROM;
    backend->erased_value = RBSP_DEVICE_PARAM_BACKEND_ERASED_VALUE;
    backend->capability_flags = RBSP_DEVICE_PARAM_BACKEND_FLAG_BYTE_REWRITABLE |
        RBSP_DEVICE_PARAM_BACKEND_FLAG_COMMIT_MARKER_LAST;
    return true;
}
