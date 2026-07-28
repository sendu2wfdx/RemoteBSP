add_library(remotebsp_remote_core
    src/gpio_bsp.cpp
    src/mock_node.cpp
    src/remote_core.cpp
    src/uart_bsp.cpp
)

target_include_directories(remotebsp_remote_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(remotebsp_remote_core
    PUBLIC
        remotebsp_protocol
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

target_compile_options(mock_mcu PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
)
