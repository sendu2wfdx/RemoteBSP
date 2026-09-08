add_executable(protocol_tests protocol_tests.cpp)
target_link_libraries(protocol_tests PRIVATE remotebsp_protocol)

target_compile_options(protocol_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
)

add_test(NAME protocol_tests COMMAND protocol_tests)

add_executable(bus_stream_protocol_tests bus_stream_protocol_tests.cpp)
target_link_libraries(bus_stream_protocol_tests PRIVATE remotebsp_protocol)
target_compile_options(bus_stream_protocol_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
)
add_test(NAME bus_stream_protocol_tests COMMAND bus_stream_protocol_tests)

add_executable(bus_resource_tests bus_resource_tests.cpp)
target_link_libraries(bus_resource_tests PRIVATE remotebsp_remote_core)
target_compile_options(bus_resource_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
)
add_test(NAME bus_resource_tests COMMAND bus_resource_tests)

add_executable(device_parameter_protocol_tests
    device_parameter_protocol_tests.cpp)
target_link_libraries(device_parameter_protocol_tests PRIVATE
    remotebsp_protocol)
target_compile_options(device_parameter_protocol_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>)
add_test(NAME device_parameter_protocol_tests
    COMMAND device_parameter_protocol_tests)

add_executable(device_parameter_store_tests
    device_parameter_store_tests.c)
target_link_libraries(device_parameter_store_tests PRIVATE
    remotebsp_device_params)
target_compile_options(device_parameter_store_tests PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>)
add_test(NAME device_parameter_store_tests
    COMMAND device_parameter_store_tests)

add_executable(device_parameter_remote_tests
    device_parameter_remote_tests.cpp)
target_link_libraries(device_parameter_remote_tests PRIVATE
    remotebsp_remote_core)
target_compile_options(device_parameter_remote_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>)
add_test(NAME device_parameter_remote_tests
    COMMAND device_parameter_remote_tests)

add_executable(motion_protocol_tests motion_protocol_tests.cpp)
target_link_libraries(motion_protocol_tests PRIVATE remotebsp_protocol)
target_compile_options(motion_protocol_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
)
add_test(NAME motion_protocol_tests COMMAND motion_protocol_tests)

add_executable(waveform_protocol_tests waveform_protocol_tests.cpp)
target_link_libraries(waveform_protocol_tests PRIVATE remotebsp_protocol)
target_compile_options(waveform_protocol_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
)
add_test(NAME waveform_protocol_tests COMMAND waveform_protocol_tests)

add_executable(fragmentation_tests fragmentation_tests.cpp)
target_link_libraries(fragmentation_tests PRIVATE remotebsp_protocol)

target_compile_options(fragmentation_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
)

add_test(NAME fragmentation_tests COMMAND fragmentation_tests)

add_executable(can_frame_tests can_frame_tests.cpp)
target_link_libraries(can_frame_tests PRIVATE remotebsp_transport)

target_compile_options(can_frame_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
)

add_test(NAME can_frame_tests COMMAND can_frame_tests)

add_executable(usb_frame_tests usb_frame_tests.cpp)
target_link_libraries(usb_frame_tests PRIVATE remotebsp_transport)
target_compile_options(usb_frame_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
)
add_test(NAME usb_frame_tests COMMAND usb_frame_tests)

add_test(
    NAME e2e_usb_mock_tests
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/e2e_vcan.sh
            $<TARGET_FILE:mock_mcu>
            $<TARGET_FILE:toolbusd>
            $<TARGET_FILE:remote-cli>
            $<TARGET_FILE:client_api_tests>
            unused
            usb-mock
)
set_tests_properties(e2e_usb_mock_tests PROPERTIES RUN_SERIAL TRUE)

add_executable(socketcan_tests socketcan_tests.cpp)
target_link_libraries(socketcan_tests PRIVATE remotebsp_transport)

target_compile_options(socketcan_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
)

set(REMOTEBSP_VCAN_INTERFACE "" CACHE STRING
    "用于 SocketCAN 集成测试的 vcan 接口；为空时不注册该测试")
