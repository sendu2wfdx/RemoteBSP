# 测试以 assert 作为可执行断言。Release 默认定义 NDEBUG 会把断言和其参数一起
# 裁掉，既可能造成 -Werror 未使用变量，也会让测试静默空跑；所有配置都必须保留断言。
if(MSVC)
    add_compile_options(/UNDEBUG)
else()
    add_compile_options(-UNDEBUG)
endif()

# Python 进程测试散布在本文件各处；必须在首次注册前解析解释器，
# 避免干净构建把空变量退化为直接执行不可执行的 .py 文件。
find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_executable(protocol_tests protocol_tests.cpp)
target_link_libraries(protocol_tests PRIVATE remotebsp_protocol)

target_compile_options(protocol_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
)

add_test(NAME protocol_tests COMMAND protocol_tests)

add_executable(firmware_identity_tests firmware_identity_tests.cpp)
target_link_libraries(firmware_identity_tests PRIVATE
    remotebsp remotebsp_remote_core)
target_compile_options(firmware_identity_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>)
add_test(NAME firmware_identity_tests COMMAND firmware_identity_tests)

add_executable(time_sync_protocol_tests time_sync_protocol_tests.cpp)
target_link_libraries(time_sync_protocol_tests PRIVATE remotebsp_protocol)
target_compile_options(time_sync_protocol_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
)
add_test(NAME time_sync_protocol_tests COMMAND time_sync_protocol_tests)

add_executable(health_contract_tests health_contract_tests.cpp)
target_link_libraries(health_contract_tests PRIVATE remotebsp_protocol)
target_compile_options(health_contract_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
)
add_test(NAME health_contract_tests COMMAND health_contract_tests)

add_executable(toolbusd_health_producer_tests
    toolbusd_health_producer_tests.cpp)
target_link_libraries(toolbusd_health_producer_tests PRIVATE
    remotebsp_request_manager)
target_compile_options(toolbusd_health_producer_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
)
add_test(NAME toolbusd_health_producer_tests
    COMMAND toolbusd_health_producer_tests)

add_executable(toolbusd_health_ipc_tests toolbusd_health_ipc_tests.cpp)
target_link_libraries(toolbusd_health_ipc_tests PRIVATE
    remotebsp_request_manager)
target_compile_options(toolbusd_health_ipc_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
)
add_test(NAME toolbusd_health_ipc_tests COMMAND toolbusd_health_ipc_tests)

add_executable(runtime_operation_ipc_tests
    runtime_operation_ipc_tests.cpp)
target_link_libraries(runtime_operation_ipc_tests PRIVATE
    remotebsp
    remotebsp_request_manager)
target_compile_options(runtime_operation_ipc_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
)
add_test(NAME runtime_operation_ipc_tests
    COMMAND runtime_operation_ipc_tests)

# operation_id 是对外恢复选择器，CLI 仅接受规范小写十六进制。
add_test(NAME runtime_operation_cli_uppercase_id_rejected
    COMMAND bash
            ${CMAKE_CURRENT_SOURCE_DIR}/runtime_operation_cli_id_test.sh
            $<TARGET_FILE:remote-cli>)

add_executable(bus_stream_protocol_tests bus_stream_protocol_tests.cpp)
target_link_libraries(bus_stream_protocol_tests PRIVATE remotebsp_protocol)
target_compile_options(bus_stream_protocol_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
)
add_test(NAME bus_stream_protocol_tests COMMAND bus_stream_protocol_tests)

add_executable(adc_resource_tests adc_resource_tests.cpp)
target_link_libraries(adc_resource_tests PRIVATE remotebsp remotebsp_remote_core)
target_compile_options(adc_resource_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>)
add_test(NAME adc_resource_tests COMMAND adc_resource_tests)

add_executable(storage_resource_tests storage_resource_tests.cpp)
target_link_libraries(storage_resource_tests PRIVATE remotebsp remotebsp_remote_core)
target_compile_options(storage_resource_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>)
add_test(NAME storage_resource_tests COMMAND storage_resource_tests)
add_executable(timer_resource_tests timer_resource_tests.cpp)
target_link_libraries(timer_resource_tests PRIVATE remotebsp remotebsp_remote_core)
target_compile_options(timer_resource_tests PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>)
add_test(NAME timer_resource_tests COMMAND timer_resource_tests)

add_executable(stream_session_tests stream_session_tests.cpp)
target_link_libraries(stream_session_tests PRIVATE remotebsp_remote_core)
target_compile_options(stream_session_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
)
add_test(NAME stream_session_tests COMMAND stream_session_tests)

