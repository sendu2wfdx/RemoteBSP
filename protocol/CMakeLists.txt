add_library(remotebsp_protocol
    src/bus_stream.cpp
    src/crc32.cpp
    src/device_parameters.cpp
    src/discovery.cpp
    src/fragmentation.cpp
    src/motion.cpp
    src/motion_group.cpp
    src/packet.cpp
    src/resource.cpp
    src/time_sync.cpp
    src/waveform.cpp
)

target_include_directories(remotebsp_protocol
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(remotebsp_protocol PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
)
