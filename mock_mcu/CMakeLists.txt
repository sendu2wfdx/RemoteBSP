add_library(remotebsp_remote_core
    src/board_manifest.cpp
    src/bus_bsp.cpp
    src/device_parameter_store.cpp
    src/gpio_bsp.cpp
    src/mock_node.cpp
    src/motion_executor.cpp
    src/remote_core.cpp
    src/time_sync_bsp.cpp
    src/uart_bsp.cpp
    src/waveform_bsp.cpp
    src/visual_state.cpp
)

target_include_directories(remotebsp_remote_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(remotebsp_remote_core
    PUBLIC
        remotebsp_protocol
        remotebsp_device_params
)

target_compile_options(remotebsp_remote_core PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
)

add_executable(mock_mcu
    src/main.cpp
)

target_link_libraries(mock_mcu PRIVATE
    remotebsp_remote_core
    remotebsp_transport
)

target_compile_definitions(mock_mcu PRIVATE
    REMOTEBSP_DEFAULT_MOCK_BOARD_MANIFEST=\"${CMAKE_SOURCE_DIR}/boards/mock-generic-v1.json\"
)

target_compile_options(mock_mcu PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
)