if(REMOTEBSP_VCAN_INTERFACE)
    add_test(
        NAME socketcan_tests
        COMMAND socketcan_tests ${REMOTEBSP_VCAN_INTERFACE}
    )

    add_test(
        NAME e2e_classical_tests
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/e2e_vcan.sh
                $<TARGET_FILE:mock_mcu>
                $<TARGET_FILE:toolbusd>
                $<TARGET_FILE:remote-cli>
                $<TARGET_FILE:client_api_tests>
                ${REMOTEBSP_VCAN_INTERFACE}
                classical
    )
    add_test(
        NAME e2e_fd_tests
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/e2e_vcan.sh
                $<TARGET_FILE:mock_mcu>
                $<TARGET_FILE:toolbusd>
                $<TARGET_FILE:remote-cli>
                $<TARGET_FILE:client_api_tests>
                ${REMOTEBSP_VCAN_INTERFACE}
                fd
    )
    add_test(
        NAME multi_node_classical_tests
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/multi_node_e2e.sh
                $<TARGET_FILE:mock_mcu>
                $<TARGET_FILE:toolbusd>
                $<TARGET_FILE:remote-cli>
                ${REMOTEBSP_VCAN_INTERFACE}
                classical
    )
    add_test(
        NAME multi_node_fd_tests
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/multi_node_e2e.sh
                $<TARGET_FILE:mock_mcu>
                $<TARGET_FILE:toolbusd>
                $<TARGET_FILE:remote-cli>
                ${REMOTEBSP_VCAN_INTERFACE}
                fd
    )
    add_test(
        NAME traffic_admission_e2e_tests
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/traffic_admission_e2e.sh
                $<TARGET_FILE:mock_mcu>
                $<TARGET_FILE:toolbusd>
                $<TARGET_FILE:remote-cli>
                ${REMOTEBSP_VCAN_INTERFACE}
    )
    set_tests_properties(
        socketcan_tests e2e_classical_tests e2e_fd_tests
        multi_node_classical_tests multi_node_fd_tests
        traffic_admission_e2e_tests
        PROPERTIES RUN_SERIAL TRUE
    )
endif()

add_executable(remote_core_tests remote_core_tests.cpp)
target_link_libraries(remote_core_tests PRIVATE remotebsp_remote_core)

target_compile_options(remote_core_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
)

add_test(NAME remote_core_tests COMMAND remote_core_tests)

add_executable(mock_node_tests mock_node_tests.cpp)
target_link_libraries(mock_node_tests PRIVATE remotebsp_remote_core)

target_compile_options(mock_node_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
)

add_test(NAME mock_node_tests COMMAND mock_node_tests)

add_executable(request_manager_tests request_manager_tests.cpp)
target_link_libraries(request_manager_tests PRIVATE remotebsp_request_manager)

target_compile_options(request_manager_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
)

add_test(NAME request_manager_tests COMMAND request_manager_tests)

add_executable(traffic_control_tests traffic_control_tests.cpp)
target_link_libraries(traffic_control_tests PRIVATE
    remotebsp_request_manager
)
target_compile_options(traffic_control_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
)
add_test(NAME traffic_control_tests COMMAND traffic_control_tests)

add_executable(discovery_tests discovery_tests.cpp)
target_link_libraries(discovery_tests PRIVATE
    remotebsp_remote_core
    remotebsp_request_manager
)

target_compile_options(discovery_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
)

add_test(NAME discovery_tests COMMAND discovery_tests)

add_executable(gpio_tests gpio_tests.cpp)
target_link_libraries(gpio_tests PRIVATE remotebsp_remote_core)

target_compile_options(gpio_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
)

add_test(NAME gpio_tests COMMAND gpio_tests)

add_executable(waveform_tests waveform_tests.cpp)
target_link_libraries(waveform_tests PRIVATE remotebsp_remote_core)
target_compile_options(waveform_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
)
add_test(NAME waveform_tests COMMAND waveform_tests)

add_executable(uart_tests uart_tests.cpp)
target_link_libraries(uart_tests PRIVATE remotebsp_remote_core)

target_compile_options(uart_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
)

add_test(NAME uart_tests COMMAND uart_tests)

add_executable(tmc2209_tests tmc2209_tests.cpp)
target_link_libraries(tmc2209_tests PRIVATE remotebsp)
target_compile_options(tmc2209_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
)
add_test(NAME tmc2209_tests COMMAND tmc2209_tests)

add_executable(resource_tests resource_tests.cpp)
target_link_libraries(resource_tests PRIVATE remotebsp_remote_core)

target_compile_options(resource_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
)

add_test(NAME resource_tests COMMAND resource_tests)

add_executable(resource_lease_tests resource_lease_tests.cpp)
target_link_libraries(resource_lease_tests PRIVATE remotebsp_remote_core)

target_compile_options(resource_lease_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
)

add_test(NAME resource_lease_tests COMMAND resource_lease_tests)

