cmake_minimum_required(VERSION 3.20)
project(remotebsp_firmware C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(RBSP_CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/.config" CACHE FILEPATH
    "Kconfig 配置文件")

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(GENERATED_CONFIG "${GENERATED_DIR}/remotebsp_config.h")

add_custom_command(
    OUTPUT "${GENERATED_CONFIG}"
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/genconfig.py"
            --kconfig "${CMAKE_CURRENT_SOURCE_DIR}/Kconfig"
            --config "${RBSP_CONFIG}"
            --header "${GENERATED_CONFIG}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Kconfig" "${RBSP_CONFIG}"
    VERBATIM
)

add_library(remotebsp_embedded_core STATIC
    src/core.c
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
     REGEX "^CONFIG_BOARD_STM32(F103CBT6|G431CBU6)=y$")
list(LENGTH RBSP_BOARD_LINES RBSP_BOARD_COUNT)
if(NOT RBSP_BOARD_COUNT EQUAL 1)
    message(FATAL_ERROR "配置文件必须且只能选择一块 STM32 目标板")
endif()

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

if(RBSP_BOARD_LINES MATCHES "STM32F103CBT6")
    set(BOARD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/boards/stm32f103cbt6")
    set(DEVICE_DIR "${VENDOR_DIR}/cmsis-device-f1")
    set(HAL_DIR "${VENDOR_DIR}/stm32f1xx-hal-driver")
    add_executable(remotebsp-stm32f103cbt6
        "${BOARD_DIR}/main.c"
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
        "${HAL_DIR}/Src/stm32f1xx_hal_uart.c"
        src/byte_ring.c
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
else()
    set(BOARD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/boards/stm32g431cbu6")
    set(DEVICE_DIR "${VENDOR_DIR}/cmsis-device-g4")
    set(HAL_DIR "${VENDOR_DIR}/stm32g4xx-hal-driver")
    add_executable(remotebsp-stm32g431cbu6
        "${BOARD_DIR}/main.c"
        "${DEVICE_DIR}/Source/Templates/system_stm32g4xx.c"
        "${DEVICE_DIR}/Source/Templates/gcc/startup_stm32g431xx.s"
        "${HAL_DIR}/Src/stm32g4xx_hal.c"
        "${HAL_DIR}/Src/stm32g4xx_hal_cortex.c"
        "${HAL_DIR}/Src/stm32g4xx_hal_rcc.c"
        "${HAL_DIR}/Src/stm32g4xx_hal_rcc_ex.c"
        "${HAL_DIR}/Src/stm32g4xx_hal_flash.c"
        "${HAL_DIR}/Src/stm32g4xx_hal_flash_ex.c"
        "${HAL_DIR}/Src/stm32g4xx_hal_gpio.c"
        "${HAL_DIR}/Src/stm32g4xx_hal_pwr.c"
        "${HAL_DIR}/Src/stm32g4xx_hal_pwr_ex.c"
        "${HAL_DIR}/Src/stm32g4xx_hal_fdcan.c"
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
    rbsp_common_target(remotebsp-stm32g431cbu6 cortex-m4
        "${BOARD_DIR}/STM32G431CBUx_FLASH.ld")
endif()
