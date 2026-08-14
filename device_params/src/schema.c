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
    definition->maximum_length = definition->minimum_length;
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
