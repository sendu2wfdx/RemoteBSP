add_library(remotebsp_request_manager
    src/clock_model.cpp
    src/ipc.cpp
    src/node_registry.cpp
    src/request_manager.cpp
    src/traffic_control.cpp
)

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
    remotebsp_transport
)

target_compile_options(toolbusd PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
)
