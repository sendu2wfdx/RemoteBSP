cmake_minimum_required(VERSION 3.20)
project(remotebsp_firmware C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(RBSP_CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/.config" CACHE FILEPATH
    "Kconfig 配置文件")

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(GENERATED_CONFIG "${GENERATED_DIR}/remotebsp_config.h")

file(STRINGS "${RBSP_CONFIG}" RBSP_MOTION_LINES
     REGEX "^CONFIG_REMOTEBSP_MOTION=y$")
file(STRINGS "${RBSP_CONFIG}" RBSP_TMC_UART_LINES
     REGEX "^CONFIG_(REMOTEBSP_TMC2209_UART|MOTION_SLOT[0-4]_DRIVER_TMC2209_UART)=y$")
set(RBSP_OPTIONAL_CORE_SOURCES)
file(STRINGS "${RBSP_CONFIG}" RBSP_WAVEFORM_LINES
     REGEX "^CONFIG_REMOTEBSP_(PWM|TIMED_BITSTREAM)=y$")
if(RBSP_MOTION_LINES)
    list(APPEND RBSP_OPTIONAL_CORE_SOURCES src/motion.c)
endif()
if(RBSP_TMC_UART_LINES)
    list(APPEND RBSP_OPTIONAL_CORE_SOURCES src/soft_half_duplex_uart.c)
endif()
if(RBSP_WAVEFORM_LINES)
    set(RBSP_OPTIONAL_BOARD_SOURCES src/board_waveform.c)
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
    "${GENERATED_DIR}"
)
target_compile_options(remotebsp_embedded_core PRIVATE
    -Wall -Wextra -Wpedantic -Werror
    -ffunction-sections -fdata-sections
)

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
    target_link_options(${target} PRIVATE
        -mcpu=${cpu} -mthumb
        -T${linker_script}
        -Wl,--defsym=RBSP_APP_FLASH_OFFSET=${RBSP_APP_FLASH_OFFSET}
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
    set(BOARD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/boards/stm32f072rbt6")
    set(DEVICE_DIR "${VENDOR_DIR}/cmsis-device-f0")
    set(HAL_DIR "${VENDOR_DIR}/stm32f0xx-hal-driver")
    if(NOT EXISTS "${DEVICE_DIR}/Include/stm32f072xb.h" OR
       NOT EXISTS "${HAL_DIR}/Src/stm32f0xx_hal_can.c")
        message(FATAL_ERROR
            "缺少 STM32F0 官方依赖，请先运行 scripts/fetch_stm32_deps.sh")
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
    )
    target_compile_definitions(remotebsp-stm32f072rbt6 PRIVATE
        USE_HAL_DRIVER STM32F072xB)
    target_compile_options(remotebsp_embedded_core PRIVATE
        -mcpu=cortex-m0 -mthumb)
    target_include_directories(remotebsp-stm32f072rbt6 PRIVATE
        "${BOARD_DIR}" "${HAL_DIR}/Inc" "${DEVICE_DIR}/Include"
        "${VENDOR_DIR}/cmsis-core/Core/Include")
    rbsp_common_target(remotebsp-stm32f072rbt6 cortex-m0
        "${BOARD_DIR}/STM32F072RBTx_FLASH.ld")
elseif(RBSP_BOARD_LINES MATCHES "STM32F103CBT6")
    set(BOARD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/boards/stm32f103cbt6")
    set(DEVICE_DIR "${VENDOR_DIR}/cmsis-device-f1")
    set(HAL_DIR "${VENDOR_DIR}/stm32f1xx-hal-driver")
    add_executable(remotebsp-stm32f103cbt6
        "${BOARD_DIR}/main.c"
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
    )
    target_compile_definitions(remotebsp-stm32f103cbt6 PRIVATE
        USE_HAL_DRIVER STM32F103xB)
    target_compile_options(remotebsp_embedded_core PRIVATE
        -mcpu=cortex-m3 -mthumb)
    target_include_directories(remotebsp-stm32f103cbt6 PRIVATE
        "${BOARD_DIR}" "${HAL_DIR}/Inc" "${DEVICE_DIR}/Include"
        "${VENDOR_DIR}/cmsis-core/Core/Include")
    rbsp_common_target(remotebsp-stm32f103cbt6 cortex-m3
        "${BOARD_DIR}/STM32F103CBTx_FLASH.ld")
elseif(RBSP_BOARD_LINES MATCHES "STM32G431CBU6")
    set(BOARD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/boards/stm32g431cbu6")
    set(DEVICE_DIR "${VENDOR_DIR}/cmsis-device-g4")
    set(HAL_DIR "${VENDOR_DIR}/stm32g4xx-hal-driver")
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
        ${RBSP_OPTIONAL_BOARD_SOURCES}
        src/startup_gpio.c
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
        ${RBSP_USB_BOARD_SOURCES}
    )
    target_compile_definitions(remotebsp-stm32g431cbu6 PRIVATE
        USE_HAL_DRIVER STM32G431xx)
    target_compile_options(remotebsp_embedded_core PRIVATE
        -mcpu=cortex-m4 -mthumb
        -mfpu=fpv4-sp-d16 -mfloat-abi=hard)
    target_compile_options(remotebsp-stm32g431cbu6 PRIVATE
        -mfpu=fpv4-sp-d16 -mfloat-abi=hard)
    target_link_options(remotebsp-stm32g431cbu6 PRIVATE
        -mfpu=fpv4-sp-d16 -mfloat-abi=hard)
    target_include_directories(remotebsp-stm32g431cbu6 PRIVATE
        "${BOARD_DIR}" "${HAL_DIR}/Inc" "${DEVICE_DIR}/Include"
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