add_executable(stream_n2h_session_tests stream_n2h_session_tests.cpp)
target_link_libraries(stream_n2h_session_tests PRIVATE remotebsp_remote_core)
target_compile_options(stream_n2h_session_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
)
add_test(NAME stream_n2h_session_tests COMMAND stream_n2h_session_tests)

add_executable(stream_ipc_tests stream_ipc_tests.cpp)
target_link_libraries(stream_ipc_tests PRIVATE remotebsp_request_manager)
target_compile_options(stream_ipc_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>)
add_test(NAME stream_ipc_tests COMMAND stream_ipc_tests)

add_test(NAME stream_process_e2e_tests
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/stream_process_e2e.sh
            $<TARGET_FILE:toolbusd> $<TARGET_FILE:mock_mcu>
            $<TARGET_FILE:remote-cli>
            ${CMAKE_CURRENT_SOURCE_DIR}/data/mock_stream_n2h_board_v4.json
            ${CMAKE_CURRENT_SOURCE_DIR}/data/mock_stream_can_only_board_v4.json)
set_tests_properties(stream_process_e2e_tests PROPERTIES TIMEOUT 20)

add_executable(stream_credit_fault_client stream_credit_fault_client.cpp)
target_link_libraries(stream_credit_fault_client PRIVATE remotebsp)
target_compile_options(stream_credit_fault_client PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>)
add_test(NAME stream_credit_fault_process_tests
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/stream_credit_fault_process.sh
            $<TARGET_FILE:toolbusd> $<TARGET_FILE:mock_mcu>
            $<TARGET_FILE:stream_credit_fault_client>
            ${CMAKE_CURRENT_SOURCE_DIR}/data/mock_stream_n2h_board_v4.json)
set_tests_properties(stream_credit_fault_process_tests PROPERTIES TIMEOUT 20)

add_executable(bus_resource_tests bus_resource_tests.cpp)
target_link_libraries(bus_resource_tests PRIVATE remotebsp_remote_core)
target_compile_options(bus_resource_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
)
add_test(NAME bus_resource_tests COMMAND bus_resource_tests)

add_executable(bus_runtime_tests bus_runtime_tests.cpp)
target_link_libraries(bus_runtime_tests PRIVATE remotebsp_request_manager)
target_compile_options(bus_runtime_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
)
add_test(NAME bus_runtime_tests COMMAND bus_runtime_tests)

add_executable(bus_manifest_tests bus_manifest_tests.cpp)
target_link_libraries(bus_manifest_tests PRIVATE
    remotebsp_remote_core
    remotebsp_request_manager)
target_compile_definitions(bus_manifest_tests PRIVATE
    TEST_BUS_MANIFEST="${CMAKE_CURRENT_SOURCE_DIR}/data/mock_bus_board_v2.json"
    TEST_BUS_FAULT_SCENARIO="${CMAKE_CURRENT_SOURCE_DIR}/data/mock_bus_fault_scenario.json"
    TEST_STUDIO_BUS_MANIFEST="${CMAKE_CURRENT_SOURCE_DIR}/data/studio_bus_manifest_v2.json"
)
target_compile_options(bus_manifest_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
)
add_test(NAME bus_manifest_tests COMMAND bus_manifest_tests)

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

add_executable(boot_epoch_journal_tests
    boot_epoch_journal_tests.c)
target_link_libraries(boot_epoch_journal_tests PRIVATE
    remotebsp_device_params)
target_compile_options(boot_epoch_journal_tests PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>)
add_test(NAME boot_epoch_journal_tests
    COMMAND boot_epoch_journal_tests)

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

add_executable(motion_group_tests motion_group_tests.cpp)
target_link_libraries(motion_group_tests PRIVATE
    remotebsp_request_manager
    remotebsp_remote_core)
target_compile_options(motion_group_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
)
add_test(NAME motion_group_tests COMMAND motion_group_tests)

add_executable(motion_group_remote_core_tests
    motion_group_remote_core_tests.cpp)
target_link_libraries(motion_group_remote_core_tests PRIVATE
    remotebsp_remote_core)
target_compile_options(motion_group_remote_core_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
)
add_test(NAME motion_group_remote_core_tests
    COMMAND motion_group_remote_core_tests)

add_executable(motion_group_service_tests
    motion_group_service_tests.cpp)
target_link_libraries(motion_group_service_tests PRIVATE
    remotebsp_remote_core
    remotebsp_request_manager)
target_compile_options(motion_group_service_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
)
add_test(NAME motion_group_service_tests
    COMMAND motion_group_service_tests)

add_executable(motion_group_ipc_tests
    motion_group_ipc_tests.cpp)
