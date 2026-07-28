add_executable(protocol_tests protocol_tests.cpp)
target_link_libraries(protocol_tests PRIVATE remotebsp_protocol)

target_compile_options(protocol_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
)

add_test(NAME protocol_tests COMMAND protocol_tests)

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
    set_tests_properties(
        socketcan_tests e2e_classical_tests e2e_fd_tests
        multi_node_classical_tests multi_node_fd_tests
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

add_executable(uart_tests uart_tests.cpp)
target_link_libraries(uart_tests PRIVATE remotebsp_remote_core)

target_compile_options(uart_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
)

add_test(NAME uart_tests COMMAND uart_tests)

add_executable(resource_tests resource_tests.cpp)
target_link_libraries(resource_tests PRIVATE remotebsp_remote_core)

target_compile_options(resource_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
)

add_test(NAME resource_tests COMMAND resource_tests)

add_executable(client_api_tests client_api_tests.cpp)
target_link_libraries(client_api_tests PRIVATE remotebsp)

target_compile_options(client_api_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
)

add_executable(embedded_core_tests
    ../firmware/tests/core_tests.c
    ../firmware/src/core.c
    ../firmware/src/byte_ring.c
)
target_include_directories(embedded_core_tests PRIVATE
    ../firmware/include
    ../firmware/tests/config
)
target_compile_options(embedded_core_tests PRIVATE
    $<$<COMPILE_LANG_AND_ID:C,GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
)
add_test(NAME embedded_core_tests COMMAND embedded_core_tests)
