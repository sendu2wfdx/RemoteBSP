add_library(remotebsp_request_manager
    src/bus_runtime.cpp
    src/clock_model.cpp
    src/clock_sync_manager.cpp
    src/health_producer.cpp
    src/gpio_input_diagnostics_registry.cpp
    src/ipc.cpp
    src/motion_group_coordinator.cpp
    src/motion_group_service.cpp
    src/node_registry.cpp
    src/operation_ledger.cpp
    src/request_manager.cpp
    src/runtime_control.cpp
    src/traffic_control.cpp
)

add_library(remotebsp_link_recording_control
    src/link_recording.cpp
)
target_include_directories(remotebsp_link_recording_control PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(remotebsp_link_recording_control PUBLIC
    remotebsp_remote_core remotebsp_transport)
target_compile_options(remotebsp_link_recording_control PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>)

target_include_directories(remotebsp_request_manager
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(remotebsp_request_manager
    PUBLIC
        remotebsp_protocol
)

target_compile_options(remotebsp_request_manager PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
)

add_executable(toolbusd src/main.cpp)
target_link_libraries(toolbusd PRIVATE
    remotebsp_request_manager
    remotebsp_link_recording_control
    remotebsp_transport
)

target_compile_options(toolbusd PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
)
