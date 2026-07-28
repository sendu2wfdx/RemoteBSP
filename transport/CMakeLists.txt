if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "SocketCAN 传输层只能在 Linux 上构建")
endif()

add_library(remotebsp_transport
    src/can_frame_codec.cpp
    src/socketcan_transport.cpp
)

target_include_directories(remotebsp_transport
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(remotebsp_transport PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
)
