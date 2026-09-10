#include "remotebsp/protocol/device_parameter_schema.h"

#include <ctype.h>
#include <string.h>

static const rbsp_device_param_definition fixed_definitions[] = {
    {RBSP_DEVICE_PARAM_SERIAL_NUMBER, RBSP_DEVICE_PARAM_TYPE_UTF8,
     RBSP_DEVICE_PARAM_FLAG_FACTORY |
         RBSP_DEVICE_PARAM_FLAG_MAINTENANCE_WRITE |
         RBSP_DEVICE_PARAM_FLAG_APPLY_AFTER_RESTART,
     1U, 32U},
    {RBSP_DEVICE_PARAM_DEVICE_UUID, RBSP_DEVICE_PARAM_TYPE_BYTES,
     RBSP_DEVICE_PARAM_FLAG_FACTORY |
         RBSP_DEVICE_PARAM_FLAG_MAINTENANCE_WRITE |
         RBSP_DEVICE_PARAM_FLAG_WRITE_ONCE |
         RBSP_DEVICE_PARAM_FLAG_APPLY_AFTER_RESTART,
     16U, 16U},
    {RBSP_DEVICE_PARAM_HARDWARE_REVISION, RBSP_DEVICE_PARAM_TYPE_UTF8,
     RBSP_DEVICE_PARAM_FLAG_FACTORY |
         RBSP_DEVICE_PARAM_FLAG_MAINTENANCE_WRITE |
         RBSP_DEVICE_PARAM_FLAG_APPLY_AFTER_RESTART,
     1U, 16U},
    {RBSP_DEVICE_PARAM_MANUFACTURING_BATCH, RBSP_DEVICE_PARAM_TYPE_UTF8,
     RBSP_DEVICE_PARAM_FLAG_FACTORY |
         RBSP_DEVICE_PARAM_FLAG_MAINTENANCE_WRITE |
         RBSP_DEVICE_PARAM_FLAG_APPLY_AFTER_RESTART,
     0U, 16U},
    {RBSP_DEVICE_PARAM_MANUFACTURING_DATE, RBSP_DEVICE_PARAM_TYPE_UTF8,
     RBSP_DEVICE_PARAM_FLAG_FACTORY |
         RBSP_DEVICE_PARAM_FLAG_MAINTENANCE_WRITE |
         RBSP_DEVICE_PARAM_FLAG_APPLY_AFTER_RESTART,
     0U, 10U},
    {RBSP_DEVICE_PARAM_DEVICE_NAME, RBSP_DEVICE_PARAM_TYPE_UTF8,
     RBSP_DEVICE_PARAM_FLAG_USER |
         RBSP_DEVICE_PARAM_FLAG_MAINTENANCE_WRITE,
     0U, 32U},
};

size_t rbsp_device_param_definition_count(void) {
    return sizeof(fixed_definitions) / sizeof(fixed_definitions[0]) +
           RBSP_DEVICE_PARAM_ADC_CHANNEL_COUNT;
}

bool rbsp_device_param_definition_at(
    size_t index, rbsp_device_param_definition* definition) {
    const size_t fixed_count =
        sizeof(fixed_definitions) / sizeof(fixed_definitions[0]);
    if (definition == NULL) {
        return false;
    }
    if (index < fixed_count) {
        *definition = fixed_definitions[index];
        return true;
    }
    index -= fixed_count;
    if (index >= RBSP_DEVICE_PARAM_ADC_CHANNEL_COUNT) {
        return false;
    }
    definition->id = (uint16_t)(RBSP_DEVICE_PARAM_ADC_CALIBRATION_BASE +
                                index);
    definition->type = RBSP_DEVICE_PARAM_TYPE_ADC_CALIBRATION;
    definition->flags = RBSP_DEVICE_PARAM_FLAG_FACTORY |
                        RBSP_DEVICE_PARAM_FLAG_MAINTENANCE_WRITE |
                        RBSP_DEVICE_PARAM_FLAG_APPLY_AFTER_RESTART;
    definition->minimum_length =
        (uint16_t)sizeof(rbsp_device_param_adc_calibration);
    definition->maximum_length = RBSP_DEVICE_PARAM_ADC_CALIBRATION_V1_SIZE;
    return true;
}

bool rbsp_device_param_find_definition(
    uint16_t id, rbsp_device_param_definition* definition) {
    size_t index;
    for (index = 0U; index < rbsp_device_param_definition_count(); ++index) {
        rbsp_device_param_definition candidate;
        if (rbsp_device_param_definition_at(index, &candidate) &&
            candidate.id == id) {
            if (definition != NULL) {
                *definition = candidate;
            }
            return true;
        }
    }
    return false;
}

static bool printable_ascii(const uint8_t* value, size_t length) {
    size_t index;
    for (index = 0U; index < length; ++index) {
        if (value[index] < 0x20U || value[index] > 0x7EU) {
            return false;
        }
    }
    return true;
}