target_link_libraries(motion_group_ipc_tests PRIVATE
    remotebsp_request_manager)
target_compile_options(motion_group_ipc_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
)
add_test(NAME motion_group_ipc_tests
    COMMAND motion_group_ipc_tests)

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

add_test(
    NAME pwm_lease_required_process_tests
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/pwm_lease_required_e2e.sh
            $<TARGET_FILE:mock_mcu>
            $<TARGET_FILE:toolbusd>
            $<TARGET_FILE:remote-cli>
            ${CMAKE_SOURCE_DIR}
)
set_tests_properties(pwm_lease_required_process_tests PROPERTIES RUN_SERIAL TRUE)
add_test(
    NAME pwm_acquire_quarantine_process_tests
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/pwm_lease_required_e2e.sh
            $<TARGET_FILE:mock_mcu> $<TARGET_FILE:toolbusd>
            $<TARGET_FILE:remote-cli> ${CMAKE_SOURCE_DIR} quarantine
)
set_tests_properties(pwm_acquire_quarantine_process_tests PROPERTIES RUN_SERIAL TRUE)

add_test(
    NAME timed_bitstream_runtime_process_tests
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/timed_bitstream_runtime_process_e2e.sh
            $<TARGET_FILE:mock_mcu>
            $<TARGET_FILE:toolbusd>
            $<TARGET_FILE:remote-cli>
            ${CMAKE_SOURCE_DIR}
)
set_tests_properties(timed_bitstream_runtime_process_tests PROPERTIES RUN_SERIAL TRUE)

add_test(
    NAME multi_node_usb_mock_tests
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/multi_node_e2e.sh
            $<TARGET_FILE:mock_mcu>
            $<TARGET_FILE:toolbusd>
            $<TARGET_FILE:remote-cli>
            unused
            usb-mock
)
set_tests_properties(multi_node_usb_mock_tests PROPERTIES RUN_SERIAL TRUE)

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
    remotebsp
    remotebsp_request_manager
)
target_compile_options(traffic_control_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
)
add_test(NAME traffic_control_tests COMMAND traffic_control_tests)

add_executable(clock_model_tests clock_model_tests.cpp)
target_link_libraries(clock_model_tests PRIVATE
    remotebsp_request_manager
)
target_compile_options(clock_model_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
)
add_test(NAME clock_model_tests COMMAND clock_model_tests)

add_executable(node_clock_registry_tests node_clock_registry_tests.cpp)
target_link_libraries(node_clock_registry_tests PRIVATE
    remotebsp_request_manager
)
target_compile_options(node_clock_registry_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
)
add_test(NAME node_clock_registry_tests COMMAND node_clock_registry_tests)

add_executable(time_sync_e2e_tests time_sync_e2e_tests.cpp)
target_link_libraries(time_sync_e2e_tests PRIVATE
    remotebsp_remote_core
    remotebsp_request_manager
)
target_compile_options(time_sync_e2e_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
)
add_test(NAME time_sync_e2e_tests COMMAND time_sync_e2e_tests)

add_executable(clock_sync_scheduler_tests
    clock_sync_scheduler_tests.cpp)
target_link_libraries(clock_sync_scheduler_tests PRIVATE
    remotebsp_request_manager
)
target_compile_options(clock_sync_scheduler_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
)
add_test(NAME clock_sync_scheduler_tests
    COMMAND clock_sync_scheduler_tests)

add_executable(runtime_snapshot_ipc_tests
    runtime_snapshot_ipc_tests.cpp)
target_link_libraries(runtime_snapshot_ipc_tests PRIVATE
    remotebsp_request_manager
)
target_compile_options(runtime_snapshot_ipc_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
)
add_test(NAME runtime_snapshot_ipc_tests
    COMMAND runtime_snapshot_ipc_tests)

add_executable(runtime_control_gate_tests
    runtime_control_gate_tests.cpp)
target_link_libraries(runtime_control_gate_tests PRIVATE
    remotebsp_request_manager
)
target_compile_options(runtime_control_gate_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
)
add_test(NAME runtime_control_gate_tests
    COMMAND runtime_control_gate_tests)

add_executable(operation_ledger_tests
    operation_ledger_tests.cpp)
target_link_libraries(operation_ledger_tests PRIVATE
    remotebsp_request_manager)
target_compile_options(operation_ledger_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
)
add_test(NAME operation_ledger_tests
    COMMAND operation_ledger_tests)

add_executable(toolbusd_operation_ledger_process_tests
    toolbusd_operation_ledger_process_tests.cpp)
target_compile_definitions(toolbusd PRIVATE REMOTEBSP_TEST_HOOKS=1)
target_link_libraries(toolbusd_operation_ledger_process_tests PRIVATE
    remotebsp
    remotebsp_request_manager)
