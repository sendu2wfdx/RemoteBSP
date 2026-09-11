cmake_minimum_required(VERSION 3.20)
project(remotebsp_firmware C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(RBSP_CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/.config" CACHE FILEPATH
    "Kconfig 配置文件")
set(RBSP_STATIC_RESOURCE_TABLE "" CACHE FILEPATH
    "Studio生成的实体固件静态资源表；为空时兼容Kconfig静态GPIO列表")
set(RBSP_FIRMWARE_INPUT_SHA256 "" CACHE STRING
    "Studio构建输入身份SHA-256；仅与静态资源表共同使用")

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(GENERATED_CONFIG "${GENERATED_DIR}/remotebsp_config.h")
if(RBSP_STATIC_RESOURCE_TABLE)
    if(NOT EXISTS "${RBSP_STATIC_RESOURCE_TABLE}")
        message(FATAL_ERROR "Studio静态资源表不存在：${RBSP_STATIC_RESOURCE_TABLE}")
    endif()
    configure_file("${RBSP_STATIC_RESOURCE_TABLE}"
                   "${GENERATED_DIR}/remotebsp_static_resources.h" COPYONLY)
    set(RBSP_STUDIO_RESOURCE_DEFINITION RBSP_STUDIO_STATIC_RESOURCE_TABLE=1)
    file(SHA256 "${RBSP_CONFIG}" RBSP_CONFIG_SHA256)
    if(RBSP_FIRMWARE_INPUT_SHA256)
        string(LENGTH "${RBSP_FIRMWARE_INPUT_SHA256}"
               RBSP_FIRMWARE_INPUT_SHA256_LENGTH)
        if(NOT RBSP_FIRMWARE_INPUT_SHA256_LENGTH EQUAL 64 OR
           NOT RBSP_FIRMWARE_INPUT_SHA256 MATCHES "^[0-9a-fA-F]+$")
            message(FATAL_ERROR
                "RBSP_FIRMWARE_INPUT_SHA256必须为64位十六进制")
        endif()
    endif()
endif()

file(STRINGS "${RBSP_CONFIG}" RBSP_MOTION_LINES
     REGEX "^CONFIG_REMOTEBSP_MOTION=y$")
file(STRINGS "${RBSP_CONFIG}" RBSP_TMC_UART_LINES
     REGEX "^CONFIG_(REMOTEBSP_TMC2209_UART|MOTION_SLOT[0-4]_DRIVER_TMC2209_UART)=y$")
set(RBSP_OPTIONAL_CORE_SOURCES)
set(RBSP_OPTIONAL_BOARD_SOURCES)
list(APPEND RBSP_OPTIONAL_CORE_SOURCES
     "${CMAKE_CURRENT_SOURCE_DIR}/../device_params/src/boot_epoch.c")
file(STRINGS "${RBSP_CONFIG}" RBSP_DEVICE_PARAM_DISABLED_LINES
     REGEX "^# CONFIG_REMOTEBSP_DEVICE_PARAMS is not set$")
file(STRINGS "${RBSP_CONFIG}" RBSP_F103_BOARD_LINES
     REGEX "^CONFIG_BOARD_STM32F103CBT6=y$")
file(STRINGS "${RBSP_CONFIG}" RBSP_DEVICE_PARAM_EEPROM_LINES
     REGEX "^CONFIG_REMOTEBSP_DEVICE_PARAM_EXTERNAL_EEPROM=y$")
if(NOT RBSP_DEVICE_PARAM_DISABLED_LINES)
    list(APPEND RBSP_OPTIONAL_CORE_SOURCES
         "${CMAKE_CURRENT_SOURCE_DIR}/../device_params/src/schema.c"
         "${CMAKE_CURRENT_SOURCE_DIR}/../device_params/src/store.c")
    list(APPEND RBSP_OPTIONAL_BOARD_SOURCES src/device_parameter_backend.c)
    if(RBSP_DEVICE_PARAM_EEPROM_LINES)
        list(APPEND RBSP_OPTIONAL_BOARD_SOURCES src/device_parameter_eeprom.c)
        set(RBSP_DEVICE_PARAM_FLASH_SIZE 0x0)
    elseif(RBSP_F103_BOARD_LINES)
        list(APPEND RBSP_OPTIONAL_BOARD_SOURCES src/device_parameter_flash.c)
        # F103中密度器件的擦除页为1KiB。
        set(RBSP_DEVICE_PARAM_FLASH_SIZE 0x800)
    else()
        list(APPEND RBSP_OPTIONAL_BOARD_SOURCES src/device_parameter_flash.c)
        # F072和G431的擦除页均为2KiB。
        set(RBSP_DEVICE_PARAM_FLASH_SIZE 0x1000)
    endif()
else()
    set(RBSP_DEVICE_PARAM_FLASH_SIZE 0x0)
endif()
file(STRINGS "${RBSP_CONFIG}" RBSP_WAVEFORM_LINES
     REGEX "^CONFIG_REMOTEBSP_(PWM|TIMED_BITSTREAM)=y$")
file(STRINGS "${RBSP_CONFIG}" RBSP_BUS_LINES
     REGEX "^CONFIG_REMOTEBSP_BUS=y$")
if(RBSP_MOTION_LINES)
    list(APPEND RBSP_OPTIONAL_CORE_SOURCES
         src/motion.c
         src/motion_group.c)
endif()
if(RBSP_TMC_UART_LINES)
    list(APPEND RBSP_OPTIONAL_CORE_SOURCES src/soft_half_duplex_uart.c)
endif()
if(RBSP_WAVEFORM_LINES)
    list(APPEND RBSP_OPTIONAL_BOARD_SOURCES src/board_waveform.c)
endif()
file(STRINGS "${RBSP_CONFIG}" RBSP_USB_LINK_LINES
     REGEX "^CONFIG_REMOTEBSP_TRANSPORT_USB=y$")
if(RBSP_USB_LINK_LINES)
    list(APPEND RBSP_OPTIONAL_CORE_SOURCES src/usb_link.c)
endif()

add_custom_command(
    OUTPUT "${GENERATED_CONFIG}"
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/genconfig.py"
            --kconfig "${CMAKE_CURRENT_SOURCE_DIR}/Kconfig"
            --config "${RBSP_CONFIG}"
            --header "${GENERATED_CONFIG}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Kconfig"
            "${RBSP_CONFIG}"
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/genconfig.py"
    VERBATIM
)

add_library(remotebsp_embedded_core STATIC
    src/byte_ring.c
    src/core.c
    ${RBSP_OPTIONAL_CORE_SOURCES}
    "${GENERATED_CONFIG}"
)
target_include_directories(remotebsp_embedded_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../device_params/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../protocol/include"
    "${GENERATED_DIR}"
)
target_compile_options(remotebsp_embedded_core PRIVATE
    -Os
    -Wall -Wextra -Wpedantic -Werror
    -ffunction-sections -fdata-sections
)
if(RBSP_STUDIO_RESOURCE_DEFINITION)
    target_compile_definitions(remotebsp_embedded_core PRIVATE
        ${RBSP_STUDIO_RESOURCE_DEFINITION}
        RBSP_CONFIG_SHA256="${RBSP_CONFIG_SHA256}"
        RBSP_FIRMWARE_INPUT_SHA256="${RBSP_FIRMWARE_INPUT_SHA256}")
endif()

file(STRINGS "${RBSP_CONFIG}" RBSP_BOARD_LINES
     REGEX "^CONFIG_BOARD_STM32(F072RBT6|F103CBT6|G431CBU6)=y$")
list(LENGTH RBSP_BOARD_LINES RBSP_BOARD_COUNT)
if(NOT RBSP_BOARD_COUNT EQUAL 1)
    message(FATAL_ERROR "配置文件必须且只能选择一块 STM32 目标板")
endif()

file(STRINGS "${RBSP_CONFIG}" RBSP_APP_OFFSET_LINES
     REGEX "^CONFIG_APPLICATION_FLASH_OFFSET=0x[0-9A-Fa-f]+$")
list(LENGTH RBSP_APP_OFFSET_LINES RBSP_APP_OFFSET_COUNT)
if(NOT RBSP_APP_OFFSET_COUNT EQUAL 1)
    message(FATAL_ERROR
        "配置文件必须显式定义一个 CONFIG_APPLICATION_FLASH_OFFSET")
endif()
list(GET RBSP_APP_OFFSET_LINES 0 RBSP_APP_OFFSET_LINE)
string(REGEX REPLACE
       "^CONFIG_APPLICATION_FLASH_OFFSET=(0x[0-9A-Fa-f]+)$" "\\1"
       RBSP_APP_FLASH_OFFSET "${RBSP_APP_OFFSET_LINE}")

set(VENDOR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor")
if(NOT EXISTS "${VENDOR_DIR}/cmsis-core/Core/Include/core_cm3.h")
    message(FATAL_ERROR
        "缺少 ST 官方依赖，请先运行 scripts/fetch_stm32_deps.sh")
endif()

function(rbsp_common_target target cpu linker_script)
    target_link_libraries(${target} PRIVATE remotebsp_embedded_core)
    target_compile_options(${target} PRIVATE
        -mcpu=${cpu} -mthumb -Os
        -ffunction-sections -fdata-sections
        -Wall -Wextra -Wpedantic
    )
    if(RBSP_STUDIO_RESOURCE_DEFINITION)
        target_compile_definitions(${target} PRIVATE
            ${RBSP_STUDIO_RESOURCE_DEFINITION})
    endif()
    target_link_options(${target} PRIVATE
        -mcpu=${cpu} -mthumb
        -T${linker_script}
        -Wl,--defsym=RBSP_APP_FLASH_OFFSET=${RBSP_APP_FLASH_OFFSET}
        -Wl,--defsym=RBSP_DEVICE_PARAM_FLASH_SIZE=${RBSP_DEVICE_PARAM_FLASH_SIZE}
        -Wl,--defsym=RBSP_HEALTH_EPOCH_FLASH_SIZE=${RBSP_HEALTH_EPOCH_FLASH_SIZE}
        -Wl,--defsym=RBSP_MOTION_EPOCH_FLASH_SIZE=${RBSP_MOTION_EPOCH_FLASH_SIZE}
        --specs=nano.specs --specs=nosys.specs
        -Wl,--gc-sections
        -Wl,-Map=${target}.map
        -Wl,--print-memory-usage
    )
    set_target_properties(${target} PROPERTIES SUFFIX ".elf")
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${ARM_OBJCOPY}" -O ihex
                "$<TARGET_FILE:${target}>" "${target}.hex"
        COMMAND "${ARM_OBJCOPY}" -O binary
                "$<TARGET_FILE:${target}>" "${target}.bin"
        COMMAND "${ARM_SIZE}" "$<TARGET_FILE:${target}>"
        VERBATIM
    )
endfunction()

if(RBSP_BOARD_LINES MATCHES "STM32F072RBT6")
    # F072页为2KiB，代际日志固定使用双页。
    set(RBSP_HEALTH_EPOCH_FLASH_SIZE 0x1000)
    set(RBSP_MOTION_EPOCH_FLASH_SIZE 0x1000)
    set(BOARD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/boards/stm32f072rbt6")
    set(DEVICE_DIR "${VENDOR_DIR}/cmsis-device-f0")
    set(HAL_DIR "${VENDOR_DIR}/stm32f0xx-hal-driver")
    if(NOT EXISTS "${DEVICE_DIR}/Include/stm32f072xb.h" OR
       NOT EXISTS "${HAL_DIR}/Src/stm32f0xx_hal_can.c")
        message(FATAL_ERROR
            "缺少 STM32F0 官方依赖，请先运行 scripts/fetch_stm32_deps.sh")
    endif()
    if(RBSP_BUS_LINES)
        set(RBSP_F072_BUS_SOURCES
            "${HAL_DIR}/Src/stm32f0xx_hal_i2c.c"
            "${HAL_DIR}/Src/stm32f0xx_hal_i2c_ex.c"
            "${HAL_DIR}/Src/stm32f0xx_hal_spi.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/boards/common/stm32_bus_hal.c")
    endif()
    add_executable(remotebsp-stm32f072rbt6
        "${BOARD_DIR}/main.c"
        ${RBSP_OPTIONAL_BOARD_SOURCES}
        "${DEVICE_DIR}/Source/Templates/system_stm32f0xx.c"
        "${DEVICE_DIR}/Source/Templates/gcc/startup_stm32f072xb.s"
        "${HAL_DIR}/Src/stm32f0xx_hal.c"
        "${HAL_DIR}/Src/stm32f0xx_hal_cortex.c"
        "${HAL_DIR}/Src/stm32f0xx_hal_rcc.c"
        "${HAL_DIR}/Src/stm32f0xx_hal_rcc_ex.c"
        "${HAL_DIR}/Src/stm32f0xx_hal_flash.c"
        "${HAL_DIR}/Src/stm32f0xx_hal_flash_ex.c"
        "${HAL_DIR}/Src/stm32f0xx_hal_gpio.c"
        "${HAL_DIR}/Src/stm32f0xx_hal_can.c"
        "${HAL_DIR}/Src/stm32f0xx_hal_dma.c"
        "${HAL_DIR}/Src/stm32f0xx_hal_tim.c"
        "${HAL_DIR}/Src/stm32f0xx_hal_uart.c"
        src/startup_gpio.c
        src/static_resources.c
        boards/common/stm32_health_hal.c
        ${RBSP_F072_BUS_SOURCES}
    )
    target_compile_definitions(remotebsp-stm32f072rbt6 PRIVATE
        USE_HAL_DRIVER STM32F072xB)
    if(RBSP_BUS_LINES)
        target_compile_definitions(remotebsp-stm32f072rbt6 PRIVATE
            RBSP_F072_BUS_ENABLED=1)
    endif()
    target_compile_options(remotebsp_embedded_core PRIVATE
        -mcpu=cortex-m0 -mthumb)
    target_include_directories(remotebsp-stm32f072rbt6 PRIVATE
        "${BOARD_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/boards/common"
        "${HAL_DIR}/Inc" "${DEVICE_DIR}/Include"
        "${VENDOR_DIR}/cmsis-core/Core/Include")
    rbsp_common_target(remotebsp-stm32f072rbt6 cortex-m0
        "${BOARD_DIR}/STM32F072RBTx_FLASH.ld")
elseif(RBSP_BOARD_LINES MATCHES "STM32F103CBT6")
    # F103中密度器件页为1KiB，代际日志固定使用双页。
    set(RBSP_HEALTH_EPOCH_FLASH_SIZE 0x800)
    set(RBSP_MOTION_EPOCH_FLASH_SIZE 0x800)
    set(BOARD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/boards/stm32f103cbt6")
    set(DEVICE_DIR "${VENDOR_DIR}/cmsis-device-f1")
    set(HAL_DIR "${VENDOR_DIR}/stm32f1xx-hal-driver")
    if(RBSP_BUS_LINES)
        set(RBSP_F103_BUS_SOURCES
            "${HAL_DIR}/Src/stm32f1xx_hal_i2c.c"
            "${HAL_DIR}/Src/stm32f1xx_hal_spi.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/boards/common/stm32_bus_hal.c")
    endif()
    add_executable(remotebsp-stm32f103cbt6
        "${BOARD_DIR}/main.c"
        boards/common/stm32_health_hal.c
        ${RBSP_OPTIONAL_BOARD_SOURCES}
        "${DEVICE_DIR}/Source/Templates/system_stm32f1xx.c"
        "${DEVICE_DIR}/Source/Templates/gcc/startup_stm32f103xb.s"
        "${HAL_DIR}/Src/stm32f1xx_hal.c"
        "${HAL_DIR}/Src/stm32f1xx_hal_cortex.c"
        "${HAL_DIR}/Src/stm32f1xx_hal_rcc.c"
        "${HAL_DIR}/Src/stm32f1xx_hal_rcc_ex.c"
        "${HAL_DIR}/Src/stm32f1xx_hal_flash.c"
        "${HAL_DIR}/Src/stm32f1xx_hal_flash_ex.c"
        "${HAL_DIR}/Src/stm32f1xx_hal_gpio.c"
        "${HAL_DIR}/Src/stm32f1xx_hal_can.c"
        "${HAL_DIR}/Src/stm32f1xx_hal_dma.c"
        "${HAL_DIR}/Src/stm32f1xx_hal_tim.c"
        "${HAL_DIR}/Src/stm32f1xx_hal_uart.c"
        src/startup_gpio.c
        src/static_resources.c
        ${RBSP_F103_BUS_SOURCES}
    )
    target_compile_definitions(remotebsp-stm32f103cbt6 PRIVATE
        USE_HAL_DRIVER STM32F103xB)
    if(RBSP_BUS_LINES)
        target_compile_definitions(remotebsp-stm32f103cbt6 PRIVATE
            RBSP_F103_BUS_ENABLED=1)
    endif()
    target_compile_options(remotebsp_embedded_core PRIVATE
        -mcpu=cortex-m3 -mthumb)
    target_include_directories(remotebsp-stm32f103cbt6 PRIVATE
        "${BOARD_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/boards/common"
        "${HAL_DIR}/Inc" "${DEVICE_DIR}/Include"
        "${VENDOR_DIR}/cmsis-core/Core/Include")
    rbsp_common_target(remotebsp-stm32f103cbt6 cortex-m3
        "${BOARD_DIR}/STM32F103CBTx_FLASH.ld")
elseif(RBSP_BOARD_LINES MATCHES "STM32G431CBU6")
    # G431页为2KiB，代际日志固定使用双页。
    set(RBSP_HEALTH_EPOCH_FLASH_SIZE 0x1000)
    set(RBSP_MOTION_EPOCH_FLASH_SIZE 0x1000)
    set(BOARD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/boards/stm32g431cbu6")
    set(DEVICE_DIR "${VENDOR_DIR}/cmsis-device-g4")
    set(HAL_DIR "${VENDOR_DIR}/stm32g4xx-hal-driver")
    if(RBSP_BUS_LINES)
        set(RBSP_G431_BUS_SOURCES
            "${HAL_DIR}/Src/stm32g4xx_hal_i2c.c"
            "${HAL_DIR}/Src/stm32g4xx_hal_i2c_ex.c"
            "${HAL_DIR}/Src/stm32g4xx_hal_spi.c"
            "${HAL_DIR}/Src/stm32g4xx_hal_spi_ex.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/boards/common/stm32_bus_hal.c")
    endif()
    if(RBSP_USB_LINK_LINES)
        set(USB_DEVICE_DIR "${VENDOR_DIR}/stm32-mw-usb-device")
        if(NOT EXISTS "${USB_DEVICE_DIR}/Core/Src/usbd_core.c")
            message(FATAL_ERROR
                "USB 主通信链路缺少 ST USB Device 中间件，请先获取固件依赖")
        endif()
        set(RBSP_USB_BOARD_SOURCES
            src/usb_device_link.c
            "${USB_DEVICE_DIR}/Core/Src/usbd_core.c"
            "${USB_DEVICE_DIR}/Core/Src/usbd_ctlreq.c"
            "${USB_DEVICE_DIR}/Core/Src/usbd_ioreq.c"
            "${HAL_DIR}/Src/stm32g4xx_hal_pcd.c"
            "${HAL_DIR}/Src/stm32g4xx_hal_pcd_ex.c"
            "${HAL_DIR}/Src/stm32g4xx_ll_usb.c"
        )
    endif()
    add_executable(remotebsp-stm32g431cbu6
        "${BOARD_DIR}/main.c"
        boards/common/stm32_health_hal.c
        ${RBSP_OPTIONAL_BOARD_SOURCES}
        src/startup_gpio.c
        src/static_resources.c
        "${DEVICE_DIR}/Source/Templates/system_stm32g4xx.c"
        "${DEVICE_DIR}/Source/Templates/gcc/startup_stm32g431xx.s"
        "${HAL_DIR}/Src/stm32g4xx_hal.c"
        "${HAL_DIR}/Src/stm32g4xx_hal_cortex.c"
        "${HAL_DIR}/Src/stm32g4xx_hal_rcc.c"
        "${HAL_DIR}/Src/stm32g4xx_hal_rcc_ex.c"
        "${HAL_DIR}/Src/stm32g4xx_hal_flash.c"
        "${HAL_DIR}/Src/stm32g4xx_hal_flash_ex.c"
        "${HAL_DIR}/Src/stm32g4xx_hal_gpio.c"
        "${HAL_DIR}/Src/stm32g4xx_hal_dma.c"
        "${HAL_DIR}/Src/stm32g4xx_hal_pwr.c"
        "${HAL_DIR}/Src/stm32g4xx_hal_pwr_ex.c"
        "${HAL_DIR}/Src/stm32g4xx_hal_tim.c"
        "${HAL_DIR}/Src/stm32g4xx_hal_fdcan.c"
        "${HAL_DIR}/Src/stm32g4xx_hal_uart.c"
        ${RBSP_G431_BUS_SOURCES}
        ${RBSP_USB_BOARD_SOURCES}
    )
    target_compile_definitions(remotebsp-stm32g431cbu6 PRIVATE
        USE_HAL_DRIVER STM32G431xx)
    if(RBSP_BUS_LINES)
        target_compile_definitions(remotebsp-stm32g431cbu6 PRIVATE
            RBSP_G431_BUS_ENABLED=1)
    endif()
    target_compile_options(remotebsp_embedded_core PRIVATE
        -mcpu=cortex-m4 -mthumb
        -mfpu=fpv4-sp-d16 -mfloat-abi=hard)
    target_compile_options(remotebsp-stm32g431cbu6 PRIVATE
        -mfpu=fpv4-sp-d16 -mfloat-abi=hard)
    target_link_options(remotebsp-stm32g431cbu6 PRIVATE
        -mfpu=fpv4-sp-d16 -mfloat-abi=hard)
    target_include_directories(remotebsp-stm32g431cbu6 PRIVATE
        "${BOARD_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/boards/common"
        "${HAL_DIR}/Inc" "${DEVICE_DIR}/Include"
        "${VENDOR_DIR}/cmsis-core/Core/Include")
    if(RBSP_USB_LINK_LINES)
        target_include_directories(remotebsp-stm32g431cbu6 PRIVATE
            "${USB_DEVICE_DIR}/Core/Inc")
    endif()
    rbsp_common_target(remotebsp-stm32g431cbu6 cortex-m4
        "${BOARD_DIR}/STM32G431CBUx_FLASH.ld")
else()
    message(FATAL_ERROR "尚未实现所选 STM32 目标板")
endif()
