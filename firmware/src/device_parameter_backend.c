#include "remotebsp_embedded/device_parameter_backend.h"
#include "remotebsp_config.h"

#ifdef CONFIG_REMOTEBSP_DEVICE_PARAM_EXTERNAL_EEPROM
#include "remotebsp_embedded/device_parameter_eeprom.h"

static uint8_t eeprom_mirror[CONFIG_REMOTEBSP_DEVICE_PARAM_EEPROM_PAGE_SIZE * 2U];
static rbsp_device_parameter_eeprom_adapter eeprom_adapter;

/* 板级强定义负责填入回调及io_context；默认实现失败关闭。 */
__attribute__((weak)) bool rbsp_board_device_parameter_eeprom_config(
    rbsp_device_parameter_eeprom_config* config) {
    (void)config;
    return false;
}

bool rbsp_device_parameter_backend_init(rbsp_device_param_backend* backend) {
    rbsp_device_parameter_eeprom_config config = {0};
    if (!rbsp_board_device_parameter_eeprom_config(&config)) return false;
    if (config.page_size != CONFIG_REMOTEBSP_DEVICE_PARAM_EEPROM_PAGE_SIZE ||
        config.region_size != sizeof(eeprom_mirror)) return false;
    config.mirror = eeprom_mirror;
    config.mirror_size = sizeof(eeprom_mirror);
    return rbsp_device_parameter_eeprom_backend_init(
        &eeprom_adapter, backend, &config);
}
#else
#include "remotebsp_embedded/device_parameter_flash.h"
bool rbsp_device_parameter_backend_init(rbsp_device_param_backend* backend) {
    return rbsp_device_parameter_flash_backend_init(backend);
}
#endif