add_dependencies(toolbusd_operation_ledger_process_tests
    toolbusd
    mock_mcu)
target_compile_definitions(toolbusd_operation_ledger_process_tests PRIVATE
    TEST_BUS_MANIFEST="${CMAKE_CURRENT_SOURCE_DIR}/data/mock_bus_board_v2.json")
target_compile_options(toolbusd_operation_ledger_process_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
)
add_test(NAME toolbusd_operation_ledger_process_tests
    COMMAND toolbusd_operation_ledger_process_tests
            $<TARGET_FILE:toolbusd>
            $<TARGET_FILE:mock_mcu>
            ${CMAKE_CURRENT_SOURCE_DIR}/data/mock_bus_board_v2.json)

add_test(NAME bus_reset_runtime_http_process_tests
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/bus_reset_runtime_http_process.py
            $<TARGET_FILE:toolbusd>
            $<TARGET_FILE:mock_mcu>
            $<TARGET_FILE:remote-cli>
            ${CMAKE_CURRENT_SOURCE_DIR}/data/mock_bus_board_v2.json)
set_tests_properties(bus_reset_runtime_http_process_tests PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    ENVIRONMENT "PYTHONPATH=${CMAKE_SOURCE_DIR}"
    TIMEOUT 30
    RUN_SERIAL TRUE)

add_executable(toolbusd_ipc_capacity_process_tests
    toolbusd_ipc_capacity_process_tests.cpp)
target_link_libraries(toolbusd_ipc_capacity_process_tests PRIVATE remotebsp)
add_dependencies(toolbusd_ipc_capacity_process_tests toolbusd)
target_compile_options(toolbusd_ipc_capacity_process_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>)
add_test(NAME toolbusd_ipc_capacity_process_tests
    COMMAND toolbusd_ipc_capacity_process_tests $<TARGET_FILE:toolbusd>)

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
    TEST_IDENTITY_V3_MANIFEST=\"${CMAKE_CURRENT_SOURCE_DIR}/data/mock_identity_board_v3.json\"
    TEST_IDENTITY_UNAVAILABLE_V3_MANIFEST=\"${CMAKE_CURRENT_SOURCE_DIR}/data/mock_identity_unavailable_board_v3.json\"
    TEST_IDENTITY_V1_MANIFEST=\"${CMAKE_CURRENT_SOURCE_DIR}/data/mock_identity_board_v1.json\"
)
target_compile_options(board_manifest_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
)
add_test(NAME board_manifest_tests COMMAND board_manifest_tests)

add_test(
    NAME firmware_identity_process_e2e_tests
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/firmware_identity_process_e2e.sh
            $<TARGET_FILE:mock_mcu>
            $<TARGET_FILE:toolbusd>
            $<TARGET_FILE:remote-cli>
            ${CMAKE_CURRENT_SOURCE_DIR}/data/mock_identity_board_v3.json
            ${CMAKE_CURRENT_SOURCE_DIR}/data/mock_identity_unavailable_board_v3.json
)
set_tests_properties(firmware_identity_process_e2e_tests PROPERTIES
    RUN_SERIAL TRUE)

add_test(
    NAME logical_recording_process_e2e_tests
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/logical_recording_process_e2e.sh
            $<TARGET_FILE:mock_mcu>
            $<TARGET_FILE:toolbusd>
            $<TARGET_FILE:remote-cli>
            ${CMAKE_CURRENT_SOURCE_DIR}/data/mock_identity_board_v3.json
)
set_tests_properties(logical_recording_process_e2e_tests PROPERTIES
    RUN_SERIAL TRUE)

add_executable(digital_twin_replay_tests digital_twin_replay_tests.cpp)
target_link_libraries(digital_twin_replay_tests PRIVATE remotebsp_remote_core)
target_compile_definitions(digital_twin_replay_tests PRIVATE
    TEST_BUS_MANIFEST="${CMAKE_CURRENT_SOURCE_DIR}/data/mock_bus_board_v2.json"
    TEST_DEFAULT_MANIFEST="${CMAKE_SOURCE_DIR}/boards/mock-generic-v1.json"
    TEST_REPLAY_OUTPUT="${CMAKE_CURRENT_BINARY_DIR}/digital-twin-replay-test.json"
)
target_compile_options(digital_twin_replay_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
)
add_test(NAME digital_twin_replay_tests COMMAND digital_twin_replay_tests)