add_executable(board_manifest_tests board_manifest_tests.cpp)
target_link_libraries(board_manifest_tests PRIVATE remotebsp_remote_core)
target_compile_definitions(board_manifest_tests PRIVATE
    TEST_BOARD_MANIFEST=\"${CMAKE_SOURCE_DIR}/boards/mock-generic-v1.json\"
    TEST_FLY_D5_MANIFEST=\"${CMAKE_SOURCE_DIR}/boards/mellow-fly-d5-v1.json\"
    TEST_FAULT_SCENARIO=\"${CMAKE_CURRENT_SOURCE_DIR}/data/mock_fault_scenario.json\"
)
target_compile_options(board_manifest_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
)
add_test(NAME board_manifest_tests COMMAND board_manifest_tests)

add_executable(motion_executor_tests motion_executor_tests.cpp)
target_link_libraries(motion_executor_tests PRIVATE remotebsp_remote_core)
target_compile_definitions(motion_executor_tests PRIVATE
    TEST_BOARD_MANIFEST="${CMAKE_SOURCE_DIR}/boards/mock-generic-v1.json"
)
target_compile_options(motion_executor_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
)
add_test(NAME motion_executor_tests COMMAND motion_executor_tests)

add_executable(client_api_tests client_api_tests.cpp)
target_link_libraries(client_api_tests PRIVATE
    remotebsp
)

target_compile_options(client_api_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
)

add_executable(embedded_core_tests
    ../firmware/tests/core_tests.c
    ../firmware/src/core.c
    ../firmware/src/motion.c
    ../firmware/src/byte_ring.c
)
target_include_directories(embedded_core_tests PRIVATE
    ../firmware/include
    ../firmware/tests/config_all
)
target_compile_options(embedded_core_tests PRIVATE
    $<$<COMPILE_LANG_AND_ID:C,GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
)
add_test(NAME embedded_core_tests COMMAND embedded_core_tests)

add_executable(embedded_usb_link_tests
    ../firmware/tests/usb_link_tests.c
    ../firmware/src/usb_link.c
)
target_include_directories(embedded_usb_link_tests PRIVATE
    ../firmware/include
    ../firmware/tests/config
)
target_compile_options(embedded_usb_link_tests PRIVATE
    $<$<COMPILE_LANG_AND_ID:C,GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
)
add_test(NAME embedded_usb_link_tests COMMAND embedded_usb_link_tests)

add_executable(embedded_motion_tests
    ../firmware/tests/motion_queue_tests.c
    ../firmware/src/motion.c
)
target_include_directories(embedded_motion_tests PRIVATE
    ../firmware/include
    ../firmware/tests/config_motion
)
target_compile_options(embedded_motion_tests PRIVATE
    $<$<COMPILE_LANG_AND_ID:C,GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
)
add_test(NAME embedded_motion_tests COMMAND embedded_motion_tests)

add_executable(embedded_motion_core_compile_tests
    ../firmware/tests/motion_core_compile_tests.c
    ../firmware/src/core.c
    ../firmware/src/motion.c
)
target_include_directories(embedded_motion_core_compile_tests PRIVATE
    ../firmware/include
    ../firmware/tests/config_motion
)
target_compile_options(embedded_motion_core_compile_tests PRIVATE
    $<$<COMPILE_LANG_AND_ID:C,GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
)
add_test(NAME embedded_motion_core_compile_tests
         COMMAND embedded_motion_core_compile_tests)

add_executable(embedded_startup_gpio_tests
    ../firmware/tests/startup_gpio_tests.c
    ../firmware/src/startup_gpio.c
)
target_include_directories(embedded_startup_gpio_tests PRIVATE
    ../firmware/include
)
target_compile_options(embedded_startup_gpio_tests PRIVATE
    $<$<COMPILE_LANG_AND_ID:C,GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
)
add_test(NAME embedded_startup_gpio_tests
         COMMAND embedded_startup_gpio_tests)

add_executable(embedded_tmc2209_uart_backend_tests
    ../firmware/tests/soft_half_duplex_uart_tests.c
    ../firmware/src/soft_half_duplex_uart.c
    ../firmware/src/byte_ring.c
)
target_include_directories(embedded_tmc2209_uart_backend_tests PRIVATE
    ../firmware/include
    ../firmware/tests/config_soft_uart
)
target_compile_options(embedded_tmc2209_uart_backend_tests PRIVATE
    $<$<COMPILE_LANG_AND_ID:C,GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
)
add_test(NAME embedded_tmc2209_uart_backend_tests
         COMMAND embedded_tmc2209_uart_backend_tests)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(
    NAME firmware_config_tests
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/firmware/tests/test_genconfig.py"
)