static bool manufacturing_date_valid(const uint8_t* value, size_t length) {
    size_t index;
    if (length == 0U) {
        return true;
    }
    if (length != 10U || value[4] != '-' || value[7] != '-') {
        return false;
    }
    for (index = 0U; index < length; ++index) {
        if (index == 4U || index == 7U) {
            continue;
        }
        if (!isdigit((int)value[index])) {
            return false;
        }
    }
    return true;
}

static uint32_t read_u32_le(const uint8_t* value) {
    return (uint32_t)value[0] | ((uint32_t)value[1] << 8U) |
           ((uint32_t)value[2] << 16U) | ((uint32_t)value[3] << 24U);
}

static uint16_t read_u16_le(const uint8_t* value) {
    return (uint16_t)((uint16_t)value[0] | ((uint16_t)value[1] << 8U));
}

static uint32_t crc32_ieee(const uint8_t* value, size_t length) {
    uint32_t crc = UINT32_MAX;
    size_t index;
    for (index = 0U; index < length; ++index) {
        unsigned bit;
        crc ^= value[index];
        for (bit = 0U; bit < 8U; ++bit) {
            crc = (crc >> 1U) ^ ((crc & 1U) != 0U ? 0xEDB88320U : 0U);
        }
    }
    return ~crc;
}

static bool adc_calibration_v1_valid(const uint8_t* value) {
    const uint8_t mode = value[5];
    const uint8_t channel = value[6];
    const uint8_t bits = value[7];
    const uint32_t resource_id = read_u32_le(value + 8U);
    const uint32_t minimum_uv = read_u32_le(value + 12U);
    const uint32_t maximum_uv = read_u32_le(value + 16U);
    const uint32_t reference_uv = read_u32_le(value + 20U);
    const int32_t gain = (int32_t)read_u32_le(value + 24U);
    const uint8_t point_count = value[32];
    unsigned index;
    uint16_t previous_code = 0U;
    uint32_t previous_uv = 0U;
    size_t reserved_index;
    if (memcmp(value, "ADCC", 4U) != 0 || value[4] != 1U ||
        channel >= RBSP_DEVICE_PARAM_ADC_CHANNEL_COUNT || bits < 8U || bits > 16U ||
        (resource_id >> 24U) != 0x05U || minimum_uv >= maximum_uv ||
        reference_uv < 1000000U || reference_uv > 5000000U ||
        reference_uv < minimum_uv || reference_uv > maximum_uv ||
        read_u32_le(value + 60U) != crc32_ieee(value, 60U)) {
        return false;
    }
    for (reserved_index = 33U; reserved_index < 36U; ++reserved_index) {
        if (value[reserved_index] != 0U) {
            return false;
        }
    }
    if (mode == 1U) {
        if (point_count != 0U || gain <= 0) {
            return false;
        }
        for (reserved_index = 36U; reserved_index < 60U; ++reserved_index) {
            if (value[reserved_index] != 0U) {
                return false;
            }
        }
        return true;
    }
    if (mode != 2U || point_count < 2U || point_count > 4U || gain != 0 ||
        read_u32_le(value + 28U) != 0U) {
        return false;
    }
    for (index = 0U; index < point_count; ++index) {
        const uint8_t* point = value + 36U + index * 6U;
        const uint16_t code = read_u16_le(point);
        const uint32_t uv = read_u32_le(point + 2U);
        if ((index > 0U && (code <= previous_code || uv <= previous_uv)) ||
            uv < minimum_uv || uv > maximum_uv ||
            (bits < 16U && code >= (1U << bits))) {
            return false;
        }
        previous_code = code;
        previous_uv = uv;
    }
    for (reserved_index = 36U + (size_t)point_count * 6U;
         reserved_index < 60U; ++reserved_index) {
        if (value[reserved_index] != 0U) {
            return false;
        }
    }
    return true;
}

bool rbsp_device_param_validate_value(
    uint16_t id, const uint8_t* value, size_t length) {
    rbsp_device_param_definition definition;
    if (!rbsp_device_param_find_definition(id, &definition) ||
        length < definition.minimum_length ||
        length > definition.maximum_length ||
        (length > 0U && value == NULL)) {
        return false;
    }
    if (definition.type == RBSP_DEVICE_PARAM_TYPE_UTF8 &&
        !printable_ascii(value, length)) {
        return false;
    }
    if (id == RBSP_DEVICE_PARAM_MANUFACTURING_DATE &&
        !manufacturing_date_valid(value, length)) {
        return false;
    }
    if (definition.type == RBSP_DEVICE_PARAM_TYPE_ADC_CALIBRATION) {
        if (length == RBSP_DEVICE_PARAM_ADC_CALIBRATION_V1_SIZE) {
            return value[6] == (uint8_t)(id - RBSP_DEVICE_PARAM_ADC_CALIBRATION_BASE) &&
                   adc_calibration_v1_valid(value);
        }
        if (length != sizeof(rbsp_device_param_adc_calibration)) {
            return false;
        }
        rbsp_device_param_adc_calibration calibration;
        memcpy(&calibration, value, sizeof(calibration));
        if (calibration.gain_q16_16 <= 0 ||
            calibration.reference_uv < 1000000U ||
            calibration.reference_uv > 5000000U) {
            return false;
        }
    }
    return true;
}