add_executable(transport_replay_tests transport_replay_tests.cpp)
target_link_libraries(transport_replay_tests PRIVATE remotebsp_remote_core)
target_compile_definitions(transport_replay_tests PRIVATE
    TEST_TRANSPORT_REPLAY_OUTPUT="${CMAKE_CURRENT_BINARY_DIR}/transport-session-v1.txt"
)
target_compile_options(transport_replay_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
)
add_test(NAME transport_replay_tests COMMAND transport_replay_tests)

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
    ../firmware/src/motion_group.c
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

add_executable(embedded_bus_core_tests
    ../firmware/tests/bus_core_tests.c
    ../firmware/src/core.c
)
target_include_directories(embedded_bus_core_tests PRIVATE
    ../firmware/include
    ../firmware/tests/config_bus
)
target_compile_options(embedded_bus_core_tests PRIVATE
    $<$<COMPILE_LANG_AND_ID:C,GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
)
add_test(NAME embedded_bus_core_tests COMMAND embedded_bus_core_tests)

# 直接编译 G431 生产板级 BUS HAL，并以主机桩验证错误恢复与片选时序。
add_executable(g431_board_bus_hal_tests
    ../firmware/tests/g431_board_bus_tests.c
    ../firmware/boards/common/stm32_bus_hal.c
)
target_include_directories(g431_board_bus_hal_tests PRIVATE
    ../firmware/tests/g431_hal_stub
    ../firmware/boards/stm32g431cbu6
    ../firmware/include
    ../firmware/tests/config_bus
)
target_compile_definitions(g431_board_bus_hal_tests PRIVATE
    CONFIG_REMOTEBSP_BUS=1
    CONFIG_G431_BUS_I2C1_PB6_PB7=1
    CONFIG_G431_BUS_I2C1_DEVICE_ADDRESS=72
    CONFIG_G431_BUS_SPI1_PA5_PA6_PA7_CS_PA15=1
    CONFIG_G431_BUS_SPI2_PB13_PB14_PB15_CS_PB12=1
)
target_compile_options(g431_board_bus_hal_tests PRIVATE
    $<$<COMPILE_LANG_AND_ID:C,GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
)
add_test(NAME g431_board_bus_hal_tests COMMAND g431_board_bus_hal_tests)

# 同时开启总线与运动，防止共享通用资源命令和会话释放路径发生编译回归。
add_executable(embedded_bus_motion_core_compile_tests
    ../firmware/tests/motion_core_compile_tests.c
    ../firmware/src/core.c
    ../firmware/src/motion.c
    ../firmware/src/motion_group.c
)
target_include_directories(embedded_bus_motion_core_compile_tests PRIVATE
    ../firmware/include
    ../firmware/tests/config_motion
)
target_compile_definitions(embedded_bus_motion_core_compile_tests PRIVATE
    CONFIG_REMOTEBSP_BUS=1
    CONFIG_REMOTEBSP_BUS_RESOURCE_COUNT=1
    CONFIG_REMOTEBSP_BUS_MAX_TRANSFER_BYTES=128
    CONFIG_REMOTEBSP_BUS_MIN_TIMEOUT_US=100
    CONFIG_REMOTEBSP_BUS_MAX_TIMEOUT_US=100000
)
target_compile_options(embedded_bus_motion_core_compile_tests PRIVATE
    $<$<COMPILE_LANG_AND_ID:C,GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
)
add_test(NAME embedded_bus_motion_core_compile_tests
    COMMAND embedded_bus_motion_core_compile_tests)

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
    ../firmware/src/motion_group.c
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

add_executable(embedded_motion_group_tests
    ../firmware/tests/motion_group_tests.c
    ../firmware/src/motion.c
    ../firmware/src/motion_group.c
)
target_include_directories(embedded_motion_group_tests PRIVATE
    ../firmware/include
    ../firmware/tests/config_motion
)
target_compile_options(embedded_motion_group_tests PRIVATE
    $<$<COMPILE_LANG_AND_ID:C,GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
)
add_test(NAME embedded_motion_group_tests
         COMMAND embedded_motion_group_tests)

add_executable(embedded_startup_gpio_tests
    ../firmware/tests/startup_gpio_tests.c
    ../firmware/src/startup_gpio.c
    ../firmware/src/static_resources.c
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

add_executable(link_recording_control_tests link_recording_control_tests.cpp)
target_link_libraries(link_recording_control_tests PRIVATE
    remotebsp_link_recording_control)
target_compile_options(link_recording_control_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>)
add_test(NAME link_recording_control_tests COMMAND link_recording_control_tests)

add_test(
    NAME firmware_config_tests
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/firmware/tests/test_genconfig.py"
)
add_test(
    NAME firmware_flash_layout_tests
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/firmware/tests/test_flash_layout.py"
)
